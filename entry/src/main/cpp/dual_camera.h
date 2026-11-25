#ifndef IPCAMERA_DUAL_CAMERA_H
#define IPCAMERA_DUAL_CAMERA_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ipcam {
// Obtained from the public CameraManager.getCameraConcurrentInfos for this exact
// pair, not independently enumerated single-camera capabilities. NORMAL_VIDEO only.
struct DualCameraPlan {
  std::string primaryId, secondaryId;
  int primaryType = -1, secondaryType = -1;
  int width = 0, height = 0, secondaryWidth = 0, secondaryHeight = 0;
};

class DualCameraCapture {
 public:
  DualCameraCapture();
  ~DualCameraCapture();
  DualCameraCapture(const DualCameraCapture&) = delete;
  DualCameraCapture& operator=(const DualCameraCapture&) = delete;

  // One instance per attempt. Control operations are serialized by the caller.
  // Install Composite on the primary OSD pipeline BEFORE starting this method.
  bool Start(const DualCameraPlan& plan, int layout, uint64_t primarySurfaceId, int primaryRotation);
  bool Composite(uint8_t* primaryNv12, size_t bytes, int outW, int outH);
  bool WaitForComposite(std::chrono::milliseconds timeout);
  void Stop(); // Stop both camera producers before stopping the primary OSD worker.
 private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace ipcam
#endif
