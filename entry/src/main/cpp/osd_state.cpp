#include "osd_state.h"

#include <mutex>

namespace ipcam {
namespace {

std::mutex g_mu;
OsdSnapshot g_osd;
uint32_t g_wmVersion = 0;

}  // namespace

OsdSnapshot GetOsd() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_osd;
}

bool OsdEnabled() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_osd.enabled;
}

void SetOsdEnabled(bool enabled) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_osd.enabled = enabled;
}

void SetOsdTextConfig(bool enabled, int padding, int position, int fontStyle,
                      uint32_t argb, bool showTimestamp, bool showDevName,
                      bool showBattery, bool showGps, bool speedMph,
                      const std::string& customText) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_osd.enabled = enabled;
  if (padding < 0) padding = 0;
  if (padding > 100) padding = 100;
  g_osd.padding = padding;
  g_osd.position = position & 0x3;
  g_osd.fontStyle = fontStyle;
  if (fontStyle < 0 || fontStyle > 3) g_osd.fontStyle = 0;
  g_osd.argb = argb;
  g_osd.showTimestamp = showTimestamp;
  g_osd.showDevName = showDevName;
  g_osd.showBattery = showBattery;
  g_osd.showGps = showGps;
  g_osd.speedMph = speedMph;
  g_osd.customText = customText;
}

void SetOsdDeviceName(const std::string& name) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_osd.devName = name;
}

void SetOsdBattery(int level, int chargeState) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_osd.batteryLevel = level;
  g_osd.batteryState = chargeState;
}

void SetOsdGps(bool valid, double lat, double lng, double speed) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_osd.gpsValid = valid;
  g_osd.gpsLat = lat;
  g_osd.gpsLng = lng;
  g_osd.gpsSpeed = speed;
}

void SetOsdWatermarkConfig(bool enabled, int padding, int maxAreaPercent,
                           int position) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_osd.wmEnabled = enabled;
  if (padding < 0) padding = 0;
  if (padding > 100) padding = 100;
  g_osd.wmPadding = padding;
  if (maxAreaPercent < 1) maxAreaPercent = 1;
  if (maxAreaPercent > 100) maxAreaPercent = 100;
  g_osd.wmMaxAreaPercent = maxAreaPercent;
  g_osd.wmPosition = position & 0x3;
}

void SetOsdWatermarkPixels(const uint8_t* rgba, int width, int height) {
  if (rgba == nullptr || width <= 0 || height <= 0) return;
  size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  auto pixels = std::make_shared<const std::vector<uint8_t>>(rgba, rgba + bytes);
  std::lock_guard<std::mutex> lk(g_mu);
  g_osd.wmPixels = std::move(pixels);
  g_osd.wmWidth = width;
  g_osd.wmHeight = height;
  g_osd.wmVersion = ++g_wmVersion;
}

}  // namespace ipcam
