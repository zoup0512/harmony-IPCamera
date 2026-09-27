#ifndef IPCAMERA_OSD_RENDERER_H
#define IPCAMERA_OSD_RENDERER_H

#include <cstdint>
#include <string>
#include <vector>

#include "osd_state.h"

namespace ipcam {

// Rasterizes the OSD text block (timestamp / device name / battery / GPS /
// custom text) into an RGBA bitmap with OH_Drawing, and computes overlay
// rectangles for the GL pipeline. All drawing runs on the render thread.
class OsdRenderer {
 public:
  struct Layer {
    std::vector<uint8_t> rgba;  // premultiplied alpha
    int width = 0;
    int height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
  };
  struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };

  // Re-rasterizes the text layer when the second changed, the config changed
  // or the frame size changed. Returns true when the layer was rebuilt.
  bool RefreshIfNeeded(const OsdSnapshot& snap, int frameW, int frameH);
  // Text layer placement for the current snapshot/layer.
  Rect TextRect(const OsdSnapshot& snap, int frameW, int frameH) const;
  // Watermark placement (scaled so its area <= wmMaxAreaPercent% of frame).
  Rect WatermarkRect(const OsdSnapshot& snap, int frameW, int frameH) const;
  const Layer& text() const { return text_; }

 private:
  void Rasterize(const std::string& text, const OsdSnapshot& snap, int frameW,
                 int frameH);

  Layer text_;
  OsdSnapshot cached_;
  long cachedSecond_ = -1;
  bool hasCached_ = false;
};

}  // namespace ipcam

#endif  // IPCAMERA_OSD_RENDERER_H
