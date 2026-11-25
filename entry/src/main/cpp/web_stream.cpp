#include "web_stream.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <unistd.h>
#include <cstring>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>

#include "jpeg_encoder.h"
#include "param_sets.h"

#define LOG_DOMAIN 0xC010
#define LOG_TAG "WebStream"

namespace ipcam {
namespace {

constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
constexpr int kGridW = 32;
constexpr int kGridH = 18;
constexpr int kCellDelta = 25;      // per-cell luminance step counted as change
constexpr int kChangedCells = 10;   // changed cells needed to call it motion
// Android arms the detector 10 s after motion mode is switched on
// ("Motion Detection will delay 10 seconds to open") and clamps the timeout
// preference to 10–120 s.
constexpr int64_t kArmDelayUs = 10 * 1000000;
constexpr int kTimeoutMinSeconds = 10;
constexpr int kTimeoutMaxSeconds = 120;

uint64_t NowUs() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

bool IsH265ParamNal(const uint8_t* nal, size_t size) {
  if (size < 2) return false;
  if ((nal[0] & 0x1F) == 7 || (nal[0] & 0x1F) == 8) return false;
  uint8_t h265Type = static_cast<uint8_t>((nal[0] >> 1) & 0x3F);
  return (h265Type == 32 || h265Type == 33 || h265Type == 34) && nal[1] == 0x01;
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

void AppendNal(std::vector<uint8_t>& v, const uint8_t* d, size_t n) {
  v.insert(v.end(), kStartCode, kStartCode + 4);
  v.insert(v.end(), d, d + n);
}

// Cold-decode one keyframe: create decoder → push params+IDR+EOS → get NV12.
bool DecodeOneFrame(bool h265, const std::vector<uint8_t>& params,
                    const std::vector<uint8_t>& idr, int w, int h,
                    std::vector<uint8_t>* nv12Out) {
  struct Ctx {
    std::mutex mu;
    std::vector<std::vector<uint8_t>> inputs;
    size_t inputIdx = 0;
    bool gotFrame = false;
    bool failed = false;
    std::vector<uint8_t> frame;
    std::condition_variable cv;
    OH_AVCodec* dec = nullptr;
  };
  auto ctx = std::make_shared<Ctx>();
  ctx->inputs.push_back(params);
  ctx->inputs.push_back(idr);

  ctx->dec = OH_VideoDecoder_CreateByMime(h265 ? "video/hevc" : "video/avc");
  if (ctx->dec == nullptr) return false;

  OH_AVCodecCallback cb{};
  cb.onError = [](OH_AVCodec*, int32_t, void* ud) {
    auto* c = static_cast<Ctx*>(ud);
    std::lock_guard<std::mutex> lk(c->mu);
    c->failed = true;
    c->cv.notify_all();
  };
  cb.onStreamChanged = [](OH_AVCodec*, OH_AVFormat*, void*) {};
  cb.onNeedInputBuffer = [](OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* ud) {
    auto* c = static_cast<Ctx*>(ud);
    OH_AVCodecBufferAttr attr{};
    if (c->inputIdx < c->inputs.size()) {
      const auto& in = c->inputs[c->inputIdx++];
      memcpy(OH_AVBuffer_GetAddr(buffer), in.data(),
             std::min(in.size(), static_cast<size_t>(OH_AVBuffer_GetCapacity(buffer))));
      attr.size = static_cast<int32_t>(
          std::min(in.size(), static_cast<size_t>(OH_AVBuffer_GetCapacity(buffer))));
      attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    } else {
      attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
    }
    OH_AVBuffer_SetBufferAttr(buffer, &attr);
    OH_VideoDecoder_PushInputBuffer(c->dec, index);
  };
  cb.onNewOutputBuffer = [](OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* ud) {
    auto* c = static_cast<Ctx*>(ud);
    OH_AVCodecBufferAttr attr{};
    OH_AVBuffer_GetBufferAttr(buffer, &attr);
    if (!(attr.flags & AVCODEC_BUFFER_FLAGS_EOS) && attr.size > 0) {
      std::lock_guard<std::mutex> lk(c->mu);
      if (!c->gotFrame) {
        c->frame.assign(OH_AVBuffer_GetAddr(buffer),
                        OH_AVBuffer_GetAddr(buffer) + attr.size);
        c->gotFrame = true;
        c->cv.notify_all();
      }
    }
    OH_VideoDecoder_FreeOutputBuffer(c->dec, index);
  };
  OH_VideoDecoder_RegisterCallback(ctx->dec, cb, ctx.get());

  OH_AVFormat* fmt = OH_AVFormat_Create();
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, w);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, h);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
  bool ok = OH_VideoDecoder_Configure(ctx->dec, fmt) == AV_ERR_OK;
  OH_AVFormat_Destroy(fmt);
  if (!ok) {
    OH_VideoDecoder_Destroy(ctx->dec);
    return false;
  }
  if (OH_VideoDecoder_Start(ctx->dec) != AV_ERR_OK) {
    OH_VideoDecoder_Destroy(ctx->dec);
    return false;
  }

  // wait up to 3 s for output
  {
    std::unique_lock<std::mutex> lk(ctx->mu);
    ctx->cv.wait_for(lk, std::chrono::seconds(3), [&] { return ctx->gotFrame || ctx->failed; });
    ok = !ctx->failed && ctx->gotFrame && !ctx->frame.empty();
    if (ok) *nv12Out = std::move(ctx->frame);
  }
  OH_VideoDecoder_Stop(ctx->dec);
  OH_VideoDecoder_Destroy(ctx->dec);
  return ok && nv12Out->size() >= static_cast<size_t>(w) * h * 3 / 2;
}

}  // namespace

WebStream::~WebStream() {
  workerRunning_ = false;
  if (workerThread_.joinable()) workerThread_.join();
}

void WebStream::ResetVideoInput() {
  std::scoped_lock lk(stateMu_, motionMu_);
  InvalidateVideoLocked();
  processedArrival_ = arrival_.load(); // discard any pre-reset FeedVideo still parsing
  params_.clear();
  h265_ = false;
  paramsSet_ = false;
  // Never rewind idrGeneration_: the worker remembers the last decoded IDR.
  ++motionGeneration_;
  prevGrid_.clear();
  lastMotionUs_ = 0;
  motionLatch_ = false;
}

void WebStream::InvalidateVideoLocked() {
  ++inputGeneration_;
  lastIdr_.clear();
  lastJpeg_.clear();
  jpegArrival_ = 0;
  freshCv_.notify_all();
}

void WebStream::SetParams(bool h265, const std::vector<uint8_t>& paramsAnnexB, int width,
                          int height) {
  auto params = MergeParamSets({}, paramsAnnexB, h265);
  std::lock_guard<std::mutex> lk(stateMu_);
  if (paramsSet_ && (h265 != h265_ || width != width_ || height != height_ || params != params_)) {
    InvalidateVideoLocked();
    processedArrival_ = arrival_.load();
  }
  h265_ = h265;
  width_ = width;
  height_ = height;
  params_ = std::move(params);
  paramsSet_ = !params_.empty();
}

void WebStream::SetMotionTimeout(int seconds) {
  if (seconds < kTimeoutMinSeconds) seconds = kTimeoutMinSeconds;
  if (seconds > kTimeoutMaxSeconds) seconds = kTimeoutMaxSeconds;
  std::lock_guard<std::mutex> lk(motionMu_);
  motionHoldUs_ = static_cast<int64_t>(seconds) * 1000000;
}

bool WebStream::MotionActive() const {
  std::lock_guard<std::mutex> lk(motionMu_);
  const uint64_t now = NowUs();
  const int64_t last = lastMotionUs_.load();
  return motionEnabled_.load() && MotionArmed(now) && last > 0 &&
         now >= static_cast<uint64_t>(last) &&
         now - static_cast<uint64_t>(last) < static_cast<uint64_t>(motionHoldUs_);
}

void WebStream::SetMotionEnabled(bool enabled) {
  bool wasActive = false;
  {
    std::lock_guard<std::mutex> lk(motionMu_);
    motionGeneration_.fetch_add(1);
    motionEnabled_.store(enabled);
    wasActive = motionLatch_.exchange(false);
    prevGrid_.clear();
    lastMotionUs_.store(0);
    armAtUs_.store(enabled ? static_cast<int64_t>(NowUs()) + kArmDelayUs : 0);
    motionDemand_.store(enabled);
  }
  if (wasActive) {
    MotionSink sink;
    {
      std::lock_guard<std::mutex> lk(sinkMu_);
      sink = motionSink_;
    }
    if (sink) sink(false);
  }
  UpdateWorker();
}

void WebStream::SetMotionSink(MotionSink sink) {
  std::lock_guard<std::mutex> lk(sinkMu_);
  motionSink_ = std::move(sink);
}

bool WebStream::WorkerWanted() const {
  return jpegClients_.load() > 0 || motionDemand_.load();
}

void WebStream::UpdateWorker() {
  std::lock_guard<std::mutex> lk(workerMu_);
  if (WorkerWanted()) {
    if (!workerRunning_.exchange(true)) {
      try {
        workerThread_ = std::thread(&WebStream::DecodeLoop, this);
      } catch (const std::system_error&) {
        workerRunning_ = false;
      }
    }
    return;
  }
  {
    std::lock_guard<std::mutex> state(stateMu_);
    workerRunning_ = false;
    ++jpegSession_;
    freshCv_.notify_all();
  }
  if (workerThread_.joinable()) workerThread_.join();
}

void WebStream::IncClients() {
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    ++jpegClients_;
  }
  UpdateWorker();
}

void WebStream::DecClients() {
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    if (jpegClients_ > 0 && --jpegClients_ == 0) { // Motion demand may keep the decoder alive across this edge.
      ++jpegSession_;
      freshCv_.notify_all();
    }
  }
  UpdateWorker();
}

std::vector<uint8_t> WebStream::LatestJpeg() const {
  std::lock_guard<std::mutex> lk(stateMu_);
  return lastJpeg_;
}

std::vector<uint8_t> WebStream::WaitForFreshJpeg(const std::atomic<bool>& running,
                                                  std::chrono::milliseconds timeout) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + timeout;
  std::unique_lock<std::mutex> lk(stateMu_);
  const auto after = arrival_.load(), epoch = inputGeneration_, session = jpegSession_;
  while (running && workerRunning_ && jpegClients_ > 0 && epoch == inputGeneration_ &&
         session == jpegSession_) {
    const auto now = Clock::now();
    if (now >= deadline) break;
    if (jpegArrival_ > after) return lastJpeg_;
    // Publication and local cancellation notify immediately. HTTP Stop owns
    // the external flag (no WebStream lock); observe it even if input stalls.
    freshCv_.wait_until(lk, std::min(deadline, now + std::chrono::milliseconds(50)));
  }
  return {};
}

void WebStream::FeedVideo(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 5) return;
  const auto arrival = ++arrival_;
  std::vector<NalView> nals = SplitAnnexB(data, size);
  std::vector<uint8_t> idr;
  std::vector<uint8_t> frameParams;
  bool sawIdr = false;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    if (arrival <= processedArrival_) return;
    processedArrival_ = arrival;
    const bool previousCodec = h265_;
    for (auto& nal : nals) {
      while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
      if (nal.size < 2) continue;
      bool h265Param = IsH265ParamNal(nal.data, nal.size);
      uint8_t h264Type = nal.data[0] & 0x1F;
      uint8_t h265Type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      if (h265Param) {
        h265_ = true;
        AppendNal(frameParams, nal.data, nal.size);
        continue;
      }
      // HEVC IDR_N_LP (0x28) has the same low five bits as AVC PPS.
      if ((h264Type == 7 || h264Type == 8) && !(h265_ && h265Type == 20)) {
        h265_ = false;
        AppendNal(frameParams, nal.data, nal.size);
        continue;
      }
      if ((h265_ && (h265Type == 19 || h265Type == 20 || h265Type == 21)) ||
          (!h265_ && h264Type == 5)) {
        // Accumulate every slice NAL of this access unit — an IDR can be
        // split across several slice NALs.
        if (!sawIdr) idr.clear();
        AppendNal(idr, nal.data, nal.size);
        sawIdr = true;
      }
    }
    if (!frameParams.empty()) {
      // Replace per NAL type: encoders re-emit the sets on every keyframe (so
      // appending would grow without bound), and a buffer may carry only some.
      if (previousCodec != h265_) params_.clear();
      auto merged = MergeParamSets(params_, frameParams, h265_);
      if (paramsSet_ && (previousCodec != h265_ || merged != params_)) InvalidateVideoLocked();
      params_ = std::move(merged);
      paramsSet_ = !params_.empty();
    }
    if (sawIdr) {
      lastIdr_ = std::move(idr);
      idrGeneration_ = arrival;
    }
  }
}

void WebStream::DecodeLoop() {
  while (workerRunning_.load()) {
    std::vector<uint8_t> params;
    std::vector<uint8_t> idr;
    bool h265;
    int w, h;
    uint64_t gen, inputGeneration, jpegSession, motionGeneration;
    {
      std::lock_guard<std::mutex> lk(stateMu_);
      params = params_;
      idr = lastIdr_;
      h265 = h265_.load();
      w = width_.load();
      h = height_.load();
      gen = idrGeneration_.load();
      inputGeneration = inputGeneration_;
      jpegSession = jpegSession_;
      motionGeneration = motionGeneration_.load();
    }
    if (params.empty() || idr.empty() || w <= 0 || h <= 0) {
      usleep(500 * 1000);
      continue;
    }
    // The cold decoder's input is the cached keyframe, so re-decoding before a
    // new keyframe arrives would only reproduce the same picture: skip it and
    // keep the last JPEG/motion sample instead.
    if (gen != decodedIdrGeneration_) {
      std::vector<uint8_t> nv12;
      if (DecodeOneFrame(h265, params, idr, w, h, &nv12) &&
          nv12.size() >= static_cast<size_t>(w) * h * 3 / 2) {
        decodedIdrGeneration_ = gen;
        // JPEG only feeds the web console; motion detection reads the NV12
        // buffer directly, so skip the encode when nobody is watching.
        if (jpegClients_.load() > 0) {
          auto jpg = JpegEncoder::EncodeNV12(nv12.data(), w, h);
          if (!jpg.empty()) {
            std::lock_guard<std::mutex> lk(stateMu_);
            if (inputGeneration == inputGeneration_ && jpegSession == jpegSession_) {
              lastJpeg_ = std::move(jpg);
              jpegArrival_ = gen;
              freshCv_.notify_all();
            }
          }
        }
        CheckMotion(nv12.data(), w, h, motionGeneration);
      }
    }
    UpdateMotionLatch();
    usleep(500 * 1000);  // ~2 fps
  }
}

bool WebStream::MotionArmed(uint64_t now) const {
  const int64_t armAt = armAtUs_.load();
  return armAt != 0 && static_cast<int64_t>(now) >= armAt;
}

void WebStream::UpdateMotionLatch() {
  bool active = false;
  {
    std::lock_guard<std::mutex> lk(motionMu_);
    if (!motionEnabled_.load()) return;
    const uint64_t now = NowUs();
    const int64_t last = lastMotionUs_.load();
    active = MotionArmed(now) && last > 0 && now >= static_cast<uint64_t>(last) &&
             now - static_cast<uint64_t>(last) < static_cast<uint64_t>(motionHoldUs_);
    if (active == motionLatch_.exchange(active)) return;
  }
  MotionSink sink;
  {
    std::lock_guard<std::mutex> lk(sinkMu_);
    sink = motionSink_;
  }
  if (sink) sink(active);
}

void WebStream::CheckMotion(const uint8_t* nv12, int w, int h, uint64_t generation) {
  if (!motionEnabled_.load()) return;
  // Sample each cell as a 3x3 average of the Y plane. A single tap per cell
  // would flag sensor noise on one pixel; averaging keeps the 32x18 grid cheap
  // while staying stable (Android feeds its detector a luminance downscale to
  // at most 640 pixels wide for the same reason).
  uint8_t grid[kGridW * kGridH];
  const int stepX = std::max(1, w / (kGridW * 4));
  const int stepY = std::max(1, h / (kGridH * 4));
  for (int gy = 0; gy < kGridH; ++gy) {
    for (int gx = 0; gx < kGridW; ++gx) {
      const int cx = ((2 * gx + 1) * w) / (2 * kGridW);
      const int cy = ((2 * gy + 1) * h) / (2 * kGridH);
      int sum = 0;
      for (int sy = -1; sy <= 1; ++sy) {
        for (int sx = -1; sx <= 1; ++sx) {
          const int x = std::min(w - 1, std::max(0, cx + sx * stepX));
          const int y = std::min(h - 1, std::max(0, cy + sy * stepY));
          sum += nv12[static_cast<size_t>(y) * w + x];
        }
      }
      grid[gy * kGridW + gx] = static_cast<uint8_t>(sum / 9);
    }
  }
  bool motion = false;
  {
    std::lock_guard<std::mutex> lk(motionMu_);
    if (!motionEnabled_.load() || generation != motionGeneration_.load()) return;
    if (!prevGrid_.empty()) {
      int changed = 0;
      for (int i = 0; i < kGridW * kGridH; ++i) {
        int d = grid[i] - prevGrid_[i];
        if (d < -kCellDelta || d > kCellDelta) changed++;
      }
      if (changed >= kChangedCells) motion = true;
    }
    prevGrid_.assign(grid, grid + kGridW * kGridH);
    const uint64_t now = NowUs();
    if (MotionArmed(now) && motion) lastMotionUs_.store(static_cast<int64_t>(now));
  }
  UpdateMotionLatch();
}

}  // namespace ipcam
