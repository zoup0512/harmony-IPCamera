// Native camera and drawing APIs are fixtures; renderer, routing and NV12 writes
// are the production translation units. No emulator, fonts or camera required.
#include "host_sdk.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include "osd_renderer.h"
#define private public
#include "osd_pipeline.h"
#undef private
#ifdef _WIN32
inline tm* localtime_r(const time_t* t, tm* out) { return localtime_s(out, t) == 0 ? out : nullptr; }
#endif

struct DrawingStub {} stub;
using OH_Drawing_Bitmap = DrawingStub;
using OH_Drawing_Canvas = DrawingStub;
using OH_Drawing_FontCollection = DrawingStub;
using OH_Drawing_Typography = DrawingStub;
using OH_Drawing_TypographyStyle = DrawingStub;
using OH_Drawing_TextStyle = DrawingStub;
using OH_Drawing_TypographyCreate = DrawingStub;
struct OH_Drawing_BitmapFormat { int color, alpha; };
enum { COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_PREMUL, FONT_WEIGHT_400, FONT_WEIGHT_700,
       FONT_STYLE_NORMAL, FONT_STYLE_ITALIC };
int rasters = 0, fontPx = -1, weight = -1, style = -1, registrations = 0, unregistrations = 0;
std::string lines, family;
auto* OH_Drawing_GetFontCollectionGlobalInstance() { return &stub; }
auto* OH_Drawing_CreateTypographyStyle() { return &stub; }
auto* OH_Drawing_CreateTextStyle() { family.clear(); return &stub; }
auto* OH_Drawing_CreateTypographyHandler(DrawingStub*, DrawingStub*) { return &stub; }
auto* OH_Drawing_CreateTypography(DrawingStub*) { return &stub; }
auto* OH_Drawing_BitmapCreate() { return &stub; }
auto* OH_Drawing_CanvasCreate() { return &stub; }
void OH_Drawing_SetTextStyleColor(DrawingStub*, uint32_t) {}
void OH_Drawing_SetTextStyleFontWeight(DrawingStub*, int v) { weight = v; }
void OH_Drawing_SetTextStyleFontStyle(DrawingStub*, int v) { style = v; }
void OH_Drawing_SetTextStyleFontSize(DrawingStub*, double v) { fontPx = static_cast<int>(v); }
void OH_Drawing_SetTextStyleFontFamilies(DrawingStub*, int n, const char** v) { if (n) family = v[0]; }
int OH_Drawing_RegisterFont(DrawingStub*, const char*, const char* path) { ++registrations; return strcmp(path, "good"); }
int OH_Drawing_UnregisterFont(DrawingStub*, const char*) { ++unregistrations; return 0; }
void OH_Drawing_TypographyHandlerPushTextStyle(DrawingStub*, DrawingStub*) {}
void OH_Drawing_TypographyHandlerAddText(DrawingStub*, const char* s) { lines = s; }
void OH_Drawing_DestroyTypographyHandler(DrawingStub*) {}
void OH_Drawing_DestroyTextStyle(DrawingStub*) {}
void OH_Drawing_DestroyTypographyStyle(DrawingStub*) {}
void OH_Drawing_TypographyLayout(DrawingStub*, double) {}
double OH_Drawing_TypographyGetHeight(DrawingStub*) { return 1; }
double OH_Drawing_TypographyGetLongestLine(DrawingStub*) { return 1; }
void OH_Drawing_BitmapBuild(DrawingStub*, uint32_t, uint32_t, OH_Drawing_BitmapFormat*) {}
void OH_Drawing_CanvasBind(DrawingStub*, DrawingStub*) {}
void OH_Drawing_CanvasClear(DrawingStub*, uint32_t) {}
void OH_Drawing_TypographyPaint(DrawingStub*, DrawingStub*, double, double) { ++rasters; }
void* OH_Drawing_BitmapGetPixels(DrawingStub*) { static uint8_t white[] = {255,255,255,255}; return white; }
void OH_Drawing_CanvasDestroy(DrawingStub*) {}
void OH_Drawing_BitmapDestroy(DrawingStub*) {}
void OH_Drawing_DestroyTypography(DrawingStub*) {}
#include "osd_state.cpp"
#include "osd_renderer.cpp"
#include "osd_pipeline.cpp"

using namespace ipcam;
void Text(bool on, bool mph = false) {
  SetOsdTextConfig(on, 0, 0, 3, 0xFFFFFFFF, false, true, true, true, mph, "custom");
}

int main() {
  OsdPipeline p;
  p.image_ = OH_NativeImage_Create(0, 0);
  p.inW_ = p.outW_ = p.inH_ = p.outH_ = 4;
  p.nv12_.resize(24);
  std::vector<uint8_t> encoded;
  p.sink_ = [&](const uint8_t* data, size_t n) { encoded.assign(data, data + n); };
  auto frame = [&](bool expected = true) {
    encoded.clear();
    auto& cam = p.image_->camera.buffer.pixels;
    cam.fill(128);
    std::fill(cam.begin(), cam.begin() + 16, 16);
    p.image_->pending = true;
    p.CompositeOne();
    Check(encoded.size() == (expected ? 24 : 0), "unexpected encoder delivery");
  };
  Check(!OsdEnabled(), "fresh camera should use direct route");
  SetOsdWatermarkConfig(true, 0, 25, 3);  // rectangle (2,2)..(3,3)
  Check(OsdEnabled() && !GetOsd().enabled, "watermark must request buffer mode before decode");
  frame();
  Check(encoded[15] == 16 && rasters == 0, "missing image drew an overlay");
  uint8_t rgba[] = {255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  SetOsdWatermarkPixels(rgba, 2, 2);
  frame();
  Check(encoded[10] == 82 && encoded[15] == 16 && encoded[0] == 16 && rasters == 0,
        "image-only path failed to blend at zero padding or drew text");
  Check(encoded[22] == 119 && encoded[23] == 156, "only one quarter of chroma block is opaque");
  rgba[0] = rgba[3] = 128;
  SetOsdWatermarkPixels(rgba, 2, 2, true); frame();
  Check(encoded[10] == 49, "premultiplied red must not be multiplied by alpha twice");
  rgba[3] = 0;
  SetOsdWatermarkPixels(rgba, 2, 2);
  frame();
  Check(encoded[10] == 16 && encoded[22] == 128, "same-size watermark replacement not picked up");
  SetOsdDeviceName("Rear camera"); SetOsdBattery(80, 2); SetOsdGps(true, 1, 2, 100);
  SetOsdFontConfig(36, "mono", "good"); Text(true, true);
  frame();
  Check(encoded[0] == 235 && lines.find("Rear camera") != std::string::npos &&
        lines.find("BAT:80% \xE2\x9A\xA1USB") != std::string::npos &&
        lines.find("62.1mph") != std::string::npos, "text fields not applied");
  Check(fontPx == 36 && weight == FONT_WEIGHT_700 && style == FONT_STYLE_ITALIC &&
        family == "IPCamera-Osd-Imported" && registrations == 1, "font selection not applied");
  frame(); Check(rasters == 1, "unchanged text redrawn");
  Text(false); frame();
  Check(OsdEnabled() && p.renderer_.text().empty() && encoded[0] == 16 && rasters == 1,
        "stale text painted in watermark-only mode");
  SetOsdFontConfig(500, "mono", "bad"); SetOsdBattery(80, 3); Text(true);
  frame();
  Check(fontPx == 128 && family == "mono" && registrations == 2 && unregistrations == 1 &&
        lines.find("Wireless") != std::string::npos && lines.find("100.0km/h") != std::string::npos,
        "fallback font or GPS/battery change not applied");
  SetOsdBattery(100, 0); SetOsdGps(false, 0, 0, 0); frame();
  Check(lines.find("BAT:100%\n") != std::string::npos && lines.find("GPS Waiting") != std::string::npos &&
        registrations == 2, "full/discharging or no-fix display incorrect");
  SetOsdWatermarkConfig(false, 0, 25, 3); Check(OsdEnabled(), "text-only must composite");
  Text(false); Check(!OsdEnabled(), "last switch off must restore direct path");

  // Treat an empty second-camera queue as a dropped entire frame. The marker
  // is applied before OSD: text and image overwrite it at their own corners.
  SetOsdWatermarkConfig(true, 0, 25, 3); rgba[0] = rgba[3] = 255;
  SetOsdWatermarkPixels(rgba, 2, 2); SetOsdDeviceName("New lens"); Text(true);
  p.uiActive_.store(true); bool haveSecond = false; int called = 0;
  p.SetPreOverlay([&](uint8_t* data, size_t n, int w, int h) {
    Check(n == 24 && w == 4 && h == 4 && data[0] == 16 && data[10] == 16,
          "callback must receive the camera frame before either overlay");
    ++called; std::fill(data, data + 16, 90); return haveSecond;
  });
  int before = rasters; frame(false);
  Check(called == 1 && rasters == before && !p.presentPending_, "dropped frame rendered or queued");
  haveSecond = true; frame();
  Check(called == 2 && rasters == before + 1 && encoded[0] == 235 && encoded[10] == 82 &&
        encoded[15] == 90 && p.presentPending_ && p.presentFrame_ == encoded,
        "UI/encoder did not receive final dual camera composite including overlays");

  // Rectangular input: verify the actual luma permutation, not just angles.
  p.SetPreOverlay({}); p.uiActive_.store(false); Text(false);
  p.inW_ = 4; p.inH_ = 2;
  auto& camera = p.image_->camera.buffer;
  camera.handle.height = 2; camera.pixels.fill(128);
  for (int i = 0; i < 8; ++i) camera.pixels[i] = static_cast<uint8_t>(i + 16);
  const uint8_t expected[][8] = {{16,17,18,19,20,21,22,23}, {20,16,21,17,22,18,23,19},
                                 {23,22,21,20,19,18,17,16}, {19,23,18,22,17,21,16,20}};
  const uint8_t red[] = {255,0,0,255}; SetOsdWatermarkPixels(red, 1, 1);
  SetOsdWatermarkConfig(true, 0, 25, 1); // one pixel at visual top right
  for (int i = 0; i < 4; ++i) {
    p.rotation_ = 90 * i; p.outW_ = i % 2 ? 2 : 4; p.outH_ = 8 / p.outW_;
    p.nv12_.resize(12); p.image_->pending = true; p.CompositeOne();
    Check(encoded.size() == 12, "incorrect rotated frame size");
    for (int j = 0; j < 8; ++j) Check(encoded[j] == (j == p.outW_ - 1 ? 82 : expected[i][j]),
                                       "wrong orientation or watermark is not at visual top right");
  }
}
