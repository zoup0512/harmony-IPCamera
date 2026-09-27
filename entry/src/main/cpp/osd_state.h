#ifndef IPCAMERA_OSD_STATE_H
#define IPCAMERA_OSD_STATE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ipcam {

// Process-global OSD ("text overlay") configuration, fed from ArkTS settings
// (keys mirror the Android preferences.xml text-overlay sub-page). The GL
// camera pipeline snapshots it per frame and the renderer re-rasterizes the
// text layer only when something actually changed.
struct OsdSnapshot {
  bool enabled = false;
  int padding = 1;             // osd_padding_int (0..100), scaled to frame height
  int position = 0;            // text_position: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
  int fontStyle = 0;           // font_style: 0 normal, 1 bold, 2 italic, 3 bold-italic
  uint32_t argb = 0xFFFFFFFF;  // text_color, 0xAARRGGBB
  bool showTimestamp = true;   // display_timestamp (yyyy-MM-dd HH:mm:ss)
  bool showDevName = true;     // display_dev_name
  bool showBattery = true;     // display_battery_info (BAT:%d% + charge suffix)
  bool showGps = false;        // display_gps_location
  bool speedMph = false;       // speed_unit: 0 km/h, 1 mph
  std::string customText;      // text_custom
  std::string devName;         // deviceInfo.productModel, pushed from ArkTS
  int batteryLevel = -1;       // percent; -1 = unknown (line hidden)
  int batteryState = 0;        // batteryInfo.chargingState (2 = wireless)
  bool gpsValid = false;       // false renders "GPS Waiting"
  double gpsLat = 0;
  double gpsLng = 0;
  double gpsSpeed = 0;         // km/h (ArkTS converts m/s)
  bool wmEnabled = false;      // watermark_overlay
  int wmPadding = 0;           // watermark_padding_int
  int wmMaxAreaPercent = 10;   // watermark_max_area_occupied_int (1..100)
  int wmPosition = 0;          // watermark_position
  int wmWidth = 0;             // decoded watermark RGBA_8888, ArkTS-provided
  int wmHeight = 0;
  uint32_t wmVersion = 0;      // bumps on every SetOsdWatermarkPixels
  std::shared_ptr<const std::vector<uint8_t>> wmPixels;  // straight alpha
};

// Single-server application: one global state object keeps CameraStreamer /
// GL pipeline access simple.
OsdSnapshot GetOsd();
bool OsdEnabled();

void SetOsdEnabled(bool enabled);
void SetOsdTextConfig(bool enabled, int padding, int position, int fontStyle,
                      uint32_t argb, bool showTimestamp, bool showDevName,
                      bool showBattery, bool showGps, bool speedMph,
                      const std::string& customText);
void SetOsdDeviceName(const std::string& name);
void SetOsdBattery(int level, int chargeState);
void SetOsdGps(bool valid, double lat, double lng, double speed);
void SetOsdWatermarkConfig(bool enabled, int padding, int maxAreaPercent,
                           int position);
// Copies the RGBA_8888 buffer; bumps wmVersion.
void SetOsdWatermarkPixels(const uint8_t* rgba, int width, int height);

}  // namespace ipcam

#endif  // IPCAMERA_OSD_STATE_H
