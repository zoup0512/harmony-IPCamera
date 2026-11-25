#ifndef IPCAMERA_CAMERA_CAPABILITIES_H
#define IPCAMERA_CAMERA_CAPABILITIES_H

#include <string>
#include <utility>
#include <vector>
#include <ohcamera/camera_manager.h>

namespace ipcam {

// Owns the SDK lists, not the manager. Release before deleting the manager.
// The chosen device and profile are borrowed until Free()/destruction.
class CameraProfiles {
 public:
  explicit CameraProfiles(Camera_Manager* manager) : manager_(manager) {}
  ~CameraProfiles() { Free(); }
  CameraProfiles(const CameraProfiles&) = delete;
  CameraProfiles& operator=(const CameraProfiles&) = delete;

  // Empty id selects the first lens of this facing (the settings selector).
  // A running stream pins its selected id even if CameraKit reorders devices.
  Camera_ErrorCode Load(bool front, const char*& failedStep, const std::string& id = {});
  std::vector<std::pair<int, int>> Resolutions() const;
  const Camera_Device* Device() const { return device_; }
  const Camera_Profile* Exact(int width, int height) const;
  std::string ResolutionsJson() const;
  void Free();

 private:
  Camera_Manager* manager_;
  Camera_Device* devices_ = nullptr;
  uint32_t count_ = 0;
  const Camera_Device* device_ = nullptr;
  Camera_OutputCapability* caps_ = nullptr;
};

// Does not open a camera. [] means there is no selectable resolution.
std::string CameraResolutionsJson(bool front, const std::string& deviceId = {});
std::string CameraDevicesJson();
std::string EncoderOptionsJson(const std::string& mime);

// Resolve only names reported for this MIME and direction. Never fall back to
// another implementation on an explicit choice. Query again for each start.
bool CheckEncoderChoice(const std::string& mime, const std::string& name, int profile,
                        int bitrate, bool& requestCbr);

}  // namespace ipcam
#endif  // IPCAMERA_CAMERA_CAPABILITIES_H
