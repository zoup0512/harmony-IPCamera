#include "camera_capabilities.h"
#include "camera_streamer.h"

#include <multimedia/player_framework/native_avcapability.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace ipcam {
namespace {

bool Usable(const Camera_Profile* p) {
  // The surface/OSD paths both consume 8-bit YUV420, not HDR P010. Use
  // the same predicate for the options shown and the profile passed to CameraKit.
  return p && p->format == CAMERA_FORMAT_YUV_420_SP && p->size.width && p->size.height &&
         p->size.width <= std::numeric_limits<int>::max() &&
         p->size.height <= std::numeric_limits<int>::max() &&
         p->size.width % 2 == 0 && p->size.height % 2 == 0;
}

using Range = std::pair<uint32_t, uint32_t>;
bool ValidRange(const Camera_FrameRateRange& r) {
  return r.min > 0 && r.min <= r.max && r.max <= std::numeric_limits<int32_t>::max();
}
std::vector<Range> SupportedRates(Camera_PreviewOutput* output) {
  std::vector<Range> result;
  if (!output) return result;
  Camera_FrameRateRange* list = nullptr;
  uint32_t count = 0;
  auto err = OH_PreviewOutput_GetSupportedFrameRates(output, &list, &count);
  if (err == CAMERA_OK && list) {
    for (uint32_t i = 0; i < count; ++i) {
      if (ValidRange(list[i])) result.emplace_back(list[i].min, list[i].max);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
  }
  if (list) OH_PreviewOutput_DeleteFrameRates(output, list);
  return result;
}
std::string RangeJson(uint32_t min, uint32_t max) {
  return "{\"min\":" + std::to_string(min) + ",\"max\":" + std::to_string(max) + "}";
}

}  // namespace

void CameraProfiles::Free() {
  if (caps_) OH_CameraManager_DeleteSupportedCameraOutputCapability(manager_, caps_);
  if (devices_) OH_CameraManager_DeleteSupportedCameras(manager_, devices_, count_);
  caps_ = nullptr;
  devices_ = nullptr;
  device_ = nullptr;
  count_ = 0;
}

Camera_ErrorCode CameraProfiles::Load(bool front, const char*& failedStep, const std::string& id) {
  Free();
  failedStep = "GetSupportedCameras";
  if (!manager_) return CAMERA_INVALID_ARGUMENT;
  auto err = OH_CameraManager_GetSupportedCameras(manager_, &devices_, &count_);
  if (err != CAMERA_OK) return err;
  failedStep = front ? "Front camera unavailable" : "Back camera unavailable";
  for (uint32_t i = 0; devices_ && i < count_; ++i) {
    if (devices_[i].cameraPosition == (front ? CAMERA_POSITION_FRONT : CAMERA_POSITION_BACK) &&
        devices_[i].cameraId && *devices_[i].cameraId && (id.empty() || id == devices_[i].cameraId)) {
      device_ = &devices_[i];
      break;
    }
  }
  if (!device_) return CAMERA_INVALID_ARGUMENT;
  failedStep = "GetSupportedCameraOutputCapability";
  err = OH_CameraManager_GetSupportedCameraOutputCapability(manager_, device_, &caps_);
  if (err != CAMERA_OK) return err;
  return caps_ && caps_->previewProfiles ? CAMERA_OK : CAMERA_INVALID_ARGUMENT;
}

const Camera_Profile* CameraProfiles::Exact(int width, int height) const {
  if (!caps_ || !caps_->previewProfiles || width <= 0 || height <= 0) return nullptr;
  for (uint32_t i = 0; i < caps_->previewProfilesSize; ++i) {
    const auto* p = caps_->previewProfiles[i];
    if (Usable(p) && p->size.width == static_cast<uint32_t>(width) &&
        p->size.height == static_cast<uint32_t>(height)) return p;
  }
  return nullptr;
}

std::vector<std::pair<int, int>> CameraProfiles::Resolutions() const {
  std::vector<std::pair<int, int>> sizes;
  if (!caps_ || !caps_->previewProfiles) return sizes;
  for (uint32_t i = 0; i < caps_->previewProfilesSize; ++i) {
    const auto* p = caps_->previewProfiles[i];
    if (!Usable(p)) continue;
    const std::pair<int, int> size{p->size.width, p->size.height};
    if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) sizes.push_back(size);
  }
  return sizes;
}

std::string CameraProfiles::ResolutionsJson() const {
  std::string json = "[";
  for (const auto& size : Resolutions()) {
    if (json.size() != 1) json += ',';
    json += "{\"width\":" + std::to_string(size.first) + ",\"height\":" +
            std::to_string(size.second) + "}";
  }
  return json + "]";
}

namespace {
std::string Quote(const char* text) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    if (*p < 0x20) {
      out += "\\u00"; out += hex[*p >> 4]; out += hex[*p & 15];
    } else {
      if (*p == '"' || *p == '\\') out += '\\';
      out += static_cast<char>(*p);
    }
  }
  return out + '"';
}

bool VideoMime(const std::string& mime) { return mime == "video/avc" || mime == "video/hevc"; }
// API 24 minimum: the category getter only returns a single preferred codec.
// Enumerate all non-secure encoders for this MIME, including multiple of one kind.
std::vector<OH_AVCapability*> Encoders(const std::string& mime) {
  std::vector<OH_AVCapability*> result;
  if (!VideoMime(mime)) return result;
  uint32_t count = 0;
  auto** list = OH_AVCodec_GetCapabilityList(OH_AVCODEC_TYPE_VIDEO_ENCODER, &count);
  for (uint32_t i = 0; list && i < count; ++i) {
    auto* cap = list[i];
    if (!cap || OH_AVCapability_IsSecure(cap) || !OH_AVCapability_CheckMimeType(cap, mime.c_str())) continue;
    const char* name = OH_AVCapability_GetName(cap);
    if (!name || !*name) continue;
    auto same = [name](OH_AVCapability* c) { return std::string(OH_AVCapability_GetName(c)) == name; };
    if (std::none_of(result.begin(), result.end(), same)) result.push_back(cap);
  }
  return result;  // SDK owns the list, names and descriptors.
}
std::vector<int> AvcProfiles(OH_AVCapability* cap, const std::string& mime) {
  std::vector<int> out;
  const int32_t* items = nullptr;
  uint32_t count = 0;
  if (mime == "video/avc" && OH_AVCapability_GetSupportedProfiles(cap, &items, &count) == AV_ERR_OK && items) {
    for (uint32_t i = 0; i < count; ++i) {
      const int p = items[i];
      if ((p == AVC_PROFILE_BASELINE || p == AVC_PROFILE_MAIN || p == AVC_PROFILE_HIGH) &&
          std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
  }
  return out;
}
OH_AVRange BitrateRange(OH_AVCapability* cap) {
  OH_AVRange range{};
  if (OH_AVCapability_GetEncoderBitrateRange(cap, &range) != AV_ERR_OK || range.minVal < 0 ||
      range.maxVal <= 0 || range.maxVal < range.minVal) return {};
  return range;
}

auto const focusModes = { FOCUS_MODE_MANUAL, FOCUS_MODE_CONTINUOUS_AUTO, FOCUS_MODE_AUTO, FOCUS_MODE_LOCKED };
bool SupportsFocus(Camera_CaptureSession* session, Camera_FocusMode mode) {
  bool supported = false;
  return session && OH_CaptureSession_IsFocusModeSupported(session, mode, &supported) == CAMERA_OK && supported;
}
}  // namespace

std::string CameraDevicesJson() {
  Camera_Manager* manager = nullptr;
  Camera_Device* devices = nullptr;
  uint32_t count = 0;
  std::string json = "[";
  if (OH_Camera_GetCameraManager(&manager) == CAMERA_OK && manager &&
      OH_CameraManager_GetSupportedCameras(manager, &devices, &count) == CAMERA_OK && devices) {
    for (uint32_t i = 0; i < count; ++i) {
      const auto& d = devices[i];
      if (!d.cameraId || !*d.cameraId ||
          (d.cameraPosition != CAMERA_POSITION_FRONT && d.cameraPosition != CAMERA_POSITION_BACK)) continue;
      Camera_OutputCapability* caps = nullptr;
      bool usable = OH_CameraManager_GetSupportedCameraOutputCapability(manager, &d, &caps) == CAMERA_OK &&
          caps && caps->previewProfiles && std::any_of(caps->previewProfiles,
          caps->previewProfiles + caps->previewProfilesSize, Usable);
      if (caps) OH_CameraManager_DeleteSupportedCameraOutputCapability(manager, caps);
      if (!usable) continue;
      if (json.size() > 1) json += ',';
      json += "{\"id\":" + Quote(d.cameraId) + ",\"front\":" + (d.cameraPosition == CAMERA_POSITION_FRONT ? "true}" : "false}");
    }
  }
  if (devices) OH_CameraManager_DeleteSupportedCameras(manager, devices, count);
  if (manager) OH_Camera_DeleteCameraManager(manager);
  return json + ']';
}

std::string EncoderOptionsJson(const std::string& mime) {
  if (!VideoMime(mime)) return "[]";
  std::string json = "[";
  for (auto* cap : Encoders(mime)) {
    const char* name = OH_AVCapability_GetName(cap);
    auto range = BitrateRange(cap);
    if (json.size() > 1) json += ',';
    json += "{\"name\":" + Quote(name) + ",\"hardware\":" + (OH_AVCapability_IsHardware(cap) ? "true" : "false") +
            ",\"minBitrate\":" + std::to_string(range.minVal) + ",\"maxBitrate\":" + std::to_string(range.maxVal) + ",\"profiles\":[";
    for (auto profile : AvcProfiles(cap, mime)) {
      if (json.back() != '[') json += ',';
      json += std::to_string(profile);
    }
    json += "]}";
  }
  return json + ']';
}

bool CheckEncoderChoice(const std::string& mime, const std::string& name, int profile, int bitrate, bool& requestCbr) {
  if (!VideoMime(mime) || bitrate <= 0 || profile < -1) return false;
  OH_AVCapability* cap = nullptr;
  if (name.empty()) {
    // Profiles belong to a specific enumerated implementation, not a MIME default.
    if (profile != -1) return false;
    cap = OH_AVCodec_GetCapability(mime.c_str(), true);
  } else {
    for (auto* candidate : Encoders(mime)) {
      if (name == OH_AVCapability_GetName(candidate)) { cap = candidate; break; }
    }
    if (!cap) return false;
  }
  requestCbr = cap && OH_AVCapability_IsEncoderBitrateModeSupported(cap, BITRATE_MODE_CBR);
  if (cap) {
    auto range = BitrateRange(cap);
    if (range.maxVal && (bitrate < range.minVal || bitrate > range.maxVal)) return false;
  }
  if (profile == -1) return true;
  auto profiles = AvcProfiles(cap, mime);
  return std::find(profiles.begin(), profiles.end(), profile) != profiles.end();
}

std::string CameraResolutionsJson(bool front, const std::string& deviceId) {
  Camera_Manager* manager = nullptr;
  auto err = OH_Camera_GetCameraManager(&manager);
  std::string result = "[]";
  if (err == CAMERA_OK && manager) {
    CameraProfiles profiles(manager);
    const char* step = nullptr;
    if (profiles.Load(front, step, deviceId) == CAMERA_OK) result = profiles.ResolutionsJson();
  }
  if (manager) OH_Camera_DeleteCameraManager(manager);
  return result;
}

std::string CameraStreamer::CaptureFpsJson() const {
  Camera_PreviewOutput* output = running_.load() ? preview_ : nullptr;
  std::string json = "{\"ranges\":[";
  for (const auto& r : SupportedRates(output)) {
    if (json.back() != '[') json += ',';
    json += RangeJson(r.first, r.second);
  }
  json += ']';
  Camera_FrameRateRange active{};
  if (output && OH_PreviewOutput_GetActiveFrameRate(output, &active) == CAMERA_OK &&
      ValidRange(active)) json += ",\"active\":" + RangeJson(active.min, active.max);
  return json + "}";
}

bool CameraStreamer::SetCaptureFps(int min, int max) {
  if (!running_.load() || !preview_ || min <= 0 || max < min) return false;
  const auto ranges = SupportedRates(preview_);
  if (std::find(ranges.begin(), ranges.end(), Range{min, max}) == ranges.end()) return false;
  if (OH_PreviewOutput_SetFrameRate(preview_, min, max) != CAMERA_OK) return false;
  config_.fpsMin = min;  // Survives resolution/lens/layout rebuilds of this run.
  config_.fpsMax = max;
  return true;
}

void CameraStreamer::ApplyCaptureControls() {
  // Best effort: a rebuilt profile or another lens may not offer the same
  // choices. Keep the request so a later rebuild can apply it again.
  if (config_.fpsMax > 0 && preview_) {
    const auto ranges = SupportedRates(preview_);
    const Range wanted{static_cast<uint32_t>(config_.fpsMin), static_cast<uint32_t>(config_.fpsMax)};
    if (std::find(ranges.begin(), ranges.end(), wanted) != ranges.end()) {
      OH_PreviewOutput_SetFrameRate(preview_, config_.fpsMin, config_.fpsMax);
    }
  }
  if (config_.focusMode >= 0 && session_) {
    for (auto candidate : focusModes) {
      if (config_.focusMode == candidate && SupportsFocus(session_, candidate)) {
        OH_CaptureSession_SetFocusMode(session_, candidate);
      }
    }
  }
}

std::string CameraStreamer::FocusInfoJson() const {
  auto* s = IsRunning() ? session_ : nullptr;
  std::string json = "{\"modes\":[";
  for (auto mode : focusModes) {
    if (!SupportsFocus(s, mode)) continue;
    if (json.back() != '[') json += ',';
    json += std::to_string(mode);
  }
  json += ']';
  Camera_FocusMode active{};
  if (s && OH_CaptureSession_GetFocusMode(s, &active) == CAMERA_OK &&
      std::find(focusModes.begin(), focusModes.end(), active) != focusModes.end()) {
    json += ",\"active\":" + std::to_string(active);
  }
  return json + '}';
}

bool CameraStreamer::SetFocusMode(int mode) {
  for (auto candidate : focusModes) {
    if (mode == candidate && IsRunning() && SupportsFocus(session_, candidate)) {
      if (OH_CaptureSession_SetFocusMode(session_, candidate) != CAMERA_OK) return false;
      config_.focusMode = mode;
      return true;
    }
  }
  return false;
}

bool CameraStreamer::FocusAt(double x, double y) {
  if (!IsRunning() || !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > 1 || y > 1 ||
      !SupportsFocus(session_, FOCUS_MODE_AUTO)) return false;
  Camera_FocusMode active{};
  return OH_CaptureSession_GetFocusMode(session_, &active) == CAMERA_OK && active == FOCUS_MODE_AUTO &&
      OH_CaptureSession_SetFocusPoint(session_, Camera_Point{x, y}) == CAMERA_OK;
}

}  // namespace ipcam
