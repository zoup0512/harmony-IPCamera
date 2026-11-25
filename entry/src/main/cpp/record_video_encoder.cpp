#include "record_video_encoder.h"

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <native_window/external_window.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace ipcam {
namespace {
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr size_t kMaxAU = 8u << 20, kQueueBytes = 16u << 20, kQueueItems = 256;
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void Check(OH_AVErrCode rc, const char* step) {
  if (rc != AV_ERR_OK) throw std::runtime_error(std::string(step) + " failed (" + std::to_string(rc) + ")");
}
auto Format(OH_AVFormat* f) {
  Require(f != nullptr, "codec format unavailable");
  return std::unique_ptr<OH_AVFormat, decltype(&OH_AVFormat_Destroy)>(f, OH_AVFormat_Destroy);
}
const char* Mime(bool hevc) { return hevc ? "video/hevc" : "video/avc"; }
int Type(const uint8_t* p, size_t n, bool hevc) {
  Require(n >= (hevc ? 2u : 1u) && !(p[0] & 0x80) && (!hevc || (p[1] & 7)), "invalid video NAL header");
  return hevc ? (p[0] >> 1) & 63 : p[0] & 31;
}
struct Picture { bool vcl = false, key = false, leading = false; };
// A codec buffer contains one Annex-B access unit or standalone parameter sets.
// Never guess the output codec by inspecting bytes from the source encoder.
Picture Inspect(const uint8_t* p, size_t n, RecordVideoEncoder::Source& params, bool immutable) {
  Picture pic;
  size_t begin = n, zeros = 0;
  auto visit = [&](size_t end) {
    if (begin == n) return;
    Require(begin < end, "empty video NAL");
    int t = Type(p + begin, end - begin, params.h265);
    auto* param = params.h265 ? (t == 32 ? &params.vps : t == 33 ? &params.sps : t == 34 ? &params.pps : nullptr)
                              : (t == 7 ? &params.sps : t == 8 ? &params.pps : nullptr);
    if (param) {
      Require(end - begin <= 65535, "oversize video parameter set");
      if (immutable) Require(param->size() == end - begin && std::equal(param->begin(), param->end(), p + begin),
                             "video parameters changed during recording");
      else param->assign(p + begin, p + end);
    }
    pic.vcl |= params.h265 ? t <= 31 : (t >= 1 && t <= 5);
    pic.key |= params.h265 ? (t >= 16 && t <= 21) : t == 5;
    pic.leading |= params.h265 && (t == 8 || t == 9);  // RASL after an open-GOP CRA
  };
  for (size_t i = 0; i < n; ++i) {
    if (p[i] == 0) { ++zeros; continue; }
    if (p[i] == 1 && zeros >= 2) {
      Require(begin != n || zeros == i, "video is not Annex-B");
      visit(i - zeros); begin = i + 1;
    } else Require(begin != n, "video is not Annex-B");
    zeros = 0;
  }
  Require(begin < n, "missing Annex-B video NAL");
  visit(n - zeros);
  return pic;
}
Bytes CachedConfig(const RecordVideoEncoder::Source& s) {
  Bytes b;
  auto add = [&](const Bytes& nal, int type) {
    Require(!nal.empty() && nal.size() <= 65535 && Type(nal.data(), nal.size(), s.h265) == type,
            "source video parameter cache not ready");
    b.insert(b.end(), {0,0,0,1}); b.insert(b.end(), nal.begin(), nal.end());
  };
  if (s.h265) add(s.vps, 32);
  add(s.sps, s.h265 ? 33 : 7); add(s.pps, s.h265 ? 34 : 8);
  return b;
}
}  // namespace

struct RecordVideoEncoder::Impl {
  struct Item { bool audio; Bytes bytes; int64_t pts; };
  OH_AVCodec *enc = nullptr, *dec = nullptr;
  OHNativeWindow* window = nullptr;
  bool encStarted = false, decStarted = false;
  Source input{}, output{};
  Bytes config;
  Sinks sinks;
  std::mutex mu;
  std::condition_variable cv;
  bool used = false, accepting = false, overflow = false;
  std::deque<Item> queue, audio;
  size_t queuedBytes = 0, audioBytes = 0;
  std::thread thread;
  // Worker-only state. No wall-clock timestamp substitution or independent video epoch.
  std::set<int64_t> submitted, rendered;
  bool inputKey = false, skipInputLeading = false, skipOutputLeading = false, checkedSize = false;
  bool decoderEos = false, encoderEos = false;
  int64_t first = -1, last = -1;
  Clock::time_point progress;

  void Release() {
    if (decStarted) OH_VideoDecoder_Stop(dec);
    if (dec) OH_VideoDecoder_Destroy(dec);
    if (encStarted) OH_VideoEncoder_Stop(enc);
    if (enc) OH_VideoEncoder_Destroy(enc);
    if (window) OH_NativeWindow_DestroyNativeWindow(window);
    dec = enc = nullptr; window = nullptr; decStarted = encStarted = false;
  }
  void Open(const Options& o) {
    Require(o.bitrate >= 250000 && o.bitrate <= 20000000 && o.gopMs >= 500 && o.gopMs <= 10000,
            "invalid recording video bitrate or GOP");
    Require(input.width > 0 && input.height > 0, "source video dimensions unavailable");
    config = CachedConfig(input);
    auto codec = [&](bool encode, bool hevc) {
      auto* cap = OH_AVCodec_GetCapabilityByCategory(Mime(hevc), encode, HARDWARE);
      Require(cap && OH_AVCapability_IsHardware(cap) && OH_AVCapability_GetName(cap),
              encode ? "hardware recording encoder unavailable" : "hardware recording decoder unavailable");
      Require(OH_AVCapability_IsVideoSizeSupported(cap, input.width, input.height), "recording dimensions unsupported");
      if (encode) {
        OH_AVRange range{};
        Check(OH_AVCapability_GetEncoderBitrateRange(cap, &range), "recording bitrate query");
        Require(range.minVal <= o.bitrate && o.bitrate <= range.maxVal, "recording bitrate unsupported");
        return OH_VideoEncoder_CreateByName(OH_AVCapability_GetName(cap));
      }
      return OH_VideoDecoder_CreateByName(OH_AVCapability_GetName(cap));
    };
    enc = codec(true, o.h265); Require(enc, "cannot allocate independent hardware video encoder");
    dec = codec(false, input.h265); Require(dec, "cannot allocate independent hardware video decoder");
    auto common = [&]() {
      auto f = Format(OH_AVFormat_Create());
      Require(OH_AVFormat_SetIntValue(f.get(), OH_MD_KEY_WIDTH, input.width) &&
              OH_AVFormat_SetIntValue(f.get(), OH_MD_KEY_HEIGHT, input.height) &&
              OH_AVFormat_SetIntValue(f.get(), OH_MD_KEY_ENABLE_SYNC_MODE, 1), "cannot configure sync video format");
      return f;
    };
    auto e = common();
    Require(OH_AVFormat_SetIntValue(e.get(), OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_SURFACE_FORMAT) &&
            OH_AVFormat_SetDoubleValue(e.get(), OH_MD_KEY_FRAME_RATE, 30.0) &&
            OH_AVFormat_SetLongValue(e.get(), OH_MD_KEY_BITRATE, o.bitrate) &&
            OH_AVFormat_SetIntValue(e.get(), OH_MD_KEY_I_FRAME_INTERVAL, o.gopMs) &&
            OH_AVFormat_SetIntValue(e.get(), OH_MD_KEY_VIDEO_ENCODER_ENABLE_B_FRAME, 0), "invalid recording encoder format");
    Check(OH_VideoEncoder_Configure(enc, e.get()), "recording encoder configure");
    Check(OH_VideoEncoder_GetSurface(enc, &window), "recording encoder input surface");
    Require(window, "recording encoder returned no input surface");
    auto d = common();
    Require(OH_AVFormat_SetBuffer(d.get(), OH_MD_KEY_CODEC_CONFIG, config.data(), config.size()), "decoder cache injection failed");
    Check(OH_VideoDecoder_Configure(dec, d.get()), "recording decoder configure");
    Check(OH_VideoDecoder_SetSurface(dec, window), "recording decoder output surface");
    Check(OH_VideoEncoder_Prepare(enc), "recording encoder prepare");
    Check(OH_VideoDecoder_Prepare(dec), "recording decoder prepare");
    Check(OH_VideoEncoder_Start(enc), "recording encoder start"); encStarted = true;
    Check(OH_VideoDecoder_Start(dec), "recording decoder start"); decStarted = true;
  }

  void Enqueue(bool isAudio, const uint8_t* p, size_t n, int64_t pts) {
    std::lock_guard<std::mutex> lock(mu);
    if (!accepting || (isAudio && !sinks.audio)) return;
    if (!p || !n || n > kMaxAU || pts < 0 || queue.size() >= kQueueItems || queuedBytes + n > kQueueBytes) {
      overflow = true; accepting = false;
    } else {
      try { queue.push_back({isAudio, Bytes(p, p+n), pts}); queuedBytes += n; }
      catch (const std::bad_alloc&) { overflow = true; accepting = false; }
    }
    cv.notify_one();
  }
  void FlushAudio() {
    while (!audio.empty() && audio.front().pts <= last) {
      auto& a = audio.front();
      if (a.pts >= first) sinks.audio(a.bytes.data(), a.bytes.size(), a.pts);
      audioBytes -= a.bytes.size(); audio.pop_front();
    }
  }
  void Encoded(const uint8_t* p, size_t n, int64_t pts) {
    auto pic = Inspect(p, n, output, first >= 0);
    if (!pic.vcl) return;
    // Only accept timestamps that crossed the private Surface from THIS decoder.
    // A driver that retimes frames is not safe for shared AAC: report it, don't guess.
    Require(rendered.erase(pts) == 1, "recording encoder did not preserve source PTS");
    if (first < 0) {
      if (!pic.key) return;
      Require(!output.sps.empty() && !output.pps.empty() && (!output.h265 || !output.vps.empty()),
              "recording encoder produced no codec config before random access");
      sinks.params(output); first = pts; skipOutputLeading = true;
    } else if (skipOutputLeading && pic.leading) return;
    else skipOutputLeading = false;
    Require(pts > last, "recording encoder video PTS regressed");
    sinks.video(p, n, pts); last = pts; progress = Clock::now(); FlushAudio();
  }
  void Pump() {
    for (int i = 0; !decoderEos && i < 16; ++i) {
      uint32_t index = 0;
      auto rc = OH_VideoDecoder_QueryOutputBuffer(dec, &index, 0);
      if (rc == AV_ERR_TRY_AGAIN_LATER) break;
      if (rc != AV_ERR_STREAM_CHANGED) Check(rc, "recording decoder output");
      if (!checkedSize || rc == AV_ERR_STREAM_CHANGED) {
        auto f = Format(OH_VideoDecoder_GetOutputDescription(dec)); int w = 0, h = 0;
        Require(OH_AVFormat_GetIntValue(f.get(), OH_MD_KEY_VIDEO_PIC_WIDTH, &w) &&
                OH_AVFormat_GetIntValue(f.get(), OH_MD_KEY_VIDEO_PIC_HEIGHT, &h) &&
                w == input.width && h == input.height, "recording source dimensions changed");
        checkedSize = true;
      }
      if (rc == AV_ERR_STREAM_CHANGED) continue;
      OH_AVCodecBufferAttr a{};
      auto* b = OH_VideoDecoder_GetOutputBuffer(dec, index); Require(b, "missing decoded buffer");
      Check(OH_AVBuffer_GetBufferAttr(b, &a), "decoded frame attributes");
      if (a.flags & AVCODEC_BUFFER_FLAGS_EOS) {
        Check(OH_VideoDecoder_FreeOutputBuffer(dec, index), "release decoder EOS");
        Check(OH_VideoEncoder_NotifyEndOfStream(enc), "recording encoder EOS"); decoderEos = true;
      } else {
        Require(submitted.erase(a.pts) == 1 && a.pts <= INT64_MAX/1000, "unexpected decoded frame PTS");
        Require(rendered.size() < 128 && rendered.insert(a.pts).second, "recording encoder stalled");
        Check(OH_VideoDecoder_RenderOutputBufferAtTime(dec, index, a.pts * 1000), "render to recording encoder");
      }
    }
    for (int i = 0; !encoderEos && i < 32; ++i) {
      uint32_t index = 0; auto rc = OH_VideoEncoder_QueryOutputBuffer(enc, &index, 0);
      if (rc == AV_ERR_TRY_AGAIN_LATER) break;
      if (rc == AV_ERR_STREAM_CHANGED) continue;  // parameter sets come from the encoded bytes
      Check(rc, "recording encoder output");
      auto* b = OH_VideoEncoder_GetOutputBuffer(enc, index); Require(b, "missing encoded buffer");
      OH_AVCodecBufferAttr a{}; Check(OH_AVBuffer_GetBufferAttr(b, &a), "encoded frame attributes");
      int cap = OH_AVBuffer_GetCapacity(b);
      Require(a.offset >= 0 && a.size >= 0 && a.size <= cap && a.offset <= cap - a.size &&
              static_cast<size_t>(a.size) <= kMaxAU, "invalid encoded buffer bounds");
      if (a.size) {
        auto* p = OH_AVBuffer_GetAddr(b); Require(p, "unreadable encoded frame");
        Encoded(p + a.offset, a.size, a.pts);
      }
      Check(OH_VideoEncoder_FreeOutputBuffer(enc, index), "release recording encoder output");
      encoderEos = (a.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0;
    }
  }
  void Push(const Bytes& data, int64_t pts, uint32_t flags) {
    const auto deadline = Clock::now() + 2s; uint32_t index = 0;
    for (;;) {
      auto rc = OH_VideoDecoder_QueryInputBuffer(dec, &index, 5000);
      if (rc != AV_ERR_TRY_AGAIN_LATER) { Check(rc, "recording decoder input"); break; }
      Pump(); Require(Clock::now() < deadline, "recording decoder input stalled");
    }
    auto* b = OH_VideoDecoder_GetInputBuffer(dec, index); Require(b, "missing decoder input");
    if (!data.empty()) {
      auto* p = OH_AVBuffer_GetAddr(b); auto cap = OH_AVBuffer_GetCapacity(b);
      Require(p && cap >= 0 && data.size() <= static_cast<size_t>(cap), "encoded source frame exceeds decoder capacity");
      std::memcpy(p, data.data(), data.size());
    }
    OH_AVCodecBufferAttr a{}; a.pts = pts; a.size = static_cast<int32_t>(data.size()); a.flags = flags;
    Check(OH_AVBuffer_SetBufferAttr(b, &a), "recording decoder input attributes");
    Check(OH_VideoDecoder_PushInputBuffer(dec, index), "recording decoder submit");
  }
  void Handle(Item item) {
    if (item.audio) {
      if (first >= 0 && item.pts < first) return;
      // AAC and camera callbacks can arrive out of order relative to each other.
      // Retain a bounded lookback BEFORE the first input IDR too; use the actual
      // encoded IDR timestamp, not callback arrival order, to trim that preroll.
      while (!inputKey && !audio.empty() && (audio.size() >= kQueueItems || audioBytes + item.bytes.size() > kQueueBytes)) {
        audioBytes -= audio.front().bytes.size(); audio.pop_front();
      }
      Require(audio.size() < kQueueItems && audioBytes + item.bytes.size() <= kQueueBytes, "recording audio alignment queue full");
      audioBytes += item.bytes.size(); audio.push_back(std::move(item)); FlushAudio(); return;
    }
    auto pic = Inspect(item.bytes.data(), item.bytes.size(), input, true);
    if (!pic.vcl) return;
    if (!inputKey) {
      if (!pic.key) return;
      while (!audio.empty() && audio.front().pts < item.pts) {
        audioBytes -= audio.front().bytes.size(); audio.pop_front();
      }
      Push(config, item.pts, AVCODEC_BUFFER_FLAGS_CODEC_DATA); inputKey = true; skipInputLeading = true;
    } else if (skipInputLeading && pic.leading) return;
    else skipInputLeading = false;
    Require(submitted.size() < 128 && submitted.insert(item.pts).second, "recording decoder stalled or duplicate PTS");
    Push(item.bytes, item.pts, pic.key ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : 0);
  }
  void Run() {
    std::string failure;
    try {
      progress = Clock::now();
      for (;;) {
        Item item{}; bool stop;
        {
          std::unique_lock<std::mutex> lock(mu);
          cv.wait_for(lock, 5ms, [&]{ return !queue.empty() || !accepting; });
          Require(!overflow, "invalid recording input or private codec queue full");
          stop = !accepting && queue.empty();
          if (!queue.empty()) { item = std::move(queue.front()); queue.pop_front(); queuedBytes -= item.bytes.size(); }
        }
        if (stop) break;
        if (!item.bytes.empty()) Handle(std::move(item));
        Pump();
        Require(!decoderEos && !encoderEos, "unexpected recording codec EOS");
        Require(Clock::now() - progress < 15s, "recording timed out waiting for random access or encoded video");
      }
      if (inputKey) {
        Push({}, 0, AVCODEC_BUFFER_FLAGS_EOS);
        auto deadline = Clock::now() + 2s;
        while (!encoderEos) {
          Pump(); Require(Clock::now() < deadline, "recording codec EOS timeout");
          if (!encoderEos) std::this_thread::sleep_for(1ms);
        }
        Require(first >= 0, "recording ended without encoded random access");
        Require(submitted.empty() && rendered.empty(), "recording codec lost frames before EOS");
      }
    } catch (const std::exception& e) { failure = e.what(); }
    {
      std::lock_guard<std::mutex> lock(mu); accepting = false; queue.clear(); queuedBytes = 0;
    }
    Release(); audio.clear();
    if (!failure.empty()) sinks.fatal(failure);
  }
};

RecordVideoEncoder::RecordVideoEncoder() : impl_(std::make_unique<Impl>()) {}
RecordVideoEncoder::~RecordVideoEncoder() { Stop(); }
bool RecordVideoEncoder::Configure(const Options& o, const Source& source, Sinks sinks, std::string& error) {
  auto& p = *impl_;
  if (p.used) { error = "recording encoder is single-use"; return false; }
  p.used = true;
  try {
    Require(sinks.params && sinks.video && sinks.fatal, "missing recording encoder sink");
    p.input = source; p.output = {o.h265, source.width, source.height, {}, {}, {}};
    p.sinks = std::move(sinks); p.Open(o);
    std::lock_guard<std::mutex> lock(p.mu); p.thread = std::thread([&p]{ p.Run(); }); p.accepting = true;
    error.clear(); return true;
  } catch (const std::exception& e) { p.Release(); error = e.what(); return false; }
}
void RecordVideoEncoder::FeedVideo(const uint8_t* data, size_t size, int64_t pts) { impl_->Enqueue(false, data, size, pts); }
void RecordVideoEncoder::FeedAudio(const uint8_t* data, size_t size, int64_t pts) { impl_->Enqueue(true, data, size, pts); }
void RecordVideoEncoder::Stop() {
  { std::lock_guard<std::mutex> lock(impl_->mu); impl_->accepting = false; impl_->cv.notify_one(); }
  if (impl_->thread.joinable()) impl_->thread.join();
}
}  // namespace ipcam
