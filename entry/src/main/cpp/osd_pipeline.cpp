#include "osd_pipeline.h"

#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unistd.h>
#include <poll.h>

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "OsdPipeline"

namespace {

constexpr uint64_t kCpuUsage =
    NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;

bool CheckedMul(size_t a, size_t b, size_t* result) {
  if (result == nullptr ||
      (a != 0 && b > std::numeric_limits<size_t>::max() / a)) {
    return false;
  }
  *result = a * b;
  return true;
}

bool CheckedAdd(size_t a, size_t b, size_t* result) {
  if (result == nullptr || b > std::numeric_limits<size_t>::max() - a) {
    return false;
  }
  *result = a + b;
  return true;
}

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
// (PopFromFreeList NULL deref).
//
// Refcount rules (learned the hard way, see the OHOS capi-native-buffer-h
// docs): OH_NativeBuffer_FromNativeWindowBuffer returns a BORROWED
// OH_NativeBuffer — unlike OH_NativeBuffer_Alloc/ReadFromParcel it does not
// take a reference, so Unreference on it decrements the queue's own count and
// destroys buffers the queue still owns. We must never Unreference these, and
// never touch a cached nb after the queue may have reallocated its buffer set
// (happens across camera reconnection, e.g. background/foreground cycles —
// Unmap on such a stale pointer was the 11h-lifetime SIGSEGV in
// ClearMapCache). The mappings die with the SurfaceBuffers when
// OH_NativeImage_Destroy tears the queue down.
struct CachedMap {
  OHNativeWindowBuffer* wb;
  OH_NativeBuffer* nb;
  BufferHandle* h;
  uint8_t* addr;
};

// Rotation needed so a sensor frame stands upright in natural viewing
// orientation, derived from the preview surface's display transform. Mirrors
// (front-camera selfie flips) are deliberately dropped: surveillance output
// should not be left-right flipped, and they don't affect uprightness.
int RotationFromTransform(int transform) {
  switch (transform) {
    case 1:  // ROTATE_90
    case 6:  // FLIP_H_ROT90
    case 7:  // FLIP_V_ROT90
      return 90;
    case 2:  // ROTATE_180
    case 8:  // FLIP_H_ROT180
    case 9:  // FLIP_V_ROT180
      return 180;
    case 3:  // ROTATE_270
    case 10: // FLIP_H_ROT270
    case 11: // FLIP_V_ROT270
      return 270;
    default:  // NONE or a pure flip
      return 0;
  }
}

}  // namespace

namespace ipcam {

uint8_t* OsdPipeline::AddrFor(OHNativeWindowBuffer* wb, BufferHandle** hOut) {
  std::lock_guard<std::mutex> lk(mapMu_);
  BufferHandle* h = OH_NativeWindow_GetBufferHandleFromNative(wb);
  if (h == nullptr) return nullptr;
  for (auto it = mapCache_.begin(); it != mapCache_.end(); ++it) {
    if (it->wb == wb) {
      if (it->h != h) {
        // Same wrapper address but a different BufferHandle inside: the queue
        // recycled this address for a new SurfaceBuffer and the cached entry
        // (nb included) is stale. Drop it and re-map below.
        mapCache_.erase(it);
        break;
      }
      *hOut = it->h;
      return it->addr;
    }
  }
  CachedMap c{wb, nullptr, h, nullptr};
  // Borrowed pointer — do NOT Unreference it on any failure path.
  if (OH_NativeBuffer_FromNativeWindowBuffer(wb, &c.nb) != 0 || c.nb == nullptr) {
    return nullptr;
  }
  void* addr = nullptr;
  if (OH_NativeBuffer_Map(c.nb, &addr) != 0 || addr == nullptr) {
    return nullptr;
  }
  c.addr = static_cast<uint8_t*>(addr);
  mapCache_.push_back(c);
  *hOut = c.h;
  return c.addr;
}

void OsdPipeline::ClearMapCache() {
  // The cached OH_NativeBuffer pointers are borrowed from the queue and some
  // may already be freed after a queue-side buffer reallocation; calling
  // Unmap/Unreference on them crashes (freed-heap deref) and over-releases
  // the queue's own references. Just forget them — the mappings are torn
  // down by the SurfaceBuffer destructors inside OH_NativeImage_Destroy.
  std::lock_guard<std::mutex> lk(mapMu_);
  mapCache_.clear();
}

bool OsdPipeline::SetUiSurface(uint64_t surfaceId) {
  if (surfaceId == 0) return false;
  int w = 0;
  int h = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (surfaceId == uiSurfaceId_ && uiWindow_ != nullptr) return true;
    w = outW_;
    h = outH_;
  }
  if (w <= 0 || h <= 0) return false;  // Start() has not run yet
  OHNativeWindow* window = nullptr;
  int createRet = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
  if (createRet != 0 || window == nullptr) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                 "ui preview surface %{public}llu unavailable (ret=%{public}d)",
                 static_cast<unsigned long long>(surfaceId), createRet);
    return false;
  }
  int transform = 0;
  OH_NativeWindow_NativeWindowHandleOpt(window, GET_TRANSFORM, &transform);
  int rot = RotationFromTransform(transform);
  if (rot != rotation_) {
    // The encoder geometry is fixed for this run; only the preview would be
    // able to follow the new orientation.
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                 "preview orientation changed mid-stream (%{public}d -> %{public}d); "
                 "keeping encoder rotation",
                 rotation_, rot);
  }
  // Put the frame in the layout the pipeline produces. The render service owns
  // this surface, so set usage first: a CPU producer needs CPU-visible buffers.
  int usageRet = OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE, kCpuUsage);
  int geomRet = OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY, w, h);
  int fmtRet = OH_NativeWindow_NativeWindowHandleOpt(
      window, SET_FORMAT,
      static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP));
  int transformRet = OH_NativeWindow_NativeWindowHandleOpt(window, SET_TRANSFORM,
                                                           NATIVEBUFFER_ROTATE_NONE);
  int gotW = 0;
  int gotH = 0;
  int gotFmt = 0;
  uint64_t gotUsage = 0;
  OH_NativeWindow_NativeWindowHandleOpt(window, GET_BUFFER_GEOMETRY, &gotH, &gotW);
  OH_NativeWindow_NativeWindowHandleOpt(window, GET_FORMAT, &gotFmt);
  OH_NativeWindow_NativeWindowHandleOpt(window, GET_USAGE, &gotUsage);
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "ui preview setup usage=%{public}d geom=%{public}d fmt=%{public}d "
               "transform=%{public}d(%{public}d) now=%{public}dx%{public}d fmt=%{public}d "
               "usage=0x%{public}llx",
               usageRet, geomRet, fmtRet, transformRet, transform, gotW, gotH,
               gotFmt, static_cast<unsigned long long>(gotUsage));
  ClearUiSurface();
  {
    std::lock_guard<std::mutex> lk(mu_);
    uiWindow_ = window;
    uiSurfaceId_ = surfaceId;
    uiOrigTransform_ = transform;
    uiFailCount_ = 0;
    uiLogged_ = false;
  }
  return true;
}

void OsdPipeline::ClearUiSurface() {
  OHNativeWindow* window = nullptr;
  int origTransform = -1;
  {
    std::lock_guard<std::mutex> lk(mu_);
    window = uiWindow_;
    origTransform = uiOrigTransform_;
    uiWindow_ = nullptr;
    uiSurfaceId_ = 0;
    uiOrigTransform_ = -1;
  }
  if (window != nullptr) {
    if (origTransform >= 0) {
      // Give the transform back so the raw camera path (OSD off) shows its
      // usual selfie-oriented preview on the same surface.
      OH_NativeWindow_NativeWindowHandleOpt(window, SET_TRANSFORM, origTransform);
    }
    OH_NativeWindow_DestroyNativeWindow(window);
  }
}

// Local preview producer thread. Kept off the compositing thread because
// writing into a render-service surface can block for seconds when the surface
// is not consuming; the encoder feed must never wait on the preview. Only the
// newest frame is kept — a slow preview drops frames instead of building lag.
void OsdPipeline::PreviewLoop() {
  for (;;) {
    std::vector<uint8_t> frame;
    {
      std::unique_lock<std::mutex> lk(presentMu_);
      presentCv_.wait(lk, [this] { return presentQuit_ || presentPending_; });
      if (presentQuit_) return;
      frame.swap(presentFrame_);
      presentPending_ = false;
    }
    if (!PresentFrameToUi(frame)) {
      if (uiFailCount_ >= 3) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                     "ui preview surface unresponsive, local preview disabled");
        ClearUiSurface();
      }
    }
  }
}

// Returns false when the frame could not be handed to the surface.
bool OsdPipeline::PresentFrameToUi(const std::vector<uint8_t>& frame) {
  OHNativeWindow* window = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    window = uiWindow_;
  }
  if (window == nullptr) {
    return true;  // nothing to do; not a failure
  }
  if (frame.size() < static_cast<size_t>(outW_) * outH_ * 3 / 2) {
    ++uiFailCount_;
    return false;
  }
  OHNativeWindowBuffer* buf = nullptr;
  int fence = -1;
  int ret = OH_NativeWindow_NativeWindowRequestBuffer(window, &buf, &fence);
  if (ret != 0 || buf == nullptr) {
    ++uiFailCount_;
    if (uiFailCount_ <= 3) {
      OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                   "ui preview request failed ret=%{public}d buf=%{public}p "
                   "(fail %{public}d)",
                   ret, static_cast<void*>(buf), uiFailCount_);
    }
    return false;
  }
  uiFailCount_ = 0;
  WaitFence(fence);
  // Address via Map, not BufferHandle::virAddr: queue buffers report no virtual
  // address (same as the camera buffers on the consumer side), and AddrFor
  // caches the mapping per buffer wrapper. The wrapper belongs to the queue —
  // only Abort/Flush it, never OH_NativeWindow_DestroyNativeWindowBuffer
  // (destroying it starves the queue after the first frame).
  BufferHandle* h = nullptr;
  uint8_t* dstBase = AddrFor(buf, &h);
  bool presented = false;
  if (h != nullptr && dstBase != nullptr && h->stride >= outW_) {
    const int dstRows = h->height > outH_ ? h->height : outH_;
    const size_t yBytes = static_cast<size_t>(h->stride) * dstRows;
    const size_t need = yBytes + static_cast<size_t>(h->stride) * (outH_ / 2);
    if (h->size >= 0 && static_cast<size_t>(h->size) >= need) {
      const uint8_t* srcY = frame.data();
      const uint8_t* srcUv = srcY + static_cast<size_t>(outW_) * outH_;
      uint8_t* dstY = dstBase;
      for (int row = 0; row < outH_; ++row) {
        memcpy(dstY + static_cast<size_t>(h->stride) * row,
               srcY + static_cast<size_t>(outW_) * row,
               static_cast<size_t>(outW_));
      }
      if (h->format == static_cast<int>(NATIVEBUFFER_PIXEL_FMT_YCBCR_420_P)) {
        // The queue normalised the request to planar I420: split the
        // interleaved UV pair into separate U and V planes.
        const int chromaStride = h->stride / 2;
        uint8_t* dstU = dstBase + yBytes;
        uint8_t* dstV = dstU + static_cast<size_t>(chromaStride) * (dstRows / 2);
        for (int row = 0; row < outH_ / 2; ++row) {
          const uint8_t* s = srcUv + static_cast<size_t>(outW_) * row;
          uint8_t* du = dstU + static_cast<size_t>(chromaStride) * row;
          uint8_t* dv = dstV + static_cast<size_t>(chromaStride) * row;
          for (int pair = 0; pair < outW_ / 2; ++pair) {
            du[pair] = s[pair * 2];
            dv[pair] = s[pair * 2 + 1];
          }
        }
        presented = true;
      } else if (h->format ==
                 static_cast<int>(NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP)) {
        uint8_t* dstUv = dstBase + yBytes;
        for (int row = 0; row < outH_ / 2; ++row) {
          memcpy(dstUv + static_cast<size_t>(h->stride) * row,
                 srcUv + static_cast<size_t>(outW_) * row,
                 static_cast<size_t>(outW_));
        }
        presented = true;
      }
      if (presented && !uiLogged_) {
        uiLogged_ = true;
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                     "ui preview %{public}dx%{public}d stride=%{public}d "
                     "fmt=%{public}d size=%{public}d",
                     outW_, outH_, h->stride, h->format, h->size);
      }
    }
  }
  if (!presented && !uiLogged_) {
    uiLogged_ = true;
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                 "ui preview buffer unusable (h=%{public}p addr=%{public}p "
                 "stride=%{public}d fmt=%{public}d size=%{public}d)",
                 static_cast<void*>(h), static_cast<void*>(dstBase),
                 h != nullptr ? h->stride : -1, h != nullptr ? h->format : -1,
                 h != nullptr ? h->size : -1);
  }
  Region region{nullptr, 0};  // empty region = whole buffer is dirty
  if (presented) {
    OH_NativeWindow_NativeWindowFlushBuffer(window, buf, -1, region);
  } else {
    OH_NativeWindow_NativeWindowAbortBuffer(window, buf);
  }
  return presented;
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

bool OsdPipeline::Start(int reqWidth, int reqHeight, uint64_t uiSurfaceId,
                        uint64_t* cameraSurfaceId, FrameSink sink,
                        ErrorSink onError) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    sink_ = std::move(sink);
    onError_ = std::move(onError);
    quit_ = false;
    ready_ = false;
    initOk_ = false;
    cameraSurfaceId_ = 0;
  }
  if (reqWidth <= 0 || reqHeight <= 0 || (reqWidth & 1) != 0 ||
      (reqHeight & 1) != 0) {
    Fail("invalid OSD output dimensions");
    return false;
  }
  // Adopt the local preview surface first: its display transform tells how far
  // sensor frames must rotate for the encoded picture to stand upright, and
  // that fixes the encoder dimensions before anything is configured.
  rotation_ = 0;
  uiOrigTransform_ = -1;
  if (uiSurfaceId != 0) {
    OHNativeWindow* window = nullptr;
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(uiSurfaceId, &window) == 0 &&
        window != nullptr) {
      int transform = 0;
      OH_NativeWindow_NativeWindowHandleOpt(window, GET_TRANSFORM, &transform);
      rotation_ = RotationFromTransform(transform);
      // The pipeline delivers already-rotated frames; the render service must
      // not rotate them again. ClearUiSurface restores the original transform
      // so the raw camera path keeps its selfie view.
      OH_NativeWindow_NativeWindowHandleOpt(window, SET_TRANSFORM,
                                            NATIVEBUFFER_ROTATE_NONE);
      OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE, kCpuUsage);
      uiWindow_ = window;
      uiSurfaceId_ = uiSurfaceId;
      uiOrigTransform_ = transform;
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "ui preview surface transform=%{public}d -> rotate %{public}d",
                   transform, rotation_);
    } else {
      OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                   "ui preview surface %{public}llu unavailable, OSD stays "
                   "unrotated",
                   static_cast<unsigned long long>(uiSurfaceId));
    }
  }
  inW_ = reqWidth;
  inH_ = reqHeight;
  outW_ = (rotation_ % 180 != 0) ? reqHeight : reqWidth;
  outH_ = (rotation_ % 180 != 0) ? reqWidth : reqHeight;
  if (uiWindow_ != nullptr) {
    OH_NativeWindow_NativeWindowHandleOpt(uiWindow_, SET_BUFFER_GEOMETRY, outW_,
                                          outH_);
    OH_NativeWindow_NativeWindowHandleOpt(
        uiWindow_, SET_FORMAT,
        static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP));
  }
  size_t outputYBytes = 0;
  size_t outputUvBytes = 0;
  size_t outputBytes = 0;
  if (!CheckedMul(static_cast<size_t>(outW_), static_cast<size_t>(outH_),
                  &outputYBytes) ||
      !CheckedMul(static_cast<size_t>(outW_),
                  static_cast<size_t>(outH_ / 2), &outputUvBytes) ||
      !CheckedAdd(outputYBytes, outputUvBytes, &outputBytes)) {
    Fail("OSD output buffer size overflow");
    return false;
  }
  if (outputBytes > nv12_.max_size()) {
    Fail("OSD output buffer size is unsupported");
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(presentMu_);
    presentQuit_ = false;
    presentPending_ = false;
  }
  try {
    nv12_.assign(outputBytes, 0);
  } catch (const std::bad_alloc&) {
    Fail("OSD output buffer allocation failed");
    return false;
  }

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
  previewThread_ = std::thread(&OsdPipeline::PreviewLoop, this);
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
  {
    std::lock_guard<std::mutex> lk(presentMu_);
    presentQuit_ = true;
    presentPending_ = false;
  }
  presentCv_.notify_all();
  if (previewThread_.joinable()) previewThread_.join();
  ClearUiSurface();  // drop the preview window before its surface may die
  ClearMapCache();  // drop borrowed buffer pointers before the surface dies
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
  const int camW = ch->width;
  const int camH = ch->height;
  const int camStride = ch->stride;
  const bool nv12Cam =
      ch->format == static_cast<int>(NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP);
  const bool nv21Cam =
      ch->format == static_cast<int>(NATIVEBUFFER_PIXEL_FMT_YCRCB_420_SP);
  auto dropInvalid = [&](const char* reason) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                 "drop camera buffer: %{public}s (w=%{public}d h=%{public}d "
                 "stride=%{public}d size=%{public}d fmt=%{public}d)",
                 reason, camW, camH, camStride, ch->size, ch->format);
    OH_NativeImage_ReleaseNativeWindowBuffer(image_, latest, -1);
  };
  if (!nv12Cam && !nv21Cam) {
    dropInvalid("unsupported format");
    return;
  }
  if (camW <= 0 || camH <= 0 || (camW & 1) != 0 || (camH & 1) != 0 ||
      camStride < camW) {
    dropInvalid("invalid dimensions or stride");
    return;
  }

  size_t camYBytes = 0;
  size_t camUvBytes = 0;
  size_t camBytes = 0;
  if (!CheckedMul(static_cast<size_t>(camStride), static_cast<size_t>(camH),
                  &camYBytes) ||
      !CheckedMul(static_cast<size_t>(camStride),
                  static_cast<size_t>(camH / 2), &camUvBytes) ||
      !CheckedAdd(camYBytes, camUvBytes, &camBytes) || ch->size < 0 ||
      static_cast<size_t>(ch->size) < camBytes) {
    dropInvalid("buffer is too small or size overflowed");
    return;
  }

  size_t outputYBytes = 0;
  size_t outputUvBytes = 0;
  size_t outputBytes = 0;
  if (outW_ <= 0 || outH_ <= 0 || (outW_ & 1) != 0 || (outH_ & 1) != 0 ||
      !CheckedMul(static_cast<size_t>(outW_), static_cast<size_t>(outH_),
                  &outputYBytes) ||
      !CheckedMul(static_cast<size_t>(outW_), static_cast<size_t>(outH_ / 2),
                  &outputUvBytes) ||
      !CheckedAdd(outputYBytes, outputUvBytes, &outputBytes) ||
      nv12_.size() < outputBytes) {
    dropInvalid("invalid OSD output buffer");
    return;
  }

  ++frameCount_;
  OsdSnapshot snap = GetOsd();
  renderer_.RefreshIfNeeded(snap, outW_, outH_);
  if (frameCount_ <= 2 || frameCount_ % 300 == 0) {
    const OsdRenderer::Layer& tl = renderer_.text();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                 "cam %{public}dx%{public}d stride=%{public}d fmt=%{public}d frame=%{public}d "
                 "osd=%{public}d rot=%{public}d text=%{public}dx%{public}d",
                 camW, camH, camStride, ch->format, frameCount_,
                 snap.enabled ? 1 : 0, rotation_, tl.width, tl.height);
  }

  // 1) Copy the camera frame into the tightly packed NV12 target, scaled to
  //    the requested size and rotated so the encoded picture stands upright
  //    (output is always NV12; a camera NV21 source gets its UV pair
  //    swapped). The OSD is burned after this step, in the rotated — visual —
  //    coordinate system, so "top-left" is the top-left the viewer sees.
  const uint8_t* camY = camAddr;
  const uint8_t* camUv = camY + camYBytes;
  uint8_t* encY = nv12_.data();
  uint8_t* encUv = encY + outputYBytes;
  const bool sameSize = camW == inW_ && camH == inH_;
  // Maps an output (rotated) pixel coordinate back to the pre-rotation frame.
  auto toPre = [&](int ox, int oy, int* px, int* py) {
    switch (rotation_) {
      case 90:
        *px = oy;
        *py = outW_ - 1 - ox;
        break;
      case 180:
        *px = outW_ - 1 - ox;
        *py = outH_ - 1 - oy;
        break;
      case 270:
        *px = outH_ - 1 - oy;
        *py = ox;
        break;
      default:
        *px = ox;
        *py = oy;
        break;
    }
  };
  for (int row = 0; row < outH_; ++row) {
    uint8_t* dstY = encY + static_cast<size_t>(outW_) * row;
    if (rotation_ == 0 && sameSize) {
      memcpy(dstY, camY + static_cast<size_t>(camStride) * row,
             static_cast<size_t>(outW_));
      continue;
    }
    for (int col = 0; col < outW_; ++col) {
      int px = 0;
      int py = 0;
      toPre(col, row, &px, &py);
      int sx = sameSize
                   ? px
                   : static_cast<int>(static_cast<int64_t>(px) * camW / inW_);
      int sy = sameSize
                   ? py
                   : static_cast<int>(static_cast<int64_t>(py) * camH / inH_);
      dstY[col] = camY[static_cast<size_t>(camStride) * sy + sx];
    }
  }

  const int outChromaW = outW_ / 2;
  const int outChromaH = outH_ / 2;
  for (int row = 0; row < outChromaH; ++row) {
    uint8_t* dstUv = encUv + static_cast<size_t>(outW_) * row;
    for (int pair = 0; pair < outChromaW; ++pair) {
      // A corner of the 2x2 luma block is enough to identify its chroma
      // sample: 90-multiple rotations map blocks to blocks.
      int px = 0;
      int py = 0;
      toPre(pair * 2, row * 2, &px, &py);
      int sx = sameSize
                   ? px
                   : static_cast<int>(static_cast<int64_t>(px) * camW / inW_);
      int sy = sameSize
                   ? py
                   : static_cast<int>(static_cast<int64_t>(py) * camH / inH_);
      const uint8_t* srcUv =
          camUv + static_cast<size_t>(camStride) * (sy / 2) + (sx / 2) * 2;
      size_t dstOffset = static_cast<size_t>(pair) * 2;
      dstUv[dstOffset] = nv21Cam ? srcUv[1] : srcUv[0];
      dstUv[dstOffset + 1] = nv21Cam ? srcUv[0] : srcUv[1];
    }
  }

  // 2) Burn the overlays into the NV12 copy.
  const OsdRenderer::Layer& text = renderer_.text();
  if (snap.enabled && !text.empty()) {
    OsdRenderer::Rect tr = renderer_.TextRect(snap, outW_, outH_);
    BlendRgbaIntoYuv(text.rgba.data(), text.width, text.height, true, tr.x, tr.y,
                     tr.w, tr.h, encY, encUv, outW_, outW_, outW_, outH_);
    if (!blendLogged_ && sameSize && rotation_ == 0) {
      blendLogged_ = true;
      // Sampled diff of the composited Y plane against the untouched camera
      // plane inside the text rect: proves the blend reached the encoder input.
      size_t changed = 0;
      size_t sampled = 0;
      for (int y = tr.y; y < tr.y + tr.h && y < outH_; y += 2) {
        for (int x = tr.x; x < tr.x + tr.w && x < outW_; x += 2) {
          ++sampled;
          if (encY[static_cast<size_t>(y) * outW_ + x] !=
              camY[static_cast<size_t>(y) * camStride + x]) {
            ++changed;
          }
        }
      }
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "osd blend rect %{public}d,%{public}d %{public}dx%{public}d "
                   "changed=%{public}zu/%{public}zu sampled px",
                   tr.x, tr.y, tr.w, tr.h, changed, sampled);
    }
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
  bool uiActive = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    uiActive = uiWindow_ != nullptr;
  }
  if (uiActive) {
    std::lock_guard<std::mutex> lk(presentMu_);
    if (!presentPending_) {  // otherwise drop: the preview is still behind
      presentFrame_.assign(nv12_.begin(), nv12_.end());
      presentPending_ = true;
      presentCv_.notify_one();
    }
  }
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
  if (rgba == nullptr || yPlane == nullptr || uvPlane == nullptr || srcW <= 0 ||
      srcH <= 0 || dstW <= 0 || dstH <= 0 || frameW <= 0 || frameH <= 0 ||
      (frameW & 1) != 0 || (frameH & 1) != 0 || yStride <= 0 ||
      uvStride <= 0 || yStride < frameW || uvStride < frameW) {
    return;
  }
  size_t checkedBytes = 0;
  if (!CheckedMul(static_cast<size_t>(srcW), static_cast<size_t>(srcH),
                  &checkedBytes) ||
      !CheckedMul(checkedBytes, static_cast<size_t>(4), &checkedBytes) ||
      !CheckedMul(static_cast<size_t>(yStride), static_cast<size_t>(frameH),
                  &checkedBytes) ||
      !CheckedMul(static_cast<size_t>(uvStride),
                  static_cast<size_t>(frameH / 2), &checkedBytes)) {
    return;
  }
  int64_t dstRight = static_cast<int64_t>(dstX) + dstW;
  int64_t dstBottom = static_cast<int64_t>(dstY) + dstH;
  int x0 = dstX <= 0 ? 0 : (dstX >= frameW ? frameW : dstX);
  int y0 = dstY <= 0 ? 0 : (dstY >= frameH ? frameH : dstY);
  int x1 = dstRight <= 0
               ? 0
               : (dstRight >= frameW ? frameW : static_cast<int>(dstRight));
  int y1 = dstBottom <= 0
               ? 0
               : (dstBottom >= frameH ? frameH : static_cast<int>(dstBottom));
  if (x0 >= x1 || y0 >= y1) return;
  auto sample = [&](int px, int py, int* r, int* g, int* b, int* a) {
    int sx = static_cast<int>((static_cast<int64_t>(px) - dstX) * srcW / dstW);
    int sy = static_cast<int>((static_cast<int64_t>(py) - dstY) * srcH / dstH);
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
        if (py < y0) continue;
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
