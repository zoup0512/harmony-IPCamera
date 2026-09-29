#ifndef IPCAMERA_OSD_PIPELINE_H
#define IPCAMERA_OSD_PIPELINE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>

#include "osd_renderer.h"

namespace ipcam {

// CPU compositing pipeline between the camera and the video encoder when OSD
// is enabled. The camera PreviewOutput renders into an OH_ConsumerSurface; a
// worker thread acquires the latest NV12/NV21 buffer, burns the
// OH_Drawing-rasterized text layer (and the watermark) directly into the YUV
// planes and hands a tightly-packed NV12 frame to the encoder via FrameSink
// (buffer-mode PushInputBuffer). Chosen over GL/OES because this device's
// driver refuses to import camera NV12 buffers into an EGLImage, and over
// producer-side Request/FlushBuffer on the encoder surface because the codec
// HDI rejects it (NATIVE_ERROR_UNKNOWN).
class OsdPipeline {
 public:
  using FrameSink = std::function<void(const uint8_t* nv12, size_t size)>;
  using ErrorSink = std::function<void(const std::string&)>;

  OsdPipeline() = default;
  ~OsdPipeline();
  OsdPipeline(const OsdPipeline&) = delete;
  OsdPipeline& operator=(const OsdPipeline&) = delete;

  // Creates the consumer surface, boots the worker thread and returns the
  // surface id the camera PreviewOutput must render into. Blocks until init
  // finished. uiSurfaceId (local XComponent preview, 0 = none) is adopted
  // first: its display transform decides how far frames rotate so the encoded
  // picture stands upright — for a 90/270 rotation the encoder dimensions
  // become (reqHeight, reqWidth), exposed via OutWidth()/OutHeight().
  bool Start(int reqWidth, int reqHeight, uint64_t uiSurfaceId,
             uint64_t* cameraSurfaceId, FrameSink sink, ErrorSink onError);
  void Stop();

  int rotation() const { return rotation_; }
  int OutWidth() const { return outW_; }
  int OutHeight() const { return outH_; }

  // Local preview sink (XComponent surface id from ArkTS). When set, every
  // composited frame is written into that surface as well, so the in-app
  // preview shows the same picture as the encoder — OSD burned in — instead of
  // the raw camera frames a second PreviewOutput would deliver. Can be called
  // again to adopt a new surface (page switches recreate the XComponent).
  bool SetUiSurface(uint64_t surfaceId);
  void ClearUiSurface();

 private:
  struct CachedMap {
    OHNativeWindowBuffer* wb;
    OH_NativeBuffer* nb;
    BufferHandle* h;
    uint8_t* addr;
  };
  uint8_t* AddrFor(OHNativeWindowBuffer* wb, BufferHandle** hOut);
  void ClearMapCache();
  void Run();
  void CompositeOne();
  void PreviewLoop();
  bool PresentFrameToUi(const std::vector<uint8_t>& frame);
  void BlendRgbaIntoYuv(const uint8_t* rgba, int srcW, int srcH, bool premul,
                        int dstX, int dstY, int dstW, int dstH,
                        uint8_t* yPlane, uint8_t* uvPlane, int yStride,
                        int uvStride, int frameW, int frameH);
  void EnsureWmScaled(int dstW, int dstH, const OsdSnapshot& snap);
  static void OnFrameAvailable(void* context);
  static void WaitFence(int fd);
  void Fail(const char* what);

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool frameReady_ = false;
  bool quit_ = false;
  bool ready_ = false;
  bool initOk_ = false;
  uint64_t cameraSurfaceId_ = 0;
  int inW_ = 0;   // pre-rotation target (= camera-facing request)
  int inH_ = 0;
  int outW_ = 0;  // encoder-facing output (swapped when rotating 90/270)
  int outH_ = 0;
  int rotation_ = 0;         // 0/90/180/270 applied to every frame
  int uiOrigTransform_ = -1;  // display transform to restore on ClearUiSurface

  OH_NativeImage* image_ = nullptr;
  OsdRenderer renderer_;
  std::mutex mapMu_;  // guards mapCache_ (compositing and preview threads)
  std::vector<CachedMap> mapCache_;
  std::vector<uint8_t> nv12_;  // tightly packed compositing target
  std::vector<uint8_t> wmScaled_;  // watermark RGBA, straight alpha, scaled
  int wmScaledW_ = 0;
  int wmScaledH_ = 0;
  uint32_t wmScaledVersion_ = 0;
  OHNativeWindow* uiWindow_ = nullptr;  // local preview producer window
  uint64_t uiSurfaceId_ = 0;
  int uiFailCount_ = 0;
  bool uiLogged_ = false;
  // The preview producer runs on its own thread: writing into an RS surface can
  // block for seconds, and that must never stall the encoder feed.
  std::thread previewThread_;
  std::mutex presentMu_;
  std::condition_variable presentCv_;
  std::vector<uint8_t> presentFrame_;
  bool presentPending_ = false;
  bool presentQuit_ = false;
  FrameSink sink_;
  ErrorSink onError_;
  std::atomic<bool> stopped_{false};
  int frameCount_ = 0;
  bool blendLogged_ = false;
};

}  // namespace ipcam

#endif  // IPCAMERA_OSD_PIPELINE_H
