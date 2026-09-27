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

#include "osd_pipeline.h"

#define LOG_DOMAIN 0xC010
#define LOG_TAG "CameraStreamer"

namespace ipcam {
namespace {

void LogErr(const char* what, int err) {
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "%{public}s err=%{public}d", what, err);
}

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

void CameraStreamer::OnNeedInputBuffer(OH_AVCodec*, uint32_t index,
                                       OH_AVBuffer* buffer, void* userData) {
  // Buffer mode (OSD on): pull the latest composited NV12 frame and push it.
  // Surface mode (OSD off) never gets input callbacks (camera feeds directly).
  auto self = static_cast<CameraStreamer*>(userData);
  if (self->osd_ == nullptr || !self->running_.load()) return;
  std::vector<uint8_t> frame;
  {
    std::unique_lock<std::mutex> lk(self->pendingMu_);
    self->pendingCv_.wait_for(lk, std::chrono::milliseconds(200),
                              [&self] { return self->hasPending_ || !self->running_.load(); });
    if (self->hasPending_) {
      frame.swap(self->pendingFrame_);
      self->hasPending_ = false;
      self->lastFrame_ = frame;  // repeat-push fallback keeps the encoder fed
    } else if (!self->lastFrame_.empty()) {
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
  OH_VideoEncoder_PushInputBuffer(self->encoder_, index);
}

void CameraStreamer::OnNewOutputBuffer(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer,
                                       void* userData) {
  auto self = static_cast<CameraStreamer*>(userData);
  OH_AVCodecBufferAttr attr{};
  OH_AVBuffer_GetBufferAttr(buffer, &attr);
  if (!(attr.flags & AVCODEC_BUFFER_FLAGS_EOS) && attr.size > 0) {
    FrameSink sink;
    {
      std::lock_guard<std::mutex> lk(self->stateMu_);
      sink = self->sink_;
    }
    if (sink) {
      sink(OH_AVBuffer_GetAddr(buffer), static_cast<size_t>(attr.size),
           static_cast<uint64_t>(attr.pts));
    }
  }
  OH_VideoEncoder_FreeOutputBuffer(self->encoder_, index);
}

void CameraStreamer::Cleanup() {
  // The OSD pipeline writes into the encoder window; shut it down before the
  // encoder (its consumer) goes away.
  if (osd_ != nullptr) {
    osd_->Stop();
    osd_.reset();
    pendingCv_.notify_all();  // wake a blocked buffer-mode input callback
  }
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
  if (encoder_ != nullptr) {
    OH_VideoEncoder_Stop(encoder_);
    OH_VideoEncoder_Destroy(encoder_);
    encoder_ = nullptr;
  }
  window_ = nullptr;
}

bool CameraStreamer::Start(int width, int height, int bitrate, const char* mimeType,
                           bool frontCamera, int iFrameIntervalMs,
                           uint64_t previewSurfaceId, bool osdEnabled, FrameSink sink,
                           ErrorSink onError) {
  if (running_.load()) return false;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = std::move(sink);
    onError_ = std::move(onError);
  }
  lastWidth_ = width;
  lastHeight_ = height;
  lastBitrate_ = bitrate;
  lastIFrameMs_ = iFrameIntervalMs > 0 ? iFrameIntervalMs : 2000;
  lastMime_ = mimeType;
  lastFront_ = frontCamera;
  lastOsd_ = osdEnabled;
  if (previewSurfaceId != 0) {
    pendingPreviewId_ = previewSurfaceId;
    hasPendingPreview_ = true;
  }
  running_ = true;

  Camera_ErrorCode err = OH_Camera_GetCameraManager(&manager_);
  if (err != CAMERA_OK || manager_ == nullptr) {
    Fail("GetCameraManager", err);
    Cleanup();
    running_ = false;
    return false;
  }

  Camera_Device* devices = nullptr;
  uint32_t devCount = 0;
  err = OH_CameraManager_GetSupportedCameras(manager_, &devices, &devCount);
  if (err != CAMERA_OK || devices == nullptr || devCount == 0) {
    Fail("GetSupportedCameras", err);
    Cleanup();
    running_ = false;
    return false;
  }
  Camera_Position wanted =
      frontCamera ? CAMERA_POSITION_FRONT : CAMERA_POSITION_BACK;
  Camera_Device* chosen = &devices[0];
  for (uint32_t i = 0; i < devCount; ++i) {
    if (devices[i].cameraPosition == wanted) {
      chosen = &devices[i];
      break;
    }
  }

  Camera_OutputCapability* capability = nullptr;
  err = OH_CameraManager_GetSupportedCameraOutputCapability(manager_, chosen, &capability);
  if (err != CAMERA_OK || capability == nullptr || capability->previewProfilesSize == 0) {
    Fail("GetSupportedCameraOutputCapability", err);
    OH_CameraManager_DeleteSupportedCameras(manager_, devices, devCount);
    Cleanup();
    running_ = false;
    return false;
  }
  // Pick the preview profile closest to the requested size (prefer >=).
  Camera_Profile profile = *capability->previewProfiles[0];
  long bestScore = -1;
  for (uint32_t i = 0; i < capability->previewProfilesSize; ++i) {
    const Camera_Profile* p = capability->previewProfiles[i];
    long w = static_cast<long>(p->size.width);
    long h = static_cast<long>(p->size.height);
    long score = (w >= width && h >= height ? 0 : 1000000) + (w - width) * (w - width) +
                 (h - height) * (h - height);
    if (bestScore < 0 || score < bestScore) {
      bestScore = score;
      profile = *p;
    }
  }
  // NOTE: devices/capability stay alive until the camera input is created below
  // (chosen points into the devices array).

  encoder_ = OH_VideoEncoder_CreateByMime(mimeType);
  if (encoder_ == nullptr) {
    Fail("CreateVideoEncoder", 0);
    Cleanup();
    running_ = false;
    return false;
  }
  OH_AVCodecCallback cb{};
  cb.onError = OnError;
  cb.onStreamChanged = OnStreamChanged;
  cb.onNeedInputBuffer = OnNeedInputBuffer;
  cb.onNewOutputBuffer = OnNewOutputBuffer;
  OH_AVErrCode aerr = OH_VideoEncoder_RegisterCallback(encoder_, cb, this);
  if (aerr != AV_ERR_OK) {
    Fail("EncoderRegisterCallback", aerr);
    Cleanup();
    running_ = false;
    return false;
  }
  OH_AVFormat* format = OH_AVFormat_Create();
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_BITRATE, bitrate);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE, BITRATE_MODE_CBR);
  OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, 30.0);
  // Unit is MILLISECONDS per the NDK header (an Android-style seconds value of
  // 2 here would make every frame a keyframe).
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, lastIFrameMs_);
  aerr = OH_VideoEncoder_Configure(encoder_, format);
  OH_AVFormat_Destroy(format);
  if (aerr != AV_ERR_OK) {
    Fail("EncoderConfigure", aerr);
    Cleanup();
    running_ = false;
    return false;
  }
  aerr = OH_VideoEncoder_Prepare(encoder_);
  if (aerr != AV_ERR_OK) {
    Fail("EncoderPrepare", aerr);
    Cleanup();
    running_ = false;
    return false;
  }
  // OSD path: the CPU compositing pipeline receives the camera frames and
  // feeds the encoder in BUFFER mode (OnNeedInputBuffer below). Direct path:
  // the camera renders straight into the encoder's producer surface. The
  // codec HDI on this device rejects producer-side Request/FlushBuffer on the
  // encoder surface, which is why OSD cannot reuse the surface path.
  uint64_t cameraSurfaceId = 0;
  if (osdEnabled) {
    osd_ = std::make_unique<OsdPipeline>();
    bool ok = osd_->Start(
        width, height, &cameraSurfaceId,
        [this](const uint8_t* nv12, size_t size) {
          std::lock_guard<std::mutex> lk(pendingMu_);
          pendingFrame_.assign(nv12, nv12 + size);
          hasPending_ = true;
          pendingCv_.notify_one();
        },
        [](const std::string& err) {
          OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "osd: %{public}s",
                       err.c_str());
        });
    if (ok) {
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "OSD CPU pipeline active, buffer-mode encoder (camera surface %{public}llu)",
                   static_cast<unsigned long long>(cameraSurfaceId));
    } else {
      OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                   "OSD pipeline init failed, streaming without OSD");
      osd_.reset();
      cameraSurfaceId = 0;
    }
  }
  if (cameraSurfaceId == 0) {
    // Surface mode: camera renders into the encoder's producer surface.
    aerr = OH_VideoEncoder_GetSurface(encoder_, &window_);
    if (aerr != AV_ERR_OK || window_ == nullptr) {
      Fail("EncoderGetSurface", aerr);
      Cleanup();
      running_ = false;
      return false;
    }
  }
  aerr = OH_VideoEncoder_Start(encoder_);
  if (aerr != AV_ERR_OK) {
    Fail("EncoderStart", aerr);
    Cleanup();
    running_ = false;
    return false;
  }
  uint64_t surfaceId = cameraSurfaceId;
  if (surfaceId == 0) {
    OH_NativeWindow_GetSurfaceId(window_, &surfaceId);
  }
  char sidStr[32];
  snprintf(sidStr, sizeof(sidStr), "%llu", static_cast<unsigned long long>(surfaceId));

  err = OH_CameraManager_CreatePreviewOutput(manager_, &profile, sidStr, &preview_);
  if (err != CAMERA_OK || preview_ == nullptr) {
    Fail("CreatePreviewOutput", err);
    Cleanup();
    running_ = false;
    return false;
  }
  err = OH_CameraManager_CreateCameraInput(manager_, chosen, &input_);
  if (err != CAMERA_OK || input_ == nullptr) {
    Fail("CreateCameraInput", err);
    OH_CameraManager_DeleteSupportedCameras(manager_, devices, devCount);
    OH_CameraManager_DeleteSupportedCameraOutputCapability(manager_, capability);
    Cleanup();
    running_ = false;
    return false;
  }
  OH_CameraManager_DeleteSupportedCameras(manager_, devices, devCount);
  OH_CameraManager_DeleteSupportedCameraOutputCapability(manager_, capability);
  err = OH_CameraInput_Open(input_);
  if (err != CAMERA_OK) {
    Fail("CameraInputOpen (permission?)", err);
    Cleanup();
    running_ = false;
    return false;
  }
  err = OH_CameraManager_CreateCaptureSession(manager_, &session_);
  if (err != CAMERA_OK || session_ == nullptr) {
    Fail("CreateCaptureSession", err);
    Cleanup();
    running_ = false;
    return false;
  }
  uiProfile_ = profile;
  err = OH_CaptureSession_BeginConfig(session_);
  if (err == CAMERA_OK) err = OH_CaptureSession_AddInput(session_, input_);
  if (err == CAMERA_OK) err = OH_CaptureSession_AddPreviewOutput(session_, preview_);
  if (err == CAMERA_OK && hasPendingPreview_) {
    char sidStr2[32];
    snprintf(sidStr2, sizeof(sidStr2), "%llu",
             static_cast<unsigned long long>(pendingPreviewId_));
    err = OH_CameraManager_CreatePreviewOutput(manager_, &profile, sidStr2, &previewUi_);
    if (err == CAMERA_OK && previewUi_ != nullptr) {
      err = OH_CaptureSession_AddPreviewOutput(session_, previewUi_);
    }
    hasPendingPreview_ = false;
  }
  if (err == CAMERA_OK) err = OH_CaptureSession_CommitConfig(session_);
  if (err != CAMERA_OK) {
    Fail("CaptureSessionConfig", err);
    Cleanup();
    running_ = false;
    return false;
  }
  err = OH_PreviewOutput_Start(preview_);
  if (err != CAMERA_OK) {
    Fail("PreviewOutputStart", err);
    Cleanup();
    running_ = false;
    return false;
  }
  if (previewUi_ != nullptr) {
    OH_PreviewOutput_Start(previewUi_);
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "camera streaming %{public}ux%{public}u -> encoder %{public}dx%{public}d",
               profile.size.width, profile.size.height, width, height);
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

bool CameraStreamer::SwitchFacing() {
  if (!running_.load()) return false;
  FrameSink sink;
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink = sink_;
    onError = onError_;
  }
  bool next = !lastFront_;
  Stop();
  return Start(lastWidth_, lastHeight_, lastBitrate_, lastMime_.c_str(), next,
               lastIFrameMs_, pendingPreviewId_, lastOsd_, sink, onError);
}

bool CameraStreamer::Restart() {
  if (!running_.load()) return false;
  FrameSink sink;
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink = sink_;
    onError = onError_;
  }
  Stop();
  return Start(lastWidth_, lastHeight_, lastBitrate_, lastMime_.c_str(), lastFront_,
               lastIFrameMs_, pendingPreviewId_, lastOsd_, sink, onError);
}

bool CameraStreamer::AttachPreview(uint64_t surfaceId) {
  if (previewUi_ != nullptr) return true;  // already attached
  if (!running_.load() || session_ == nullptr || manager_ == nullptr) {
    pendingPreviewId_ = surfaceId;
    hasPendingPreview_ = true;
    return true;
  }
  // A live BeginConfig/CommitConfig reconfig breaks the encoder's surface
  // producer (frames stop). Restart the whole camera with both surfaces
  // configured in one pass instead.
  FrameSink sink;
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink = sink_;
    onError = onError_;
  }
  Stop();
  pendingPreviewId_ = surfaceId;
  hasPendingPreview_ = true;
  return Start(lastWidth_, lastHeight_, lastBitrate_, lastMime_.c_str(), lastFront_,
               lastIFrameMs_, surfaceId, lastOsd_, sink, onError);
  char sidStr[32];
  snprintf(sidStr, sizeof(sidStr), "%llu", static_cast<unsigned long long>(surfaceId));
  Camera_PreviewOutput* out = nullptr;
  Camera_ErrorCode err =
      OH_CameraManager_CreatePreviewOutput(manager_, &uiProfile_, sidStr, &out);
  if (err != CAMERA_OK || out == nullptr) {
    LogErr("AttachPreview: CreatePreviewOutput", err);
    return false;
  }
  err = OH_CaptureSession_BeginConfig(session_);
  if (err == CAMERA_OK) err = OH_CaptureSession_AddPreviewOutput(session_, out);
  if (err == CAMERA_OK) err = OH_CaptureSession_CommitConfig(session_);
  if (err == CAMERA_OK) err = OH_PreviewOutput_Start(out);
  if (err != CAMERA_OK) {
    LogErr("AttachPreview: config/start", err);
    OH_PreviewOutput_Release(out);
    return false;
  }
  previewUi_ = out;
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "UI preview attached");
  return true;
}

void CameraStreamer::Stop() {
  if (!running_.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = nullptr;
    onError_ = nullptr;
  }
  Cleanup();
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "camera streaming stopped");
}

}  // namespace ipcam
