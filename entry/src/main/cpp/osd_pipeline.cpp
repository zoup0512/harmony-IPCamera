#include "osd_pipeline.h"

#include <cstring>
#include <string>
#include <unistd.h>
#include <poll.h>

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#define LOG_DOMAIN 0xC010
#define LOG_TAG "OsdPipeline"

namespace {

constexpr uint64_t kCpuUsage =
    NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;

// BT.601 limited range. Good enough for OSD text/watermark color fidelity.
inline void RgbToYuv(int r, int g, int b, uint8_t* y, uint8_t* u, uint8_t* v) {
  *y = static_cast<uint8_t>((r * 66 + g * 129 + b * 25 + 128) >> 8) + 16;
  *u = static_cast<uint8_t>(((-r * 38) - (g * 74) + (b * 112) + 128) >> 8) + 128;
  *v = static_cast<uint8_t>(((r * 112) - (g * 94) - (b * 18) + 128) >> 8) + 128;
}

inline void YuvToRgb(int y, int u, int v, int* r, int* g, int* b) {
  int yy = (y - 16) * 0x2568;
  int uu = u - 128;
  int vv = v - 128;
  *r = (yy + 0x3343 * vv) >> 13;
  *g = (yy - 0x0c92 * vv - 0x1a1e * uu) >> 13;
  *b = (yy + 0x40cf * uu) >> 13;
  if (*r < 0) *r = 0; else if (*r > 255) *r = 255;
  if (*g < 0) *g = 0; else if (*g > 255) *g = 255;
  if (*b < 0) *b = 0; else if (*b > 255) *b = 255;
}

// Per-buffer-slot mapping cache. Camera buffers cycle through a small fixed
// set of queue slots; each slot is mapped exactly once via
// FromNativeWindowBuffer + Map. Per-frame Unmap/Unreference is fatal here:
// Unmap tore down memory the queue still reused (gralloc SetMetadata SEGV)
// and Unreference freed SurfaceBuffers still on the queue free list
// (PopFromFreeList NULL deref). Everything is released in ClearMapCache()
// before the consumer surface is destroyed.
struct CachedMap {
  OHNativeWindowBuffer* wb;
  OH_NativeBuffer* nb;
  BufferHandle* h;
  uint8_t* addr;
};

}  // namespace

namespace ipcam {

uint8_t* OsdPipeline::AddrFor(OHNativeWindowBuffer* wb, BufferHandle** hOut) {
  for (const auto& c : mapCache_) {
    if (c.wb == wb) {
      *hOut = c.h;
      return c.addr;
    }
  }
  CachedMap c{wb, nullptr, OH_NativeWindow_GetBufferHandleFromNative(wb), nullptr};
  if (c.h == nullptr) return nullptr;
  if (OH_NativeBuffer_FromNativeWindowBuffer(wb, &c.nb) != 0 || c.nb == nullptr) {
    return nullptr;
  }
  void* addr = nullptr;
  if (OH_NativeBuffer_Map(c.nb, &addr) != 0 || addr == nullptr) {
    OH_NativeBuffer_Unreference(c.nb);
    return nullptr;
  }
  c.addr = static_cast<uint8_t*>(addr);
  mapCache_.push_back(c);
  *hOut = c.h;
  return c.addr;
}

void OsdPipeline::ClearMapCache() {
  for (auto& c : mapCache_) {
    if (c.nb != nullptr) {
      OH_NativeBuffer_Unmap(c.nb);
      OH_NativeBuffer_Unreference(c.nb);
    }
  }
  mapCache_.clear();
}

OsdPipeline::~OsdPipeline() { Stop(); }

void OsdPipeline::OnFrameAvailable(void* context) {
  auto* self = static_cast<OsdPipeline*>(context);
  {
    std::lock_guard<std::mutex> lk(self->mu_);
    self->frameReady_ = true;
  }
  self->cv_.notify_one();
}

void OsdPipeline::WaitFence(int fd) {
  if (fd < 0) return;
  struct pollfd p{};
  p.fd = fd;
  p.events = POLLIN;
  poll(&p, 1, 100);
  close(fd);
}

void OsdPipeline::Fail(const char* what) {
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "%{public}s", what);
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(mu_);
    onError = onError_;
  }
  if (onError) onError(what);
}

bool OsdPipeline::Start(int outWidth, int outHeight, uint64_t* cameraSurfaceId,
                        FrameSink sink, ErrorSink onError) {
  outW_ = outWidth;
  outH_ = outHeight;
  {
    std::lock_guard<std::mutex> lk(mu_);
    sink_ = std::move(sink);
    onError_ = std::move(onError);
    quit_ = false;
    ready_ = false;
    initOk_ = false;
    cameraSurfaceId_ = 0;
  }
  nv12_.assign(static_cast<size_t>(outW_) * outH_ * 3 / 2, 0);

  // CPU-consumer surface. Created with OH_NativeImage_Create (same queue
  // flavor the GL path used — the camera produces into it stably);
  // OH_ConsumerSurface_Create + camera crashed in vendor gralloc metadata
  // code inside BufferQueue::ReuseBuffer on this device, and
  // OH_ConsumerSurface_SetDefaultUsage crashed the same path differently.
  // We never bind the texture; consumption is purely via
  // Acquire/ReleaseNativeWindowBuffer + OH_NativeBuffer_Map.
  image_ = OH_NativeImage_Create(0, 0);
  if (image_ == nullptr) {
    Fail("NativeImage_Create failed");
    return false;
  }
  OHNativeWindow* camWindow = OH_NativeImage_AcquireNativeWindow(image_);
  if (camWindow == nullptr ||
      OH_NativeImage_GetSurfaceId(image_, &cameraSurfaceId_) != 0) {
    Fail("consumer surface id failed");
    OH_NativeImage_Destroy(&image_);
    image_ = nullptr;
    return false;
  }
  OH_OnFrameAvailableListener listener{this, &OsdPipeline::OnFrameAvailable};
  OH_NativeImage_SetOnFrameAvailableListener(image_, listener);

  thread_ = std::thread(&OsdPipeline::Run, this);
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [this] { return ready_; });
  if (initOk_ && cameraSurfaceId != nullptr) {
    *cameraSurfaceId = cameraSurfaceId_;
  }
  return initOk_;
}

void OsdPipeline::Stop() {
  if (stopped_.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
    sink_ = nullptr;
    onError_ = nullptr;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  ClearMapCache();  // release mappings before the consumer surface dies
  if (image_ != nullptr) {
    OH_NativeImage_Destroy(&image_);
    image_ = nullptr;
  }
  stopped_ = false;
}

void OsdPipeline::Run() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    initOk_ = true;
    ready_ = true;
  }
  cv_.notify_all();
  for (;;) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return quit_ || frameReady_; });
    if (quit_) break;
    frameReady_ = false;
    lk.unlock();
    CompositeOne();
  }
}

void OsdPipeline::CompositeOne() {
  // Drain to the latest camera buffer (readLatest semantics).
  OHNativeWindowBuffer* latest = nullptr;
  int latestFence = -1;
  for (;;) {
    OHNativeWindowBuffer* b = nullptr;
    int f = -1;
    int ret = OH_NativeImage_AcquireNativeWindowBuffer(image_, &b, &f);
    if (ret != 0 || b == nullptr) break;
    if (latest != nullptr) {
      OH_NativeImage_ReleaseNativeWindowBuffer(image_, latest, -1);
      if (latestFence >= 0) close(latestFence);
    }
    latest = b;
    latestFence = f;
  }
  if (latest == nullptr) return;
  WaitFence(latestFence);
  BufferHandle* ch = nullptr;
  uint8_t* camAddr = AddrFor(latest, &ch);
  if (camAddr == nullptr || ch == nullptr) {
    if (frameCount_ < 5) {
      Fail("camera buffer map failed");
    }
    OH_NativeImage_ReleaseNativeWindowBuffer(image_, latest, -1);
    return;
  }
  bool nv21Cam = (ch->format == static_cast<int>(NATIVEBUFFER_PIXEL_FMT_YCRCB_420_SP));
  ++frameCount_;
  if (frameCount_ <= 2 || frameCount_ % 300 == 0) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                 "cam %{public}dx%{public}d stride=%{public}d fmt=%{public}d frame=%{public}d",
                 ch->width, ch->height, ch->stride, ch->format, frameCount_);
  }

  OsdSnapshot snap = GetOsd();
  renderer_.RefreshIfNeeded(snap, outW_, outH_);

  // 1) Copy the camera frame into the tightly packed NV12 target (output is
  //    always NV12; a camera NV21 source gets its UV pair swapped).
  const int camW = ch->width;
  const int camH = ch->height;
  const int camStride = ch->stride;
  const uint8_t* camY = camAddr;
  const uint8_t* camUv = camY + static_cast<size_t>(camStride) * camH;

  uint8_t* encY = nv12_.data();
  uint8_t* encUv = encY + static_cast<size_t>(outW_) * outH_;
  const int sameSize = (camW == outW_ && camH == outH_);
  for (int row = 0; row < outH_; ++row) {
    int sy = sameSize ? row : row * camH / outH_;
    const uint8_t* srcY = camY + static_cast<size_t>(camStride) * sy;
    uint8_t* dstY = encY + static_cast<size_t>(outW_) * row;
    if (sameSize) {
      memcpy(dstY, srcY, static_cast<size_t>(outW_));
    } else {
      for (int x = 0; x < outW_; ++x) dstY[x] = srcY[x * camW / outW_];
    }
  }
  for (int row = 0; row < (outH_ + 1) / 2; ++row) {
    int sy = sameSize ? row : row * ((camH + 1) / 2) / ((outH_ + 1) / 2);
    const uint8_t* srcUv = camUv + static_cast<size_t>(camStride) * sy;
    uint8_t* dstUv = encUv + static_cast<size_t>(outW_) * row;
    for (int x = 0; x < outW_; ++x) {
      int sx = x * camW / outW_;
      dstUv[x * 2] = nv21Cam ? srcUv[sx * 2 + 1] : srcUv[sx * 2];
      dstUv[x * 2 + 1] = nv21Cam ? srcUv[sx * 2] : srcUv[sx * 2 + 1];
    }
  }

  // 2) Burn the overlays into the NV12 copy.
  const OsdRenderer::Layer& text = renderer_.text();
  if (snap.enabled && !text.empty()) {
    OsdRenderer::Rect tr = renderer_.TextRect(snap, outW_, outH_);
    BlendRgbaIntoYuv(text.rgba.data(), text.width, text.height, true, tr.x, tr.y,
                     tr.w, tr.h, encY, encUv, outW_, outW_, outW_, outH_);
  }
  if (snap.wmEnabled && snap.wmPixels != nullptr && snap.wmWidth > 0) {
    OsdRenderer::Rect wr = renderer_.WatermarkRect(snap, outW_, outH_);
    if (wr.w > 0 && wr.h > 0) {
      EnsureWmScaled(wr.w, wr.h, snap);
      if (!wmScaled_.empty()) {
        BlendRgbaIntoYuv(wmScaled_.data(), wmScaledW_, wmScaledH_, false, wr.x,
                         wr.y, wr.w, wr.h, encY, encUv, outW_, outW_, outW_,
                         outH_);
      }
    }
  }

  FrameSink sink;
  {
    std::lock_guard<std::mutex> lk(mu_);
    sink = sink_;
  }
  if (sink) sink(nv12_.data(), nv12_.size());
  OH_NativeImage_ReleaseNativeWindowBuffer(image_, latest, -1);
}

void OsdPipeline::EnsureWmScaled(int dstW, int dstH, const OsdSnapshot& snap) {
  if (snap.wmVersion == wmScaledVersion_ && dstW == wmScaledW_ &&
      dstH == wmScaledH_) {
    return;
  }
  wmScaled_.assign(static_cast<size_t>(dstW) * dstH * 4, 0);
  wmScaledW_ = dstW;
  wmScaledH_ = dstH;
  wmScaledVersion_ = snap.wmVersion;
  const uint8_t* src = snap.wmPixels->data();
  for (int y = 0; y < dstH; ++y) {
    int sy = y * snap.wmHeight / dstH;
    for (int x = 0; x < dstW; ++x) {
      int sx = x * snap.wmWidth / dstW;
      const uint8_t* s = src + (static_cast<size_t>(sy) * snap.wmWidth + sx) * 4;
      uint8_t* d = wmScaled_.data() + (static_cast<size_t>(y) * dstW + x) * 4;
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = s[3];
    }
  }
}

// Alpha-blend an RGBA layer (srcW x srcH, scaled to dstW x dstH at dstX/dstY)
// directly into 4:2:0 semi-planar planes. Per-pixel Y is exact; chroma is
// computed from the 2x2 block's average blended RGB. Output is always NV12.
void OsdPipeline::BlendRgbaIntoYuv(const uint8_t* rgba, int srcW, int srcH,
                                   bool premul, int dstX, int dstY, int dstW,
                                   int dstH, uint8_t* yPlane, uint8_t* uvPlane,
                                   int yStride, int uvStride, int frameW,
                                   int frameH) {
  int x0 = dstX < 0 ? 0 : dstX;
  int y0 = dstY < 0 ? 0 : dstY;
  int x1 = dstX + dstW > frameW ? frameW : dstX + dstW;
  int y1 = dstY + dstH > frameH ? frameH : dstY + dstH;
  auto sample = [&](int px, int py, int* r, int* g, int* b, int* a) {
    int sx = (px - dstX) * srcW / dstW;
    int sy = (py - dstY) * srcH / dstH;
    const uint8_t* s = rgba + (static_cast<size_t>(sy) * srcW + sx) * 4;
    int alpha = s[3];
    if (premul && alpha > 0) {
      *r = s[0] * 255 / alpha;
      *g = s[1] * 255 / alpha;
      *b = s[2] * 255 / alpha;
    } else {
      *r = s[0];
      *g = s[1];
      *b = s[2];
    }
    *a = alpha;
  };
  for (int by = y0 & ~1; by < y1; by += 2) {
    for (int bx = x0 & ~1; bx < x1; bx += 2) {
      long sr = 0, sg = 0, sb = 0;
      int n = 0;
      for (int py = by; py < by + 2 && py < y1; ++py) {
        for (int px = bx; px < bx + 2 && px < x1; ++px) {
          if (px < x0) continue;
          uint8_t* yp = yPlane + static_cast<size_t>(yStride) * py + px;
          const uint8_t* uvp =
              uvPlane + static_cast<size_t>(uvStride) * (py / 2) + (px / 2) * 2;
          int u = uvp[0];
          int v = uvp[1];
          int r, g, b, a;
          sample(px, py, &r, &g, &b, &a);
          int dr, dg, db;
          YuvToRgb(*yp, u, v, &dr, &dg, &db);
          int outR = (r * a + dr * (255 - a) + 127) / 255;
          int outG = (g * a + dg * (255 - a) + 127) / 255;
          int outB = (b * a + db * (255 - a) + 127) / 255;
          uint8_t yy, uu, vv;
          RgbToYuv(outR, outG, outB, &yy, &uu, &vv);
          *yp = yy;
          sr += outR;
          sg += outG;
          sb += outB;
          ++n;
        }
      }
      if (n > 0) {
        uint8_t yy, uu, vv;
        RgbToYuv(static_cast<int>(sr / n), static_cast<int>(sg / n),
                 static_cast<int>(sb / n), &yy, &uu, &vv);
        uint8_t* uvp =
            uvPlane + static_cast<size_t>(uvStride) * (by / 2) + (bx / 2) * 2;
        uvp[0] = uu;
        uvp[1] = vv;
      }
    }
  }
}

}  // namespace ipcam
