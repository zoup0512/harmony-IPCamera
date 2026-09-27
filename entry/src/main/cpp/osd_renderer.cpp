#include "osd_renderer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <hilog/log.h>
#include <native_drawing/drawing_bitmap.h>
#include <native_drawing/drawing_canvas.h>
#include <native_drawing/drawing_font_collection.h>
#include <native_drawing/drawing_text_typography.h>
#include <native_drawing/drawing_types.h>

#define LOG_DOMAIN 0xC010
#define LOG_TAG "OsdRenderer"

namespace {

// Battery line mirrors the Android osd_battery / osd_battery_charge_* strings.
std::string ChargeSuffix(int state) {
  if (state == 1) return "\xE2\x9A\xA1" "AC";            // ⚡AC (wired charging)
  if (state == 2) return "\xE2\x9A\xA1" "Wireless";      // ⚡Wireless
  return "";
}

// Mirror of Android osd_location_kmh / osd_location_mph.
std::string GpsLine(const ipcam::OsdSnapshot& snap) {
  if (!snap.gpsValid) return "GPS Waiting";
  char buf[128];
  if (snap.speedMph) {
    snprintf(buf, sizeof(buf), "Lat:%.6f Lng:%.6f %.1fmph",
             snap.gpsLat, snap.gpsLng, snap.gpsSpeed * 0.621371);
  } else {
    snprintf(buf, sizeof(buf), "Lat:%.6f Lng:%.6f %.1fkm/h",
             snap.gpsLat, snap.gpsLng, snap.gpsSpeed);
  }
  return buf;
}

}  // namespace

namespace ipcam {

bool OsdRenderer::RefreshIfNeeded(const OsdSnapshot& snap, int frameW, int frameH) {
  time_t now = time(nullptr);
  struct tm lt{};
  localtime_r(&now, &lt);
  long second = lt.tm_sec + 60L * (lt.tm_min + 60L * lt.tm_hour);

  // The timestamp only changes once per second; re-raster on the second tick
  // or whenever any text-affecting field differs from the cached snapshot.
  bool dirty = !hasCached_ || second != cachedSecond_ || text_.width != frameW ||
               snap.enabled != cached_.enabled || snap.padding != cached_.padding ||
               snap.position != cached_.position || snap.fontStyle != cached_.fontStyle ||
               snap.argb != cached_.argb || snap.showTimestamp != cached_.showTimestamp ||
               snap.showDevName != cached_.showDevName ||
               snap.showBattery != cached_.showBattery || snap.showGps != cached_.showGps ||
               snap.speedMph != cached_.speedMph ||
               snap.customText != cached_.customText || snap.devName != cached_.devName ||
               snap.batteryLevel != cached_.batteryLevel ||
               snap.batteryState != cached_.batteryState ||
               snap.gpsValid != cached_.gpsValid || snap.gpsLat != cached_.gpsLat ||
               snap.gpsLng != cached_.gpsLng || snap.gpsSpeed != cached_.gpsSpeed;
  if (!dirty) return false;

  cached_ = snap;
  cachedSecond_ = second;
  hasCached_ = true;

  std::string lines;
  auto addLine = [&lines](const std::string& s) {
    if (s.empty()) return;
    if (!lines.empty()) lines += "\n";
    lines += s;
  };
  if (snap.showTimestamp) {
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);
    addLine(ts);
  }
  if (snap.showDevName && !snap.devName.empty()) addLine(snap.devName);
  if (snap.showBattery && snap.batteryLevel >= 0) {
    char bat[48];
    snprintf(bat, sizeof(bat), "BAT:%d%%%s", snap.batteryLevel,
             ChargeSuffix(snap.batteryState).c_str());
    addLine(bat);
  }
  if (snap.showGps) addLine(GpsLine(snap));
  if (!snap.customText.empty()) addLine(snap.customText);

  if (lines.empty()) {
    text_ = Layer{};
    return true;
  }
  Rasterize(lines, snap, frameW, frameH);
  return true;
}

void OsdRenderer::Rasterize(const std::string& text, const OsdSnapshot& snap, int frameW,
                            int frameH) {
  int fontPx = frameH / 24;
  if (fontPx < 16) fontPx = 16;
  if (fontPx > 96) fontPx = 96;
  int maxW = frameW * 2 / 3;
  if (maxW < 64) maxW = 64;

  // The global instance is the only FontCollection whose internals are valid
  // outside the ArkTS UI runtime; a self-created one SEGVs inside
  // TypographyCreate (observed on HarmonyOS 7.0: its inner shared_ptr holds
  // garbage when the collection is created on a pure native thread).
  static OH_Drawing_FontCollection* collection = OH_Drawing_GetFontCollectionGlobalInstance();
  if (collection == nullptr) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "no global font collection");
    text_ = Layer{};
    return;
  }

  OH_Drawing_TypographyStyle* tstyle = OH_Drawing_CreateTypographyStyle();
  OH_Drawing_TextStyle* style = tstyle != nullptr ? OH_Drawing_CreateTextStyle() : nullptr;
  if (style == nullptr) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "text style create failed");
    if (tstyle != nullptr) OH_Drawing_DestroyTypographyStyle(tstyle);
    text_ = Layer{};
    return;
  }
  OH_Drawing_SetTextStyleColor(style, snap.argb);
  OH_Drawing_SetTextStyleFontSize(style, static_cast<double>(fontPx));
  OH_Drawing_SetTextStyleFontWeight(style, snap.fontStyle == 1 || snap.fontStyle == 3 ? 700 : 400);
  OH_Drawing_SetTextStyleFontStyle(style, snap.fontStyle >= 2 ? 1 : 0);

  OH_Drawing_TypographyCreate* handler = OH_Drawing_CreateTypographyHandler(tstyle, collection);
  if (handler == nullptr) {
    OH_Drawing_DestroyTextStyle(style);
    OH_Drawing_DestroyTypographyStyle(tstyle);
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "typography handler failed");
    text_ = Layer{};
    return;
  }
  OH_Drawing_TypographyHandlerPushTextStyle(handler, style);
  OH_Drawing_TypographyHandlerAddText(handler, text.c_str());
  OH_Drawing_Typography* typo = OH_Drawing_CreateTypography(handler);
  OH_Drawing_DestroyTypographyHandler(handler);
  OH_Drawing_DestroyTextStyle(style);
  OH_Drawing_DestroyTypographyStyle(tstyle);
  if (typo == nullptr) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "CreateTypography failed");
    text_ = Layer{};
    return;
  }
  OH_Drawing_TypographyLayout(typo, static_cast<double>(maxW));
  int w = static_cast<int>(std::ceil(OH_Drawing_TypographyGetLongestLine(typo)));
  int h = static_cast<int>(std::ceil(OH_Drawing_TypographyGetHeight(typo)));
  if (w <= 0 || h <= 0) {
    OH_Drawing_DestroyTypography(typo);
    text_ = Layer{};
    return;
  }

  OH_Drawing_Bitmap* bmp = OH_Drawing_BitmapCreate();
  OH_Drawing_BitmapFormat fmt{COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_PREMUL};
  OH_Drawing_BitmapBuild(bmp, static_cast<uint32_t>(w), static_cast<uint32_t>(h), &fmt);
  OH_Drawing_Canvas* canvas = OH_Drawing_CanvasCreate();
  OH_Drawing_CanvasBind(canvas, bmp);
  OH_Drawing_CanvasClear(canvas, 0x00000000);
  OH_Drawing_TypographyPaint(typo, canvas, 0, 0);

  Layer layer;
  layer.width = w;
  layer.height = h;
  size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  layer.rgba.resize(bytes);
  void* px = OH_Drawing_BitmapGetPixels(bmp);
  if (px != nullptr) {
    memcpy(layer.rgba.data(), px, bytes);
  } else {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "BitmapGetPixels null");
    layer.width = layer.height = 0;
  }
  text_ = std::move(layer);

  OH_Drawing_CanvasDestroy(canvas);
  OH_Drawing_BitmapDestroy(bmp);
  OH_Drawing_DestroyTypography(typo);
}

OsdRenderer::Rect OsdRenderer::TextRect(const OsdSnapshot& snap, int frameW,
                                        int frameH) const {
  Rect r;
  if (text_.empty()) return r;
  int pad = snap.padding * frameH / 720;
  r.w = text_.width;
  r.h = text_.height;
  r.x = (snap.position == 1 || snap.position == 3) ? frameW - pad - r.w : pad;
  r.y = (snap.position >= 2) ? frameH - pad - r.h : pad;
  if (r.x < 0) r.x = 0;
  if (r.y < 0) r.y = 0;
  return r;
}

OsdRenderer::Rect OsdRenderer::WatermarkRect(const OsdSnapshot& snap, int frameW,
                                             int frameH) const {
  Rect r;
  if (!snap.wmEnabled || snap.wmWidth <= 0 || snap.wmHeight <= 0 ||
      snap.wmPixels == nullptr) {
    return r;
  }
  double scale = std::sqrt((static_cast<double>(frameW) * frameH *
                            snap.wmMaxAreaPercent / 100.0) /
                           (static_cast<double>(snap.wmWidth) * snap.wmHeight));
  double tw = snap.wmWidth * scale;
  double th = snap.wmHeight * scale;
  int pad = snap.wmPadding * frameH / 720;
  double maxW = frameW - 2.0 * pad;
  double maxH = frameH - 2.0 * pad;
  if (maxW <= 0 || maxH <= 0) return r;
  if (tw > maxW || th > maxH) {
    double shrink = std::min(maxW / tw, maxH / th);
    tw *= shrink;
    th *= shrink;
  }
  r.w = static_cast<int>(tw);
  r.h = static_cast<int>(th);
  if (r.w <= 0 || r.h <= 0) return r;
  r.x = (snap.wmPosition == 1 || snap.wmPosition == 3) ? frameW - pad - r.w : pad;
  r.y = (snap.wmPosition >= 2) ? frameH - pad - r.h : pad;
  if (r.x < 0) r.x = 0;
  if (r.y < 0) r.y = 0;
  return r;
}

}  // namespace ipcam
