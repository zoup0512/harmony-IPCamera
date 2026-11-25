// Runs the production Start -> CompositeOne -> encoder and preview paths.
#include "host_sdk.h"
#include "osd_renderer.h"
#include <algorithm>
#include <functional>
#include <thread>
#define private public
#include "osd_pipeline.h"
#undef private
#include "osd_pipeline.cpp"

namespace ipcam {
OsdSnapshot GetOsd() { return {}; }
bool OsdRenderer::RefreshIfNeeded(const OsdSnapshot&, int, int) { return false; }
OsdRenderer::Rect OsdRenderer::TextRect(const OsdSnapshot&, int, int) const { return {}; }
OsdRenderer::Rect OsdRenderer::WatermarkRect(const OsdSnapshot&, int, int) const { return {}; }
}
using namespace ipcam;

// Input is 6x4, stride 8. Each luma and each chroma block has a distinct id.
// Expected (clockwise) output grids, not derived from the compositor's mapping.
constexpr uint8_t luma[4][24] = {
  {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23},
  {18,12,6,0,19,13,7,1,20,14,8,2,21,15,9,3,22,16,10,4,23,17,11,5},
  {23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
  {5,11,17,23,4,10,16,22,3,9,15,21,2,8,14,20,1,7,13,19,0,6,12,18}
};
constexpr uint8_t blocks[4][6] = {{0,1,2,3,4,5},{3,0,4,1,5,2},{5,4,3,2,1,0},{2,5,1,4,0,3}};

void Run(CaptureRotation mode, bool mirror, bool hasUi, bool nv21) {
  OsdPipeline p;
  uint64_t camera = 0;
  std::vector<uint8_t> encoded;
  initialTransform = 10;  // FLIP_H_ROT270. Auto uses the angle, not the selfie flip.
  const int rot = mode == CaptureRotation::Auto ? (hasUi ? 270 : 0) : static_cast<int>(mode);
  const int k = rot / 90, w = k % 2 ? 4 : 6, h = 24 / w;
  p.SetPreOverlay([&](uint8_t* frame, size_t n, int width, int height) {
    Check(n == 36 && width == w && height == h, "hook geometry");
    Check(std::equal(frame, frame + 24, luma[k]), "rotation must precede dual layout");
    frame[0] = 99; frame[24] = 88; frame[25] = 188;  // simulated secondary at visual left
    return true;
  });
  Check(p.Start(6, 4, hasUi ? 1 : 0, &camera,
                [&](const uint8_t* b, size_t n) { encoded.assign(b, b + n); }, {}, {mode, mirror}), "Start");
  Check(camera && p.rotation() == rot && p.OutWidth() == w && p.OutHeight() == h, "run geometry");
  auto* old = p.uiWindow_;
  if (old) Check(old->transform == 0, "double rotation on initial preview");
  initialTransform = 2;
  Check(p.SetUiSurface(2), "late/replacement preview");
  auto* ui = p.uiWindow_;
  Check((!old || old->transform == 10) && ui->transform == 0 && p.rotation() == rot,
        "new preview must not redefine encoder rotation");
  auto& cam = p.image_->camera.buffer;
  cam.handle = {6, 4, 8, 48, nv21 ? NATIVEBUFFER_PIXEL_FMT_YCRCB_420_SP : NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP};
  cam.pixels.fill(0xff);
  for (int i = 0; i < 24; ++i) cam.pixels[i / 6 * 8 + i % 6] = i;
  for (int i = 0; i < 6; ++i) {
    int pos = 32 + i / 3 * 8 + i % 3 * 2;
    cam.pixels[pos + nv21] = 40 + i; cam.pixels[pos + !nv21] = 140 + i;
  }
  flushGate.Arm();
  p.image_->pending = true;
  p.CompositeOne();
  Check(encoded.size() == 36, "encoder byte count");
  for (int i = 0; i < 24; ++i) {
    int src = mirror ? i / w * w + w - 1 - i % w : i;
    Check(encoded[i] == (src == 0 ? 99 : luma[k][src]), "wrong Y / layout mirrored before hook");
  }
  for (int i = 0; i < 6; ++i) {
    int cw = w / 2, src = mirror ? i / cw * cw + cw - 1 - i % cw : i;
    Check(encoded[24 + 2*i] == (src == 0 ? 88 : 40 + blocks[k][src]) &&
          encoded[25 + 2*i] == (src == 0 ? 188 : 140 + blocks[k][src]), "wrong UV pair or position");
  }
  flushGate.Wait("no preview frame");
  Check(std::equal(encoded.begin(), encoded.end(), ui->buffer.pixels.begin()), "flushed preview != encoder input");
  flushGate.Open();
  p.Stop();
  Check(ui->transform == 2, "did not restore replacement surface transform");
}
int main() {
  for (auto r : {CaptureRotation::Auto, CaptureRotation::Deg0, CaptureRotation::Deg90,
                 CaptureRotation::Deg180, CaptureRotation::Deg270})
    for (bool m : {false, true}) for (bool ui : {false, true}) for (bool vu : {false, true}) Run(r, m, ui, vu);
  OsdPipeline bad; uint64_t id = 0;
  Check(!bad.Start(6, 4, 0, &id, {}, {}, {static_cast<CaptureRotation>(1), false}) && !id,
        "raw transform ordinal accepted as user degrees");
}
