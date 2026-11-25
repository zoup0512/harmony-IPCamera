#include "dual_camera.h"
#include "dual_camera_layout.h"
#include "osd_pipeline.h"

#include <cstdio>
#include <new>
#include <hilog/log.h>
#include <ohcamera/camera_manager.h>
#include <ohcamera/camera_input.h>
#include <ohcamera/capture_session.h>
#include <ohcamera/preview_output.h>

namespace ipcam {
namespace {
bool Ok(Camera_ErrorCode e, const char* operation) {
  if (e == CAMERA_OK) return true;
  OH_LOG_Print(LOG_APP, LOG_WARN, 0xc010, "DualCamera", "%{public}s: %{public}d", operation, static_cast<int>(e));
  return false;
}
bool ValidSize(int w, int h) {
  return w >= 4 && h >= 4 && w <= 4096 && h <= 4096 && !(w & 1) && !(h & 1) && w*h <= 4096*2160;
}
bool QuarterTurn(int rotation) { return rotation >= 0 && rotation < 360 && rotation % 90 == 0; }
struct Leg {
  Camera_Input* input = nullptr;
  Camera_CaptureSession* session = nullptr;
  Camera_PreviewOutput* output = nullptr;
  bool started = false;
  bool Open(Camera_Manager* manager, const Camera_Device* device, int type) {
    if (!Ok(OH_CameraManager_CreateCameraInput(manager, device, &input), "create input") || !input) return false;
    return Ok(OH_CameraInput_OpenConcurrentCameras(input, static_cast<Camera_ConcurrentType>(type)), "open concurrent input");
  }
  bool Configure(Camera_Manager* m, int w, int h, uint64_t id) {
    Camera_Profile profile{};
    profile.format = CAMERA_FORMAT_YUV_420_SP;
    profile.size = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    char sid[32];
    std::snprintf(sid, sizeof(sid), "%llu", static_cast<unsigned long long>(id));
    return Ok(OH_CameraManager_CreatePreviewOutput(m, &profile, sid, &output), "create output") && output &&
      Ok(OH_CameraManager_CreateCaptureSession(m, &session), "create session") && session &&
      Ok(OH_CaptureSession_SetSessionMode(session, NORMAL_VIDEO), "set normal video mode") &&
      Ok(OH_CaptureSession_BeginConfig(session), "begin config") &&
      Ok(OH_CaptureSession_AddInput(session, input), "add input") &&
      Ok(OH_CaptureSession_AddPreviewOutput(session, output), "add output") &&
      Ok(OH_CaptureSession_CommitConfig(session), "commit config");
  }
  bool Start() { return started = Ok(OH_CaptureSession_Start(session), "start session"); }
  void Stop() {
    if (started) OH_CaptureSession_Stop(session);
    started = false;
    if (session) OH_CaptureSession_Release(session);
    if (output) OH_PreviewOutput_Release(output);
    if (input) OH_CameraInput_Release(input); // Release includes closing the input.
    session = nullptr; output = nullptr; input = nullptr;
  }
};
} // namespace

struct DualCameraCapture::State {
  DualFrameGate gate;
  Camera_Manager* manager = nullptr;
  Camera_Device* devices = nullptr;
  uint32_t count = 0;
  Leg a, b;
  std::unique_ptr<OsdPipeline> reader;
  bool attempted = false;
  bool Begin(const DualCameraPlan& p, int layout, uint64_t surface, int primaryRot) {
    if (!surface || !QuarterTurn(primaryRot) || layout < 1 || layout > 3 ||
        !ValidSize(p.width,p.height) || !ValidSize(p.secondaryWidth,p.secondaryHeight) ||
        p.primaryId.empty() || p.secondaryId.empty() || p.primaryId == p.secondaryId ||
        (p.primaryType != 0 && p.primaryType != 1) || (p.secondaryType != 0 && p.secondaryType != 1)) return false;
    if (!Ok(OH_Camera_GetCameraManager(&manager), "get manager") || !manager ||
        !Ok(OH_CameraManager_GetSupportedCameras(manager, &devices, &count), "enumerate devices") || !devices) return false;
    const Camera_Device* first = nullptr;
    const Camera_Device* second = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
      if (!devices[i].cameraId) continue;
      if (p.primaryId == devices[i].cameraId) first = &devices[i];
      if (p.secondaryId == devices[i].cameraId) second = &devices[i];
    }
    if (!first || !second || !((first->cameraPosition == CAMERA_POSITION_BACK && second->cameraPosition == CAMERA_POSITION_FRONT) ||
        (first->cameraPosition == CAMERA_POSITION_FRONT && second->cameraPosition == CAMERA_POSITION_BACK))) return false;
    reader = std::make_unique<OsdPipeline>();
    reader->SetPreOverlay([this](uint8_t* pixels, size_t n, int w, int h){
      gate.Publish(pixels, n, w, h);
      return false; // secondary never burns OSD or sends its own frame to a sink
    });
    uint64_t secondSurface = 0;
    if (!reader->Start(p.secondaryWidth, p.secondaryHeight, 0, &secondSurface, {}, {}) ||
        !secondSurface || secondSurface == surface) return false;
    // Two distinct inputs, sessions, output surfaces. Neither vendor's ordinary
    // open nor a second PreviewOutput on the same input implies concurrency.
    if (!a.Open(manager, first, p.primaryType) || !b.Open(manager, second, p.secondaryType) ||
        !a.Configure(manager, p.width, p.height, surface) ||
        !b.Configure(manager, p.secondaryWidth, p.secondaryHeight, secondSurface)) return false;
    Camera_ImageRotation mainRot{}, subRot{};
    if (!Ok(OH_PreviewOutput_GetPreviewRotationWithoutDisplayRotation(a.output, &mainRot), "primary rotation") ||
        !Ok(OH_PreviewOutput_GetPreviewRotationWithoutDisplayRotation(b.output, &subRot), "secondary rotation") ||
        !QuarterTurn(mainRot) || !QuarterTurn(subRot)) return false;
    const int relativeRotation = (primaryRot + static_cast<int>(subRot) - static_cast<int>(mainRot) + 360) % 360;
    const int ow = primaryRot % 180 ? p.height : p.width, oh = primaryRot % 180 ? p.width : p.height;
    if (!gate.Configure(layout, ow, oh, p.secondaryWidth, p.secondaryHeight, relativeRotation)) return false;
    return b.Start() && a.Start();
  }
  void End() {
    gate.Close(); // cancel waiters and reject frames, without any SDK call under gate lock
    a.Stop(); b.Stop();
    if (reader) { reader->Stop(); reader.reset(); }
    if (devices) OH_CameraManager_DeleteSupportedCameras(manager, devices, count);
    devices = nullptr; count = 0;
    if (manager) OH_Camera_DeleteCameraManager(manager);
    manager = nullptr;
  }
};
DualCameraCapture::DualCameraCapture() : state_(new State) {}
DualCameraCapture::~DualCameraCapture() { Stop(); }
bool DualCameraCapture::Start(const DualCameraPlan& plan, int layout, uint64_t surface, int rotation) {
  if (state_->attempted) return false;
  state_->attempted = true;
  try {
    if (state_->Begin(plan, layout, surface, rotation)) return true;
  } catch (const std::bad_alloc&) {} // the caller restores its prior single-camera config
  Stop();
  return false;
}
bool DualCameraCapture::Composite(uint8_t* pixels, size_t size, int w, int h) {
  return state_->gate.Composite(pixels, size, w, h);
}
bool DualCameraCapture::WaitForComposite(std::chrono::milliseconds timeout) { return state_->gate.Wait(timeout); }
void DualCameraCapture::Stop() { state_->End(); }
} // namespace ipcam
