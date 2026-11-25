#ifndef IPCAMERA_CAMERA_STREAMER_H
#define IPCAMERA_CAMERA_STREAMER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <multimedia/player_framework/native_avcodec_base.h>
#include <ohcamera/camera.h>
#include <ohcamera/camera_input.h>
#include <ohcamera/capture_session.h>
#include <ohcamera/preview_output.h>

#include "capture_orientation.h"
#include "dual_camera.h"
#include "media_time.h"

namespace ipcam {

class OsdPipeline;

// Camera capture -> OH_AVCodec H.264/H.265 -> Annex-B frames.
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
             FrameSink sink, ErrorSink onError, const std::string& deviceId = {},
             const std::string& encoderName = {}, int avcProfile = -1,
             CaptureOrientation orientation = {});
  void Stop();
  bool IsRunning() const { return running_.load(); }
  int Orientation() const { return orientation_.load(); }
  // Encoder-facing dimensions: swapped for a 90/270 pixel rotation, regardless of OSD text.
  int EncodedWidth() const { return encW_; }
  int EncodedHeight() const { return encH_; }
  // Serialize all control calls and geometry/list reads with ServerHolder::cameraMu.
  int CaptureWidth() const { return IsRunning() ? config_.width : 0; }
  int CaptureHeight() const { return IsRunning() ? config_.height : 0; }
  std::vector<std::pair<int, int>> AvailableResolutions() const;
  enum class ResizeResult { Unchanged, Changed, Rejected, Restored, Stopped };
  // Rejected/Unchanged leave the current encoder untouched. Restored normally
  // means the previous run; if both dual layouts fail it may be the original
  // single-camera path (read DualLayout()). Stopped means no live stream.
  ResizeResult ChangeResolution(int width, int height);
  // 0: return to the original single path; 1: PiP; 2: left/right; 3: top/bottom.
  ResizeResult SetDualLayout(int layout, const DualCameraPlan* plan);
  int DualLayout() const { return IsRunning() ? config_.dualLayout : 0; }
  // Synchronous encoder epoch boundary, immediately BEFORE each encoder Start
  // (including rollback). Old encoder callbacks are drained. Clear all cached
  // frames and codec params, publish this actual geometry, then accept only new
  // parameter sets/random-access frames. Not a playback-ready notification.
  // Persists across Stop; do not reenter camera controls from the callback.
  void SetVideoFormatCallback(std::function<void(int, int)> cb) { formatSink_ = std::move(cb); }
  // Ranges describe the current primary preview output, not encoder or measured FPS.
  std::string CaptureFpsJson() const;
  bool SetCaptureFps(int min, int max);
  std::string FocusInfoJson() const;
  bool SetFocusMode(int mode);
  bool FocusAt(double x, double y);  // Requires single AUTO; success is acceptance, not focus lock.
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
  struct RunConfig {
    int width = 0, height = 0, bitrate = 0, iFrameMs = 0;
    std::string mime, deviceId, encoderName;
    int avcProfile = -1;
    bool front = false, osd = false;
    CaptureOrientation orientation;  // Immutable across rebuilds of this run.
    uint64_t previewId = 0;
    int dualLayout = 0;
    DualCameraPlan dualPlan;
    // User capture controls; re-applied best-effort after every rebuild.
    int fpsMin = 0, fpsMax = 0;  // 0: device default
    int focusMode = -1;          // -1: device default
  } config_;
  void ApplyCaptureControls();
  bool StartRun(RunConfig config, FrameSink sink, ErrorSink onError, bool allowOsdFallback);
  ResizeResult ReplaceRun(const RunConfig& next);
  std::function<void(int, int)> formatSink_;
  std::atomic<bool> bufferInput_{false};
  std::unique_ptr<DualCameraCapture> dual_;
  std::unique_ptr<OsdPipeline> osd_;
  // Buffer-mode input path (OSD/dual/pixel rotation). Codec input indices are
  // queued, never dropped or held by a sleeping callback: whichever side comes
  // second (a new frame or a free index) pushes. Stale frames are never repeated.
  struct FreeInput { uint32_t index; OH_AVBuffer* buffer; };
  bool PushFrame(const FreeInput& input, const uint8_t* frame, size_t size);
  std::mutex pendingMu_;
  std::condition_variable pendingCv_;
  bool hasPending_ = false;
  std::vector<uint8_t> pendingFrame_;
  std::vector<FreeInput> freeInputs_;  // under pendingMu_, FIFO
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
