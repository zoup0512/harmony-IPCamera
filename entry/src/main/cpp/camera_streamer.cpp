#include "camera_streamer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>
#include <ohcamera/camera_input.h>
#include <ohcamera/camera_manager.h>
#include <ohcamera/capture_session.h>
#include <ohcamera/preview_output.h>

#include "camera_capabilities.h"
#include "osd_pipeline.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "CameraStreamer"

namespace ipcam {
namespace {

void LogErr(const char* what, int err) {
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "%{public}s err=%{public}d", what, err);
}

// Last rotation the OSD pipeline derived from the preview surface. Recordings
// in the raw (surface) path use it as MP4 rotation metadata so players show
// them upright; the OSD path needs no metadata because its pixels are already
// rotated before encoding.
std::atomic<int> g_lastKnownRotation{0};

}  // namespace

CameraStreamer::CameraStreamer() = default;

CameraStreamer::~CameraStreamer() { Stop(); }

void CameraStreamer::Fail(const std::string& what, int err) {
  LogErr(what.c_str(), err);
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    onError = onError_;
  }
  if (onError) onError(what + " err=" + std::to_string(err));
}

void CameraStreamer::OnError(OH_AVCodec*, int32_t errorCode, void* userData) {
  auto self = static_cast<CameraStreamer*>(userData);
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(self->stateMu_);
    onError = self->onError_;
  }
  if (onError) onError("encoder error " + std::to_string(errorCode));
}

void CameraStreamer::OnStreamChanged(OH_AVCodec*, OH_AVFormat*, void*) {}

void CameraStreamer::OnNeedInputBuffer(OH_AVCodec* codec, uint32_t index,
                                       OH_AVBuffer* buffer, void* userData) {
  // Buffer mode (OSD on): pull the latest composited NV12 frame and push it.
  // Surface mode (OSD off) never gets input callbacks (camera feeds directly).
  auto self = static_cast<CameraStreamer*>(userData);
  if (!self->bufferInput_.load() || !self->running_.load()) return;
  std::vector<uint8_t> frame;
  {
    std::unique_lock<std::mutex> lk(self->pendingMu_);
    auto ready = [self] { return self->hasPending_ || !self->running_.load(); };
    if (self->freshInputOnly_) {
      // Hold this codec input index until both cameras produce a usable frame.
      // Returning unused indices on timeout would starve the codec on recovery.
      self->pendingCv_.wait(lk, ready);
    } else {
      self->pendingCv_.wait_for(lk, std::chrono::milliseconds(200), ready);
    }
    if (self->hasPending_) {
      frame.swap(self->pendingFrame_);
      self->hasPending_ = false;
      if (!self->freshInputOnly_) self->lastFrame_ = frame;
    } else if (!self->freshInputOnly_ && !self->lastFrame_.empty()) {
      frame = self->lastFrame_;
    }
  }
  if (frame.empty() || !self->running_.load()) return;
  uint8_t* addr = OH_AVBuffer_GetAddr(buffer);
  if (addr == nullptr || frame.size() > OH_AVBuffer_GetCapacity(buffer)) return;
  memcpy(addr, frame.data(), frame.size());
  OH_AVCodecBufferAttr attr{};
  attr.size = static_cast<int32_t>(frame.size());
  attr.pts = std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count();
  attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
  OH_AVBuffer_SetBufferAttr(buffer, &attr);
  OH_VideoEncoder_PushInputBuffer(codec, index);
}

void CameraStreamer::OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer,
                                       void* userData) {
  auto self = static_cast<CameraStreamer*>(userData);
  OH_AVCodecBufferAttr attr{};
  OH_AVBuffer_GetBufferAttr(buffer, &attr);
  if (self->running_.load() && !(attr.flags & AVCODEC_BUFFER_FLAGS_EOS) && attr.size > 0) {
    FrameSink sink;
    {
      std::lock_guard<std::mutex> lk(self->stateMu_);
      sink = self->sink_;
    }
    if (sink) {
      sink(OH_AVBuffer_GetAddr(buffer), static_cast<size_t>(attr.size), attr.pts);
    }
  }
  OH_VideoEncoder_FreeOutputBuffer(codec, index);
}

void CameraStreamer::Cleanup() {
  {
    // Synchronize the final notification with the sleeping dual input callback.
    std::lock_guard<std::mutex> lk(pendingMu_);
    running_ = false;
  }
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = nullptr;
    onError_ = nullptr;
  }
  bufferInput_ = false;
  pendingCv_.notify_all();
  // Tear the producer down first: stop the camera preview outputs and the
  // capture session BEFORE stopping the OSD pipeline. The OSD consumer
  // surface must not be unmapped/destroyed while the camera is still pushing
  // frames into it (in-flight FlushBuffer/ReuseBuffer on the queue during
  // teardown was the ClearMapCache SIGSEGV). The OSD path feeds the encoder
  // in buffer mode, so the encoder is independent of the camera surface.
  if (previewUi_ != nullptr) {
    OH_PreviewOutput_Stop(previewUi_);
    OH_PreviewOutput_Release(previewUi_);
    previewUi_ = nullptr;
  }
  if (preview_ != nullptr) {
    OH_PreviewOutput_Stop(preview_);
    OH_PreviewOutput_Release(preview_);
    preview_ = nullptr;
  }
  if (session_ != nullptr) {
    OH_CaptureSession_Release(session_);
    session_ = nullptr;
  }
  if (input_ != nullptr) {
    OH_CameraInput_Release(input_);
    input_ = nullptr;
  }
  if (manager_ != nullptr) {
    OH_Camera_DeleteCameraManager(manager_);
    manager_ = nullptr;
  }
  if (dual_) dual_->Stop();  // Both producers off; the primary worker may still be in Composite.
  if (osd_ != nullptr) {
    osd_->Stop();
    osd_.reset();
  }
  dual_.reset();  // The primary hook has now drained and cannot touch this object again.
  if (encoder_ != nullptr) {
    OH_VideoEncoder_Stop(encoder_);
    OH_VideoEncoder_Destroy(encoder_);
    encoder_ = nullptr;
  }
  if (window_) OH_NativeWindow_DestroyNativeWindow(window_);
  window_ = nullptr;
  encW_ = encH_ = 0;
  orientation_ = 0;
  // Producer and codec callbacks have stopped. Neither an unconsumed NV12
  // frame nor the repeat-frame slot belongs to the next encoder geometry.
  std::lock_guard<std::mutex> lk(pendingMu_);
  hasPending_ = false;
  freshInputOnly_ = false;
  pendingFrame_.clear();
  lastFrame_.clear();
}

bool CameraStreamer::Start(int width, int height, int bitrate, const char* mimeType,
                           bool frontCamera, int iFrameIntervalMs,
                           uint64_t previewSurfaceId, bool osdEnabled, FrameSink sink,
                           ErrorSink onError, const std::string& deviceId,
                           const std::string& encoderName, int avcProfile,
                           CaptureOrientation orientation) {
  if (running_.load() || !orientation.Valid()) return false;
  RunConfig config;
  config.orientation = orientation;
  config.width = width;
  config.height = height;
  config.bitrate = bitrate;
  config.iFrameMs = iFrameIntervalMs > 0 ? iFrameIntervalMs : 2000;
  config.mime = mimeType ? mimeType : "video/avc";
  config.front = frontCamera;
  config.deviceId = deviceId;
  config.encoderName = encoderName;
  config.avcProfile = avcProfile;
  config.osd = osdEnabled;
  config.previewId = previewSurfaceId ? previewSurfaceId : config_.previewId;
  return StartRun(config, std::move(sink), std::move(onError), true);
}

bool CameraStreamer::StartRun(RunConfig config, FrameSink sink, ErrorSink onError,
                              bool allowOsdFallback) {
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = std::move(sink);
    onError_ = std::move(onError);
  }
  const int width = config.width, height = config.height;
  running_ = true;

  const bool useDual = config.dualLayout != 0;
  Camera_ErrorCode err = CAMERA_OK;
  if (!useDual) err = OH_Camera_GetCameraManager(&manager_);
  CameraProfiles lists(manager_);
  auto fail = [&](const std::string& step, int code) {
    Fail(step, code);
    lists.Free();  // SDK lists must be freed before Cleanup releases manager_.
    Cleanup();
    return false;
  };
  const Camera_Device* chosen = nullptr;
  Camera_Profile profile{};
  if (!useDual) {
    if (err != CAMERA_OK || !manager_) return fail("GetCameraManager", err);
    const char* step = nullptr;
    err = lists.Load(config.front, step, config.deviceId);
    if (err != CAMERA_OK) return fail(step, err);
    const Camera_Profile* exact = lists.Exact(width, height);
    if (!exact) return fail("Unsupported preview resolution " + std::to_string(width) + "x" +
                            std::to_string(height), CAMERA_INVALID_ARGUMENT);
    chosen = lists.Device();  // borrowed through CreateCameraInput
    profile = *exact;
    config.deviceId = chosen->cameraId;
  }
  bool requestCbr = false;
  if (!CheckEncoderChoice(config.mime, config.encoderName, config.avcProfile, config.bitrate, requestCbr)) {
    return fail("Unsupported encoder, bitrate or AVC profile", CAMERA_INVALID_ARGUMENT);
  }
  encoder_ = config.encoderName.empty() ? OH_VideoEncoder_CreateByMime(config.mime.c_str()) :
      OH_VideoEncoder_CreateByName(config.encoderName.c_str());
  if (!encoder_) return fail("CreateVideoEncoder", 0);
  OH_AVCodecCallback cb{};
  cb.onError = OnError;
  cb.onStreamChanged = OnStreamChanged;
  cb.onNeedInputBuffer = OnNeedInputBuffer;
  cb.onNewOutputBuffer = OnNewOutputBuffer;
  OH_AVErrCode aerr = OH_VideoEncoder_RegisterCallback(encoder_, cb, this);
  if (aerr != AV_ERR_OK) return fail("EncoderRegisterCallback", aerr);
  // Pick the input path before configuring the encoder: the buffer (OSD) path
  // must declare its pixel format, the surface path must not (the format then
  // travels on the surface buffers, and an explicit buffer format makes the
  // codec expect CPU input instead). The OSD pipeline only touches the camera,
  // so starting it here is independent of the encoder configuration below.
  uint64_t cameraSurfaceId = 0;
  bool osdOwnsPreview = false;
  const bool requirePixels = config.orientation.RequiresPixels();
  if (config.osd || useDual || requirePixels) {
    osd_ = std::make_unique<OsdPipeline>();
    if (useDual) {
      dual_ = std::make_unique<DualCameraCapture>();
      osd_->SetPreOverlay([capture = dual_.get()](uint8_t* frame, size_t bytes, int w, int h) {
        return capture->Composite(frame, bytes, w, h);
      });
    }
    bool ok = osd_->Start(
        width, height, config.previewId,
        &cameraSurfaceId,
        [this](const uint8_t* nv12, size_t size) {
          std::lock_guard<std::mutex> lk(pendingMu_);
          pendingFrame_.assign(nv12, nv12 + size);
          hasPending_ = true;
          pendingCv_.notify_one();
        },
        [](const std::string& err) {
          OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "osd: %{public}s",
                       err.c_str());
        }, config.orientation);
    if (ok && cameraSurfaceId != 0) {
      osdOwnsPreview = config.previewId != 0;
      if (!allowOsdFallback && osdOwnsPreview && !osd_->SetUiSurface(config.previewId)) {
        return fail("Restore OSD preview", CAMERA_INVALID_ARGUMENT);
      }
      // Never feed a user's baked-in angle into a later raw recording.
      if (!useDual && config.previewId && config.orientation.rotation == CaptureRotation::Auto) {
        g_lastKnownRotation.store(osd_->rotation());
      }
      // Already-rotated pixels: rotation metadata would double-rotate.
      orientation_.store(0);
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "OSD CPU pipeline active, buffer-mode encoder rot=%{public}d "
                   "out=%{public}dx%{public}d (camera surface %{public}llu)",
                   osd_->rotation(), osd_->OutWidth(), osd_->OutHeight(),
                   static_cast<unsigned long long>(cameraSurfaceId));
    } else {
      if (useDual || requirePixels || !allowOsdFallback) return fail("Start OSD pipeline", CAMERA_INVALID_ARGUMENT);
      config.osd = false;  // Preserve the actual path on later reconfiguration.
      OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                   "OSD pipeline init failed, streaming without OSD");
      osd_.reset();
      cameraSurfaceId = 0;
    }
  }
  int encW = width;
  int encH = height;
  if (cameraSurfaceId != 0) {
    // Rotated output: 90/270 swaps the encoder dimensions.
    encW = osd_->OutWidth();
    encH = osd_->OutHeight();
  } else {
    orientation_.store(g_lastKnownRotation.load());
  }
  encW_ = encW;
  encH_ = encH;
  OH_AVFormat* format = OH_AVFormat_Create();
  if (!format) return fail("CreateEncoderFormat", 0);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, encW);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, encH);
  // The SDK bitrate key is int64_t; I-frame interval is int32 milliseconds.
  if (!OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, config.bitrate) ||
      !OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, config.iFrameMs) ||
      (requestCbr && !OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE, BITRATE_MODE_CBR)) ||
      (config.avcProfile >= 0 && !OH_AVFormat_SetIntValue(format, OH_MD_KEY_PROFILE, config.avcProfile))) {
    OH_AVFormat_Destroy(format);
    return fail("Set encoder options", CAMERA_INVALID_ARGUMENT);
  }
  OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, 30.0);
  if (cameraSurfaceId != 0) {
    // Buffer mode: pin the input layout to the tightly-packed NV12 the OSD
    // pipeline produces. Without this the codec falls back to its default
    // input format and reads the buffer as 4-byte pixels, which turns the
    // picture into 4 squashed horizontal copies with a black bottom half.
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
  }
  aerr = OH_VideoEncoder_Configure(encoder_, format);
  OH_AVFormat_Destroy(format);
  if (aerr != AV_ERR_OK) return fail("EncoderConfigure", aerr);
  if (cameraSurfaceId == 0) {
    // Surface mode: camera renders into the encoder's producer surface.
    aerr = OH_VideoEncoder_GetSurface(encoder_, &window_);
    if (aerr != AV_ERR_OK || window_ == nullptr) return fail("EncoderGetSurface", aerr);
  }
  aerr = OH_VideoEncoder_Prepare(encoder_);
  if (aerr != AV_ERR_OK) return fail("EncoderPrepare", aerr);
  bufferInput_ = cameraSurfaceId != 0;
  if (useDual) {
    {
      std::lock_guard<std::mutex> lk(pendingMu_);
      freshInputOnly_ = true;
    }
    // Overlapping cameras cannot be inferred from two single-camera profiles.
    // The dual owner opens both sessions using the pair's concurrent plan.
    if (!dual_->Start(config.dualPlan, config.dualLayout, cameraSurfaceId, osd_->rotation())) {
      return fail("Start concurrent cameras", CAMERA_INVALID_ARGUMENT);
    }
    // Wait before starting the buffer-input codec: dropping all early input
    // callbacks while the second camera warms up could starve this encoder.
    constexpr auto timeout = std::chrono::milliseconds(3000);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!dual_->WaitForComposite(timeout)) {
      return fail("No composite from concurrent cameras", CAMERA_INVALID_ARGUMENT);
    }
    // Composite signals before OSD draws. Do not consume an input callback
    // until at least the first finished frame has actually reached the slot.
    std::unique_lock<std::mutex> lk(pendingMu_);
    if (!pendingCv_.wait_until(lk, deadline, [this] { return hasPending_; })) {
      lk.unlock();
      return fail("No composite delivered to encoder input", CAMERA_INVALID_ARGUMENT);
    }
  }
  if (formatSink_) formatSink_(encW, encH);
  aerr = OH_VideoEncoder_Start(encoder_);
  if (aerr != AV_ERR_OK) return fail("EncoderStart", aerr);
  if (useDual) {
    config_ = std::move(config);
    return true;
  }
  uint64_t surfaceId = cameraSurfaceId;
  if (surfaceId == 0) {
    OH_NativeWindow_GetSurfaceId(window_, &surfaceId);
  }
  char sidStr[32];
  snprintf(sidStr, sizeof(sidStr), "%llu", static_cast<unsigned long long>(surfaceId));

  err = OH_CameraManager_CreatePreviewOutput(manager_, &profile, sidStr, &preview_);
  if (err != CAMERA_OK || preview_ == nullptr) return fail("CreatePreviewOutput", err);
  err = OH_CameraManager_CreateCameraInput(manager_, chosen, &input_);
  if (err != CAMERA_OK || input_ == nullptr) return fail("CreateCameraInput", err);
  lists.Free();
  err = OH_CameraInput_Open(input_);
  if (err != CAMERA_OK) return fail("CameraInputOpen (permission?)", err);
  err = OH_CameraManager_CreateCaptureSession(manager_, &session_);
  if (err != CAMERA_OK || session_ == nullptr) return fail("CreateCaptureSession", err);
  err = OH_CaptureSession_BeginConfig(session_);
  if (err == CAMERA_OK) err = OH_CaptureSession_AddInput(session_, input_);
  if (err == CAMERA_OK) err = OH_CaptureSession_AddPreviewOutput(session_, preview_);
  if (err == CAMERA_OK && config.previewId && !osdOwnsPreview) {
    const std::string uiId = std::to_string(config.previewId);
    err = OH_CameraManager_CreatePreviewOutput(manager_, &profile, uiId.c_str(), &previewUi_);
    if (err == CAMERA_OK && !previewUi_) return fail("Create UI preview", CAMERA_INVALID_ARGUMENT);
    if (err == CAMERA_OK) err = OH_CaptureSession_AddPreviewOutput(session_, previewUi_);
  }
  if (err == CAMERA_OK) err = OH_CaptureSession_CommitConfig(session_);
  if (err != CAMERA_OK) return fail("CaptureSessionConfig", err);
  err = OH_PreviewOutput_Start(preview_);
  if (err != CAMERA_OK) return fail("PreviewOutputStart", err);
  if (previewUi_) {
    err = OH_PreviewOutput_Start(previewUi_);
    if (err != CAMERA_OK) return fail("UI preview start", err);
  }
  config_ = std::move(config);  // Commit only after all outputs started.
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "camera streaming %{public}ux%{public}u -> encoder %{public}dx%{public}d",
               profile.size.width, profile.size.height, encW, encH);
  return true;
}

bool CameraStreamer::SetTorch(bool on) {
  if (manager_ == nullptr) return false;
  Camera_ErrorCode err =
      OH_CameraManager_SetTorchMode(manager_, on ? Camera_TorchMode(1) : Camera_TorchMode(0));
  if (err != CAMERA_OK) {
    LogErr("SetTorchMode", err);
    return false;
  }
  return true;
}

std::vector<std::pair<int, int>> CameraStreamer::AvailableResolutions() const {
  if (!IsRunning() || DualLayout()) return {};
  CameraProfiles profiles(manager_);
  const char* step = nullptr;
  if (profiles.Load(config_.front, step, config_.deviceId) != CAMERA_OK) return {};
  return profiles.Resolutions();
}

CameraStreamer::ResizeResult CameraStreamer::ChangeResolution(int width, int height) {
  if (!IsRunning()) return ResizeResult::Stopped;
  if (DualLayout()) return ResizeResult::Rejected;
  if (width == config_.width && height == config_.height) return ResizeResult::Unchanged;
  {
    CameraProfiles profiles(manager_);
    const char* step = nullptr;
    if (profiles.Load(config_.front, step, config_.deviceId) != CAMERA_OK ||
        !profiles.Exact(width, height)) return ResizeResult::Rejected;
  }
  RunConfig next = config_;
  next.width = width;
  next.height = height;
  return ReplaceRun(next);
}

CameraStreamer::ResizeResult CameraStreamer::SetDualLayout(int layout, const DualCameraPlan* plan) {
  if (!IsRunning()) return ResizeResult::Stopped;
  if (layout < 0 || layout > 3) return ResizeResult::Rejected;
  RunConfig next = config_;
  if (layout == 0) {
    if (!config_.dualLayout) return ResizeResult::Unchanged;
    next.dualPlan = {};
  } else {
    if (!plan && config_.dualLayout) plan = &config_.dualPlan;
    if (!plan || plan->primaryId.empty() || plan->primaryId != config_.deviceId ||
        plan->width != config_.width || plan->height != config_.height ||
        plan->secondaryId.empty() || plan->primaryId == plan->secondaryId ||
        plan->primaryType < 0 || plan->primaryType > 1 ||
        plan->secondaryType < 0 || plan->secondaryType > 1 ||
        plan->secondaryWidth <= 0 || plan->secondaryHeight <= 0 ||
        ((plan->secondaryWidth | plan->secondaryHeight) & 1)) return ResizeResult::Rejected;
    const auto& prev = config_.dualPlan;
    if (layout == config_.dualLayout && plan->secondaryId == prev.secondaryId &&
        plan->primaryType == prev.primaryType && plan->secondaryType == prev.secondaryType &&
        plan->secondaryWidth == prev.secondaryWidth && plan->secondaryHeight == prev.secondaryHeight) {
      return ResizeResult::Unchanged;
    }
    next.dualPlan = *plan;
  }
  next.dualLayout = layout;  // Keep single-camera osd, codec settings and latest preview id.
  return ReplaceRun(next);
}

CameraStreamer::ResizeResult CameraStreamer::ReplaceRun(const RunConfig& next) {
  const RunConfig previous = config_;
  FrameSink sink;
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink = sink_;
    onError = onError_;
  }
  Stop();
  if (StartRun(next, sink, onError, false)) return ResizeResult::Changed;
  if (StartRun(previous, sink, onError, false)) return ResizeResult::Restored;
  // Concurrency can disappear altogether (e.g. one lens became unavailable).
  // If both dual layouts failed, try the pinned primary on its original path.
  if (next.dualLayout && previous.dualLayout && next.dualLayout != previous.dualLayout) {
    RunConfig single = previous;
    single.dualLayout = 0;
    single.dualPlan = {};
    if (StartRun(single, sink, onError, false)) return ResizeResult::Restored;
  }
  return ResizeResult::Stopped;
}

bool CameraStreamer::SwitchFacing() {
  if (!IsRunning() || DualLayout()) return false;
  RunConfig next = config_;
  next.front = !next.front;
  {
    CameraProfiles profiles(manager_);
    const char* step = nullptr;
    if (profiles.Load(next.front, step) != CAMERA_OK || !profiles.Exact(next.width, next.height)) {
      return false;
    }
    next.deviceId = profiles.Device()->cameraId;
  }
  return ReplaceRun(next) == ResizeResult::Changed;
}

bool CameraStreamer::Restart() {
  return IsRunning() && ReplaceRun(config_) != ResizeResult::Stopped;
}

bool CameraStreamer::AttachPreview(uint64_t surfaceId) {
  if (surfaceId == 0) {
    config_.previewId = 0;
    if (osd_) osd_->ClearUiSurface();
    return true;
  }
  if (osd_ != nullptr) {
    // OSD mode: the pipeline owns the local preview and adopts surfaces live
    // (switching pages destroys and recreates the XComponent), so no camera
    // restart is needed here. Keep the id pending so a later start without OSD
    // re-attaches the camera output to the same surface.
    if (!osd_->SetUiSurface(surfaceId)) return false;
    config_.previewId = surfaceId;
    // Attaching a surface does not change the run's baked rotation. In
    // particular, don't publish that angle as raw MP4 metadata here.
    return true;
  }
  if (previewUi_ && config_.previewId == surfaceId) return true;
  if (!IsRunning()) {
    config_.previewId = surfaceId;
    return true;
  }
  // A live BeginConfig/CommitConfig reconfig breaks the encoder's surface
  // producer (frames stop). Restart the whole camera with both surfaces
  // configured in one pass instead.
  RunConfig next = config_;
  next.previewId = surfaceId;
  return ReplaceRun(next) == ResizeResult::Changed;
}

void CameraStreamer::Stop() {
  if (!running_.exchange(false)) return;
  Cleanup();
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "camera streaming stopped");
}

}  // namespace ipcam
