#ifndef IPCAMERA_CAPTURE_ORIENTATION_H
#define IPCAMERA_CAPTURE_ORIENTATION_H

#include <algorithm>
#include <cstdint>

namespace ipcam {

// Wire values are clockwise degrees from the unrotated camera frame, NOT
// display-transform ordinals. Only Auto consumes the preview's rotation.
enum class CaptureRotation : int { Auto = -1, Deg0 = 0, Deg90 = 90, Deg180 = 180, Deg270 = 270 };
struct CaptureOrientation {
  CaptureRotation rotation = CaptureRotation::Auto;
  bool mirror = false;  // Horizontal in final visual coordinates, before OSD.

  bool Valid() const {
    return rotation == CaptureRotation::Auto || rotation == CaptureRotation::Deg0 ||
           rotation == CaptureRotation::Deg90 || rotation == CaptureRotation::Deg180 ||
           rotation == CaptureRotation::Deg270;
  }
  bool RequiresPixels() const { return mirror || rotation != CaptureRotation::Auto; }
  int Resolve(int previewDegrees) const {
    return rotation == CaptureRotation::Auto ? previewDegrees : static_cast<int>(rotation);
  }
};

// Caller has already checked a tightly packed NV12 buffer of even w x h.
inline void MirrorPackedNv12(uint8_t* pixels, int w, int h) {
  for (int y = 0; y < h; ++y, pixels += w) std::reverse(pixels, pixels + w);
  for (int y = 0; y < h / 2; ++y, pixels += w) {
    for (int l = 0, r = w - 2; l < r; l += 2, r -= 2) {
      std::swap(pixels[l], pixels[r]);
      std::swap(pixels[l + 1], pixels[r + 1]);  // Keep U and V together.
    }
  }
}

}  // namespace ipcam
#endif  // IPCAMERA_CAPTURE_ORIENTATION_H
