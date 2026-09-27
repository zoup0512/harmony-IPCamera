#include "recorder.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmuxer.h>
#include <multimedia/player_framework/native_avmemory.h>

#include "codec_records.h"

#define LOG_DOMAIN 0xC010
#define LOG_TAG "StreamRecorder"

namespace ipcam {
namespace {

constexpr int kQueueLimit = 600;

uint64_t NowUs() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

struct NalView {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

std::vector<NalView> SplitAnnexB(const uint8_t* data, size_t size) {
  std::vector<NalView> nals;
  size_t i = 0;
  size_t nalStart = std::string::npos;
  while (i + 3 <= size) {
    if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
      if (nalStart != std::string::npos && i > nalStart) {
        nals.push_back({data + nalStart, i - nalStart});
      }
      i += 3;
      nalStart = i;
    } else {
      ++i;
    }
  }
  if (nalStart != std::string::npos && size > nalStart) {
    nals.push_back({data + nalStart, size - nalStart});
  }
  return nals;
}

// One MP4 sample per frame: 4-byte length prefixed NALUs (no param sets).
std::vector<uint8_t> LengthPrefixed(const std::vector<NalView>& nalsIn, bool h265,
                                    bool* hasKey) {
  std::vector<uint8_t> out;
  std::vector<NalView> nals = nalsIn;
  size_t hdrLen = h265 ? 2 : 1;
  for (auto& nal : nals) {
    while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
    if (nal.size < 2) continue;
    if (h265) {
      uint8_t t = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      if (t == 32 || t == 33 || t == 34) continue;
      if (t == 19 || t == 20) *hasKey = true;
    } else {
      uint8_t t = static_cast<uint8_t>(nal.data[0] & 0x1F);
      if (t == 7 || t == 8) continue;
      if (t == 5) *hasKey = true;
    }
    // keep NAL headers only (drop AUD and other non-slice overhead NALs)
    if (h265) {
      uint8_t t = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      if (t == 35 || t == 39 || t == 40) continue;  // AUD / SEI
    } else {
      uint8_t t = static_cast<uint8_t>(nal.data[0] & 0x1F);
      if (t == 9 || t == 6) continue;  // AUD / SEI
    }
    out.push_back(static_cast<uint8_t>((nal.size >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((nal.size >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((nal.size >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(nal.size & 0xFF));
    out.insert(out.end(), nal.data, nal.data + nal.size);
  }
  (void)hdrLen;
  return out;
}

}  // namespace

StreamRecorder::~StreamRecorder() { Stop(); }

void StreamRecorder::Enqueue(Item item) {
  std::lock_guard<std::mutex> lk(queueMu_);
  if (queue_.size() > kQueueLimit) {
    queue_.erase(queue_.begin(), queue_.begin() + 100);
  }
  queue_.push_back(std::move(item));
}

bool StreamRecorder::Start(const std::string& filePath, int rotation, ErrorSink onError) {
  if (running_.load()) return false;
  filePath_ = filePath;
  rotation_ = rotation;
  {
    std::lock_guard<std::mutex> lk(errMu_);
    onError_ = std::move(onError);
  }
  // open/truncate the output file up front so failures surface immediately
  int fd = open(filePath_.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "open %{public}s failed", filePath_.c_str());
    return false;
  }
  close(fd);
  running_ = true;
  writerThread_ = std::thread(&StreamRecorder::WriterLoop, this);
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "recording v2 to %{public}s",
               filePath_.c_str());
  return true;
}

void StreamRecorder::WriterLoop() {
  uint64_t firstItemUs = 0;
  while (running_.load()) {
    std::vector<Item> local;
    {
      std::lock_guard<std::mutex> lk(queueMu_);
      local.swap(queue_);
    }
    for (Item& it : local) {
      if (!tsBaseSet_) {
        tsBaseUs_ = it.tsUs;
        tsBaseSet_ = true;
      }
      if (it.audio) {
        AdtsInfo info{};
        if (ParseAdts(it.data.data(), it.data.size(), &info)) {
          audioCfg_ = {info.profileBits, info.sfIndex, info.channels, info.sampleRate};
          audioSeen_ = true;
        }
      } else {
        std::vector<NalView> nals = SplitAnnexB(it.data.data(), it.data.size());
        for (auto& nal : nals) {
          while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
          if (nal.size < 2) continue;
          // codec auto-detection from parameter-set anchors
          uint8_t h264Type = static_cast<uint8_t>(nal.data[0] & 0x1F);
          uint8_t h265Type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
          bool h265Param = (h265Type == 32 || h265Type == 33 || h265Type == 34) &&
                           nal.data[1] == 0x01;
          if (h265Param && !videoIsH265_) videoIsH265_ = true;
          if (h264Type == 7 && videoIsH265_) videoIsH265_ = false;
          if (videoIsH265_) {
            uint8_t t = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
            if (t == 32) { vps_.assign(nal.data, nal.data + nal.size); }
            if (t == 33) { sps_.assign(nal.data, nal.data + nal.size); }
            if (t == 34) { pps_.assign(nal.data, nal.data + nal.size); }
          } else {
            uint8_t t = static_cast<uint8_t>(nal.data[0] & 0x1F);
            if (t == 7) { sps_.assign(nal.data, nal.data + nal.size); }
            if (t == 8) { pps_.assign(nal.data, nal.data + nal.size); }
          }
        }
        if (!videoParamsReady_) {
          if (videoIsH265_) {
            videoParamsReady_ = !vps_.empty() && !sps_.empty() && !pps_.empty();
          } else {
            videoParamsReady_ = !sps_.empty() && !pps_.empty();
          }
        }
      }
      // Tracks can only be added before Start; give the audio config a short
      // grace period so both tracks land in one muxer when the mic is on.
      if (muxer_ == nullptr) {
        if (firstItemUs == 0) firstItemUs = it.tsUs;
        bool settled = audioSeen_ || (it.tsUs - firstItemUs) > 800000ULL;
        if (videoParamsReady_ && settled) {
          CreateMuxer();
        } else {
          Enqueue(std::move(it));
          continue;
        }
      }
      if (muxer_ == nullptr || !started_) continue;

      OH_AVBuffer* buf = nullptr;
      OH_AVCodecBufferAttr attr{};
      std::vector<uint8_t> sample;
      if (it.audio) {
        if (audioTrack_ < 0) continue;
        AdtsInfo info{};
        if (!ParseAdts(it.data.data(), it.data.size(), &info)) continue;
        sample.assign(it.data.begin() + info.headerLen, it.data.end());
        attr.pts = static_cast<int64_t>((it.tsUs - tsBaseUs_) / 1000ULL);
        attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
      } else {
        if (videoTrack_ < 0) continue;
        std::vector<NalView> nals = SplitAnnexB(it.data.data(), it.data.size());
        bool hasKey = false;
        sample = LengthPrefixed(nals, videoIsH265_, &hasKey);
        if (sample.empty()) continue;
        attr.pts = static_cast<int64_t>((it.tsUs - tsBaseUs_) / 1000ULL);
        attr.flags = hasKey ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
      }
      attr.size = static_cast<int32_t>(sample.size());
      buf = OH_AVBuffer_Create(static_cast<int>(sample.size()));
      if (buf == nullptr) continue;
      memcpy(OH_AVBuffer_GetAddr(buf), sample.data(), sample.size());
      OH_AVBuffer_SetBufferAttr(buf, &attr);
      OH_AVMuxer_WriteSampleBuffer(muxer_,
                                   static_cast<uint32_t>(it.audio ? audioTrack_ : videoTrack_),
                                   buf);
      OH_AVBuffer_Destroy(buf);
    }
    if (!running_.load()) break;
    usleep(20 * 1000);
  }
  if (muxer_ != nullptr) {
    OH_AVMuxer_Stop(muxer_);
    OH_AVMuxer_Destroy(muxer_);
    muxer_ = nullptr;
  }
}

void StreamRecorder::CreateMuxer() {
  int fd = open(filePath_.c_str(), O_WRONLY);
  if (fd < 0) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "muxer open failed errno=%{public}d", errno);
    return;
  }
  muxer_ = OH_AVMuxer_Create(fd, AV_OUTPUT_FORMAT_MPEG_4);
  if (muxer_ == nullptr) {
    close(fd);
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "OH_AVMuxer_Create failed");
    return;
  }
  if (rotation_ != 0) {
    OH_AVMuxer_SetRotation(muxer_, rotation_);
  }
  if (videoParamsReady_) {
    OH_AVFormat* fmt = OH_AVFormat_Create();
    OH_AVFormat_SetStringValue(fmt, OH_MD_KEY_CODEC_MIME,
                               videoIsH265_ ? "video/hevc" : "video/avc");
    std::vector<uint8_t> rec = videoIsH265_ ? HevcDecoderConfigRecord(vps_, sps_, pps_)
                                            : AvcDecoderConfigRecord(sps_, pps_);
    OH_AVFormat_SetBuffer(fmt, OH_MD_KEY_CODEC_CONFIG, rec.data(), static_cast<int>(rec.size()));
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, width_);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, height_);
    int32_t track = -1;
    OH_AVErrCode terr = OH_AVMuxer_AddTrack(muxer_, &track, fmt);
    videoTrack_ = track;
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                 "AddTrack video err=%{public}d track=%{public}d", terr, track);
    OH_AVFormat_Destroy(fmt);
  }
  if (audioSeen_) {
    OH_AVFormat* fmt = OH_AVFormat_Create();
    OH_AVFormat_SetStringValue(fmt, OH_MD_KEY_CODEC_MIME, "audio/mp4a-latm");
    std::vector<uint8_t> cfg = AacConfigBytes(audioCfg_.profileBits, audioCfg_.sfIndex,
                                              audioCfg_.channels);
    OH_AVFormat_SetBuffer(fmt, OH_MD_KEY_CODEC_CONFIG, cfg.data(), static_cast<int>(cfg.size()));
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUD_SAMPLE_RATE, audioCfg_.sampleRate);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUD_CHANNEL_COUNT, audioCfg_.channels);
    int32_t track = -1;
    OH_AVMuxer_AddTrack(muxer_, &track, fmt);
    audioTrack_ = track;
    OH_AVFormat_Destroy(fmt);
  }
  if (OH_AVMuxer_Start(muxer_) == AV_ERR_OK) {
    started_ = true;
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                 "muxer started (video=%{public}d audio=%{public}d h265=%{public}d)", videoTrack_,
                 audioTrack_, videoIsH265_ ? 1 : 0);
  } else {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "muxer start failed");
    OH_AVMuxer_Destroy(muxer_);
    muxer_ = nullptr;
  }
}

void StreamRecorder::SetVideoParams(bool h265, const std::vector<uint8_t>& vps,
                                    const std::vector<uint8_t>& sps,
                                    const std::vector<uint8_t>& pps, int width,
                                    int height) {
  std::lock_guard<std::mutex> lk(queueMu_);
  videoIsH265_ = h265;
  vps_ = vps;
  sps_ = sps;
  pps_ = pps;
  width_ = width;
  height_ = height;
  videoParamsReady_ = !sps_.empty() && (h265 ? !vps_.empty() : !pps_.empty());
}

void StreamRecorder::FeedVideo(const uint8_t* data, size_t size, uint64_t tsUs) {
  if (!running_.load() || data == nullptr || size < 5) return;
  Item it;
  it.audio = false;
  it.data.assign(data, data + size);
  it.tsUs = tsUs != 0 ? tsUs : NowUs();
  Enqueue(std::move(it));
}

void StreamRecorder::FeedAudio(const uint8_t* adts, size_t size, uint64_t tsUs) {
  if (!running_.load() || adts == nullptr || size < 8) return;
  Item it;
  it.audio = true;
  it.data.assign(adts, adts + size);
  it.tsUs = tsUs != 0 ? tsUs : NowUs();
  Enqueue(std::move(it));
}

void StreamRecorder::Stop() {
  if (!running_.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lk(errMu_);
    onError_ = nullptr;
  }
  if (writerThread_.joinable()) writerThread_.join();
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "recording stopped: %{public}s",
               filePath_.c_str());
}

}  // namespace ipcam
