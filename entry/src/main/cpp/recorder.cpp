#include "recorder.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

#include <hilog/log.h>
#undef LOG_DOMAIN
#undef LOG_TAG
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

constexpr size_t kQueueLimit = 600;
constexpr auto kStartupGrace = std::chrono::microseconds(800000);

int64_t NowUs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000LL +
         static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

bool RelativePtsUs(int64_t timestampUs, int64_t baseUs, int64_t* ptsUs) {
  if (ptsUs == nullptr || timestampUs < baseUs) return false;
  if (baseUs < 0 && timestampUs > std::numeric_limits<int64_t>::max() + baseUs) {
    return false;
  }
  *ptsUs = timestampUs - baseUs;
  return true;
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
  for (auto& nal : nals) {
    while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
    if (nal.size < 2) continue;
    if (h265) {
      uint8_t t = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      if (t == 32 || t == 33 || t == 34) continue;
      if (t == 19 || t == 20) *hasKey = true;
      if (t == 35 || t == 39 || t == 40) continue;  // AUD / SEI
    } else {
      uint8_t t = static_cast<uint8_t>(nal.data[0] & 0x1F);
      if (t == 7 || t == 8) continue;
      if (t == 5) *hasKey = true;
      if (t == 9 || t == 6) continue;  // AUD / SEI
    }
    out.push_back(static_cast<uint8_t>((nal.size >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((nal.size >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((nal.size >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(nal.size & 0xFF));
    out.insert(out.end(), nal.data, nal.data + nal.size);
  }
  return out;
}

}  // namespace

StreamRecorder::~StreamRecorder() { Stop(); }

bool StreamRecorder::Enqueue(Item item) {
  std::lock_guard<std::mutex> lk(queueMu_);
  if (!accepting_.load()) return false;
  if (queue_.size() >= kQueueLimit) {
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                 "recorder queue full; dropping %{public}s sample",
                 item.audio ? "audio" : "video");
    return false;
  }
  queue_.push_back(std::move(item));
  queueCv_.notify_one();
  return true;
}

void StreamRecorder::ReportFatalError(const std::string& message) {
  accepting_.store(false);
  running_.store(false);
  queueCv_.notify_all();

  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "%{public}s", message.c_str());
  ErrorSink sink;
  {
    std::lock_guard<std::mutex> lk(errMu_);
    if (!errorReported_) {
      errorReported_ = true;
      sink = onError_;
    }
  }
  if (sink) {
    try {
      sink(message);
    } catch (...) {
      OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "recording error sink threw");
    }
  }
}

bool StreamRecorder::Start(const std::string& filePath, int rotation, ErrorSink onError) {
  if (accepting_.load() || running_.load()) return false;
  if (writerThread_.joinable()) {
    if (writerThread_.get_id() == std::this_thread::get_id()) return false;
    writerThread_.join();
  }
  DestroyMuxerAndClose();

  filePath_ = filePath;
  rotation_ = rotation;
  videoTrack_ = -1;
  audioTrack_ = -1;
  started_ = false;
  tsBaseUs_ = 0;
  tsBaseSet_ = false;
  lastVideoPtsUs_ = -1;
  lastAudioPtsUs_ = -1;
  {
    std::lock_guard<std::mutex> lk(queueMu_);
    queue_.clear();
  }
  {
    std::lock_guard<std::mutex> lk(errMu_);
    onError_ = std::move(onError);
    errorReported_ = false;
  }

  outputFd_ = open(filePath_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (outputFd_ < 0) {
    ReportFatalError("open output failed, errno=" + std::to_string(errno));
    return false;
  }

  accepting_.store(true);
  running_.store(true);
  try {
    writerThread_ = std::thread(&StreamRecorder::WriterLoop, this);
  } catch (...) {
    ReportFatalError("failed to start recorder writer thread");
    DestroyMuxerAndClose();
    return false;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "recording to %{public}s",
               filePath_.c_str());
  return true;
}

void StreamRecorder::WriterLoop() {
  std::vector<Item> startupPending;
  startupPending.reserve(kQueueLimit);
  VideoParams videoParams;
  uint64_t seenVideoParamsGeneration = std::numeric_limits<uint64_t>::max();
  AudioCfg audioCfg;
  bool audioSeen = false;
  bool startupWallSet = false;
  auto startupWall = std::chrono::steady_clock::time_point{};
  bool activeVideoIsH265 = false;
  bool fatal = false;

  auto refreshVideoParams = [&]() {
    std::lock_guard<std::mutex> lk(queueMu_);
    if (seenVideoParamsGeneration != videoParamsGeneration_) {
      videoParams = configuredVideoParams_;
      seenVideoParamsGeneration = videoParamsGeneration_;
    }
  };

  auto inspectStartupItem = [&](const Item& item) {
    if (item.audio) {
      AdtsInfo info{};
      if (ParseAdts(item.data.data(), item.data.size(), &info)) {
        audioCfg = {info.profileBits, info.sfIndex, info.channels, info.sampleRate};
        audioSeen = true;
      }
      return;
    }

    std::vector<NalView> nals = SplitAnnexB(item.data.data(), item.data.size());
    for (auto& nal : nals) {
      while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
      if (nal.size < 2) continue;
      uint8_t h264Type = static_cast<uint8_t>(nal.data[0] & 0x1F);
      uint8_t h265Type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      bool h265Param = (h265Type == 32 || h265Type == 33 || h265Type == 34) &&
                       nal.data[1] == 0x01;
      if (h265Param && !videoParams.h265) videoParams.h265 = true;
      if (h264Type == 7 && videoParams.h265) videoParams.h265 = false;
      if (videoParams.h265) {
        uint8_t type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
        if (type == 32) videoParams.vps.assign(nal.data, nal.data + nal.size);
        if (type == 33) videoParams.sps.assign(nal.data, nal.data + nal.size);
        if (type == 34) videoParams.pps.assign(nal.data, nal.data + nal.size);
      } else {
        uint8_t type = static_cast<uint8_t>(nal.data[0] & 0x1F);
        if (type == 7) videoParams.sps.assign(nal.data, nal.data + nal.size);
        if (type == 8) videoParams.pps.assign(nal.data, nal.data + nal.size);
      }
    }
    videoParams.ready = !videoParams.sps.empty() && !videoParams.pps.empty() &&
                        (!videoParams.h265 || !videoParams.vps.empty());
  };

  refreshVideoParams();
  while (!fatal) {
    std::deque<Item> local;
    bool stopping = false;
    {
      std::unique_lock<std::mutex> lk(queueMu_);
      auto ready = [&]() {
        return !queue_.empty() || !accepting_.load() ||
               seenVideoParamsGeneration != videoParamsGeneration_;
      };
      if (!ready()) {
        const auto graceDeadline = startupWall + kStartupGrace;
        if (!started_ && startupWallSet && !audioSeen &&
            std::chrono::steady_clock::now() < graceDeadline) {
          queueCv_.wait_until(lk, graceDeadline, ready);
        } else {
          queueCv_.wait(lk, ready);
        }
      }
      if (seenVideoParamsGeneration != videoParamsGeneration_) {
        videoParams = configuredVideoParams_;
        seenVideoParamsGeneration = videoParamsGeneration_;
      }
      local.swap(queue_);
      stopping = !accepting_.load();
    }

    if (!started_) {
      for (Item& item : local) {
        if (!startupWallSet) {
          startupWall = std::chrono::steady_clock::now();
          startupWallSet = true;
        }
        inspectStartupItem(item);
        if (startupPending.size() >= kQueueLimit) {
          OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                       "startup sample limit reached; dropping %{public}s sample",
                       item.audio ? "audio" : "video");
          continue;
        }
        startupPending.push_back(std::move(item));
      }

      const bool graceExpired =
          startupWallSet && std::chrono::steady_clock::now() - startupWall >= kStartupGrace;
      const bool settled = audioSeen || graceExpired || stopping;
      if (videoParams.ready && settled && !startupPending.empty()) {
        std::stable_sort(startupPending.begin(), startupPending.end(),
                         [](const Item& lhs, const Item& rhs) { return lhs.tsUs < rhs.tsUs; });
        tsBaseUs_ = startupPending.front().tsUs;
        tsBaseSet_ = true;
        if (!CreateMuxer(videoParams, audioSeen, audioCfg)) {
          fatal = true;
          break;
        }
        activeVideoIsH265 = videoParams.h265;
        for (const Item& item : startupPending) {
          if (!WriteItem(item, activeVideoIsH265)) {
            fatal = true;
            break;
          }
        }
        startupPending.clear();
        if (fatal) break;
      }

      if (!started_) {
        if (stopping) {
          if (!startupPending.empty()) {
            OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                         "discarding %{public}zu startup samples without video parameters",
                         startupPending.size());
            startupPending.clear();
          }
          break;
        }
        continue;
      }
    } else {
      for (const Item& item : local) {
        if (!WriteItem(item, activeVideoIsH265)) {
          fatal = true;
          break;
        }
      }
      if (fatal) break;
    }

    if (stopping) break;
  }

  startupPending.clear();
  {
    std::lock_guard<std::mutex> lk(queueMu_);
    queue_.clear();
  }
  accepting_.store(false);
  running_.store(false);
  DestroyMuxerAndClose();
}

bool StreamRecorder::CreateMuxer(const VideoParams& videoParams, bool audioSeen,
                                 const AudioCfg& audioCfg) {
  if (outputFd_ < 0) {
    ReportFatalError("output file is not open");
    return false;
  }

  muxer_ = OH_AVMuxer_Create(outputFd_, AV_OUTPUT_FORMAT_MPEG_4);
  if (muxer_ == nullptr) {
    ReportFatalError("OH_AVMuxer_Create failed");
    return false;
  }

  if (rotation_ != 0) {
    OH_AVErrCode err = OH_AVMuxer_SetRotation(muxer_, rotation_);
    if (err != AV_ERR_OK) {
      ReportFatalError("OH_AVMuxer_SetRotation failed, err=" +
                       std::to_string(static_cast<int>(err)));
      return false;
    }
  }

  if (videoParams.ready) {
    OH_AVFormat* fmt = OH_AVFormat_Create();
    if (fmt == nullptr) {
      ReportFatalError("create video track format failed");
      return false;
    }
    std::vector<uint8_t> rec =
        videoParams.h265
            ? HevcDecoderConfigRecord(videoParams.vps, videoParams.sps, videoParams.pps)
            : AvcDecoderConfigRecord(videoParams.sps, videoParams.pps);
    bool formatOk =
        OH_AVFormat_SetStringValue(fmt, OH_MD_KEY_CODEC_MIME,
                                   videoParams.h265 ? "video/hevc" : "video/avc") &&
        !rec.empty() &&
        OH_AVFormat_SetBuffer(fmt, OH_MD_KEY_CODEC_CONFIG, rec.data(), rec.size()) &&
        OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, videoParams.width) &&
        OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, videoParams.height);
    int32_t track = -1;
    OH_AVErrCode err = formatOk ? OH_AVMuxer_AddTrack(muxer_, &track, fmt)
                                : AV_ERR_INVALID_VAL;
    OH_AVFormat_Destroy(fmt);
    if (!formatOk || err != AV_ERR_OK || track < 0) {
      ReportFatalError("add video track failed, err=" +
                       std::to_string(static_cast<int>(err)));
      return false;
    }
    videoTrack_ = track;
  }

  if (audioSeen) {
    OH_AVFormat* fmt = OH_AVFormat_Create();
    if (fmt == nullptr) {
      ReportFatalError("create audio track format failed");
      return false;
    }
    std::vector<uint8_t> cfg =
        AacConfigBytes(audioCfg.profileBits, audioCfg.sfIndex, audioCfg.channels);
    bool formatOk =
        OH_AVFormat_SetStringValue(fmt, OH_MD_KEY_CODEC_MIME, "audio/mp4a-latm") &&
        !cfg.empty() &&
        OH_AVFormat_SetBuffer(fmt, OH_MD_KEY_CODEC_CONFIG, cfg.data(), cfg.size()) &&
        OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUD_SAMPLE_RATE, audioCfg.sampleRate) &&
        OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUD_CHANNEL_COUNT, audioCfg.channels);
    int32_t track = -1;
    OH_AVErrCode err = formatOk ? OH_AVMuxer_AddTrack(muxer_, &track, fmt)
                                : AV_ERR_INVALID_VAL;
    OH_AVFormat_Destroy(fmt);
    if (!formatOk || err != AV_ERR_OK || track < 0) {
      ReportFatalError("add audio track failed, err=" +
                       std::to_string(static_cast<int>(err)));
      return false;
    }
    audioTrack_ = track;
  }

  if (videoTrack_ < 0 && audioTrack_ < 0) {
    ReportFatalError("no muxer tracks available");
    return false;
  }

  OH_AVErrCode err = OH_AVMuxer_Start(muxer_);
  if (err != AV_ERR_OK) {
    ReportFatalError("OH_AVMuxer_Start failed, err=" +
                     std::to_string(static_cast<int>(err)));
    return false;
  }
  started_ = true;
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "muxer started (video=%{public}d audio=%{public}d h265=%{public}d)",
               videoTrack_, audioTrack_, videoParams.h265 ? 1 : 0);
  return true;
}

bool StreamRecorder::WriteItem(const Item& item, bool videoIsH265) {
  if (muxer_ == nullptr || !started_ || !tsBaseSet_) return true;

  OH_AVCodecBufferAttr attr{};
  std::vector<uint8_t> sample;
  int track = -1;
  int64_t* lastPtsUs = nullptr;
  if (item.audio) {
    if (audioTrack_ < 0) return true;
    AdtsInfo info{};
    if (!ParseAdts(item.data.data(), item.data.size(), &info)) return true;
    if (static_cast<size_t>(info.frameLen) > item.data.size()) return true;
    sample.assign(item.data.begin() + info.headerLen,
                  item.data.begin() + info.frameLen);
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    track = audioTrack_;
    lastPtsUs = &lastAudioPtsUs_;
  } else {
    if (videoTrack_ < 0) return true;
    std::vector<NalView> nals = SplitAnnexB(item.data.data(), item.data.size());
    bool hasKey = false;
    sample = LengthPrefixed(nals, videoIsH265, &hasKey);
    if (sample.empty()) return true;
    attr.flags = hasKey ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
    track = videoTrack_;
    lastPtsUs = &lastVideoPtsUs_;
  }

  int64_t ptsUs = 0;
  if (!RelativePtsUs(item.tsUs, tsBaseUs_, &ptsUs)) {
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                 "dropping %{public}s timestamp before base/overflow: ts=%{public}lld base=%{public}lld",
                 item.audio ? "audio" : "video", static_cast<long long>(item.tsUs),
                 static_cast<long long>(tsBaseUs_));
    return true;
  }
  if (*lastPtsUs >= 0 && ptsUs < *lastPtsUs) {
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                 "dropping regressing %{public}s PTS: pts=%{public}lld last=%{public}lld",
                 item.audio ? "audio" : "video", static_cast<long long>(ptsUs),
                 static_cast<long long>(*lastPtsUs));
    return true;
  }
  if (sample.empty() || sample.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    ReportFatalError("encoded sample size is invalid");
    return false;
  }

  attr.pts = ptsUs;
  attr.size = static_cast<int32_t>(sample.size());
  OH_AVBuffer* buffer = OH_AVBuffer_Create(attr.size);
  if (buffer == nullptr) {
    ReportFatalError("OH_AVBuffer_Create failed");
    return false;
  }
  uint8_t* addr = OH_AVBuffer_GetAddr(buffer);
  if (addr == nullptr) {
    OH_AVBuffer_Destroy(buffer);
    ReportFatalError("OH_AVBuffer_GetAddr failed");
    return false;
  }
  memcpy(addr, sample.data(), sample.size());
  OH_AVErrCode attrErr = OH_AVBuffer_SetBufferAttr(buffer, &attr);
  if (attrErr != AV_ERR_OK) {
    OH_AVBuffer_Destroy(buffer);
    ReportFatalError("OH_AVBuffer_SetBufferAttr failed, err=" +
                     std::to_string(static_cast<int>(attrErr)));
    return false;
  }

  OH_AVErrCode writeErr =
      OH_AVMuxer_WriteSampleBuffer(muxer_, static_cast<uint32_t>(track), buffer);
  OH_AVBuffer_Destroy(buffer);
  if (writeErr != AV_ERR_OK) {
    ReportFatalError("OH_AVMuxer_WriteSampleBuffer failed, err=" +
                     std::to_string(static_cast<int>(writeErr)));
    return false;
  }
  *lastPtsUs = ptsUs;
  return true;
}

void StreamRecorder::DestroyMuxerAndClose() {
  if (muxer_ != nullptr) {
    if (started_) {
      OH_AVErrCode stopErr = OH_AVMuxer_Stop(muxer_);
      if (stopErr != AV_ERR_OK) {
        ReportFatalError("OH_AVMuxer_Stop failed, err=" +
                         std::to_string(static_cast<int>(stopErr)));
      }
    }
    OH_AVErrCode destroyErr = OH_AVMuxer_Destroy(muxer_);
    muxer_ = nullptr;
    started_ = false;
    if (destroyErr != AV_ERR_OK) {
      ReportFatalError("OH_AVMuxer_Destroy failed, err=" +
                       std::to_string(static_cast<int>(destroyErr)));
    }
  }
  videoTrack_ = -1;
  audioTrack_ = -1;

  if (outputFd_ >= 0) {
    int fd = outputFd_;
    outputFd_ = -1;
    if (close(fd) != 0) {
      ReportFatalError("close output failed, errno=" + std::to_string(errno));
    }
  }
}

void StreamRecorder::SetVideoParams(bool h265, const std::vector<uint8_t>& vps,
                                    const std::vector<uint8_t>& sps,
                                    const std::vector<uint8_t>& pps, int width,
                                    int height) {
  std::lock_guard<std::mutex> lk(queueMu_);
  configuredVideoParams_.h265 = h265;
  configuredVideoParams_.vps = vps;
  configuredVideoParams_.sps = sps;
  configuredVideoParams_.pps = pps;
  configuredVideoParams_.width = width;
  configuredVideoParams_.height = height;
  configuredVideoParams_.ready = !sps.empty() && !pps.empty() && (!h265 || !vps.empty());
  ++videoParamsGeneration_;
  queueCv_.notify_all();
}

void StreamRecorder::FeedVideo(const uint8_t* data, size_t size, int64_t tsUs) {
  if (!accepting_.load() || data == nullptr || size < 5) return;
  Item item;
  item.audio = false;
  item.data.assign(data, data + size);
  item.tsUs = tsUs == -1 ? NowUs() : tsUs;
  Enqueue(std::move(item));
}

void StreamRecorder::FeedAudio(const uint8_t* adts, size_t size, int64_t tsUs) {
  if (!accepting_.load() || adts == nullptr || size < 8) return;
  Item item;
  item.audio = true;
  item.data.assign(adts, adts + size);
  item.tsUs = tsUs == -1 ? NowUs() : tsUs;
  Enqueue(std::move(item));
}

void StreamRecorder::Stop() {
  accepting_.store(false);
  running_.store(false);
  queueCv_.notify_all();

  if (writerThread_.joinable()) {
    if (writerThread_.get_id() == std::this_thread::get_id()) return;
    writerThread_.join();
  }
  DestroyMuxerAndClose();

  {
    std::lock_guard<std::mutex> lk(queueMu_);
    queue_.clear();
  }
  {
    std::lock_guard<std::mutex> lk(errMu_);
    onError_ = nullptr;
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "recording stopped: %{public}s",
               filePath_.c_str());
}

}  // namespace ipcam
