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
  // finished.
  bool Start(int outWidth, int outHeight, uint64_t* cameraSurfaceId,
             FrameSink sink, ErrorSink onError);
  void Stop();

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
  int outW_ = 0;
  int outH_ = 0;

  OH_NativeImage* image_ = nullptr;
  OsdRenderer renderer_;
  std::vector<CachedMap> mapCache_;
  std::vector<uint8_t> nv12_;  // tightly packed compositing target
  std::vector<uint8_t> wmScaled_;  // watermark RGBA, straight alpha, scaled
  int wmScaledW_ = 0;
  int wmScaledH_ = 0;
  uint32_t wmScaledVersion_ = 0;
  FrameSink sink_;
  ErrorSink onError_;
  std::atomic<bool> stopped_{false};
  int frameCount_ = 0;
};

}  // namespace ipcam

#endif  // IPCAMERA_OSD_PIPELINE_H
