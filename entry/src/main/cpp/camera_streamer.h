#ifndef IPCAMERA_CAMERA_STREAMER_H
#define IPCAMERA_CAMERA_STREAMER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <multimedia/player_framework/native_avcodec_base.h>
#include <ohcamera/camera.h>
#include <ohcamera/camera_input.h>
#include <ohcamera/capture_session.h>
#include <ohcamera/preview_output.h>

#include "media_time.h"

namespace ipcam {

class OsdPipeline;

// Camera capture -> OH_AVCodec H.264 (surface mode) -> Annex-B frames.
// With OSD disabled the camera preview output renders straight into the
// encoder's producer surface; with OSD enabled an OsdPipeline intercepts the
// frames, burns the text/watermark overlays in (CPU, YUV-domain) and feeds
// the encoder. Encoded Annex-B is handed to the RtspServer via FrameSink.
// Replaces the test pattern of the POC with real device camera content.
class CameraStreamer {
 public:
  using FrameSink = std::function<void(const uint8_t*, size_t, TimestampUs)>;
  using ErrorSink = std::function<void(const std::string&)>;

  CameraStreamer();
  ~CameraStreamer();
  CameraStreamer(const CameraStreamer&) = delete;
  CameraStreamer& operator=(const CameraStreamer&) = delete;

  bool Start(int width, int height, int bitrate, const char* mimeType, bool frontCamera,
             int iFrameIntervalMs, uint64_t previewSurfaceId, bool osdEnabled,
             FrameSink sink, ErrorSink onError);
  void Stop();
  bool IsRunning() const { return running_.load(); }
  int Orientation() const { return orientation_.load(); }
  // Encoder-facing dimensions: the requested size, except in OSD mode with a
  // 90/270 rotation where they are swapped.
  int EncodedWidth() const { return encW_; }
  int EncodedHeight() const { return encH_; }
  bool SetTorch(bool on);
  bool SwitchFacing();  // restart with the other camera, preview preserved
  bool Restart();       // restart with the same params (fresh encoder emits IDR)
  // Attach an extra preview surface (XComponent). Safe before or after Start;
  // before Start it is remembered and added when the session is configured.
  bool AttachPreview(uint64_t surfaceId);

 private:
  void Cleanup();
  void Fail(const std::string& what, int err);

  static void OnError(OH_AVCodec* codec, int32_t errorCode, void* userData);
  static void OnStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData);
  static void OnNeedInputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer,
                                void* userData);
  static void OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer,
                                void* userData);

  OH_AVCodec* encoder_ = nullptr;
  OHNativeWindow* window_ = nullptr;
  Camera_Manager* manager_ = nullptr;
  Camera_Input* input_ = nullptr;
  Camera_CaptureSession* session_ = nullptr;
  Camera_PreviewOutput* preview_ = nullptr;
  Camera_PreviewOutput* previewUi_ = nullptr;
  Camera_Profile uiProfile_{};
  uint64_t pendingPreviewId_ = 0;
  bool hasPendingPreview_ = false;
  // last-start parameters, kept so AttachPreview can restart with both surfaces
  int lastWidth_ = 1280;
  int lastHeight_ = 720;
  int lastBitrate_ = 2000000;
  int lastIFrameMs_ = 2000;
  std::string lastMime_ = "video/avc";
  bool lastFront_ = false;
  bool lastOsd_ = false;
  std::unique_ptr<OsdPipeline> osd_;
  // buffer-mode input path (OSD on): latest composited NV12 frame slot
  std::mutex pendingMu_;
  std::condition_variable pendingCv_;
  bool hasPending_ = false;
  std::vector<uint8_t> pendingFrame_;
  std::vector<uint8_t> lastFrame_;
  std::atomic<bool> running_{false};
  std::atomic<int> orientation_{0};
  int encW_ = 0;  // encoder dimensions of the current run
  int encH_ = 0;
  FrameSink sink_;
  ErrorSink onError_;
  std::mutex stateMu_;
};

}  // namespace ipcam

#endif  // IPCAMERA_CAMERA_STREAMER_H
