#include "dual_camera_layout.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace ipcam {
namespace {
bool Valid(int w, int h) {
  return w >= 4 && h >= 4 && !(w & 1) && !(h & 1) && w <= 4096 && h <= 4096 && w * h <= 4096 * 2160;
}
size_t Bytes(int w, int h) { return static_cast<size_t>(w) * h * 3 / 2; }
struct Rect { int x, y, w, h; };
int Even(int n) { return n & ~1; }
// Fit without cropping or stretching surveillance content. All boundaries are
// 2x2-aligned so neighbouring slots cannot share a chroma sample.
void Draw(const uint8_t* src, int sw, int sh, int rot, uint8_t* dst, int dw, int dh, Rect box) {
  int rw = rot % 180 ? sh : sw, rh = rot % 180 ? sw : sh;
  int w = box.w, h = box.h;
  if (rw * h > rh * w) h = std::max(2, Even(w * rh / rw));
  else w = std::max(2, Even(h * rw / rh));
  const int left = box.x + Even((box.w - w) / 2), top = box.y + Even((box.h - h) / 2);
  const uint8_t* suv = src + static_cast<size_t>(sw) * sh;
  uint8_t* duv = dst + static_cast<size_t>(dw) * dh;
  // Rotate the Y and UV grids separately; UV is always a two-byte U,V sample.
  for (int plane = 0; plane < 2; ++plane) {
    const int shift = plane, pixel = 1 << shift;
    const int sW = sw >> shift, sH = sh >> shift, rW = rw >> shift, rH = rh >> shift;
    const int W = w >> shift, H = h >> shift;
    const uint8_t* source = plane ? suv : src;
    uint8_t* target = (plane ? duv : dst) + static_cast<size_t>(top >> shift) * dw + left;
    // Fixed-integer stepping, without a divide in the per-pixel loop.
    const int xStep = rW / W, xRem = rW % W;
    for (int y = 0; y < H; ++y) {
      int ry = y * rH / H, rx = 0, rem = 0;
      uint8_t* row = target + static_cast<size_t>(y) * dw;
      for (int x = 0; x < W; ++x) {
        int sx = rx, sy = ry;
        if (rot == 90) { sx = ry; sy = sH - 1 - rx; }
        else if (rot == 180) { sx = sW - 1 - rx; sy = sH - 1 - ry; }
        else if (rot == 270) { sx = sW - 1 - ry; sy = rx; }
        const uint8_t* p = source + static_cast<size_t>(sy) * sw + sx * pixel;
        row[x * pixel] = *p;
        if (plane) row[x * pixel + 1] = p[1];
        rx += xStep;
        rem += xRem;
        if (rem >= W) { rem -= W; ++rx; }
      }
    }
  }
}
} // namespace

bool DualFrameGate::Configure(int layout, int w, int h, int sw, int sh, int rotation) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_ || configured_ || layout < 1 || layout > 3 || !Valid(w,h) || !Valid(sw,sh) ||
      rotation < 0 || rotation >= 360 || rotation % 90) return false;
  try {
    second_.resize(Bytes(sw, sh));
    if (layout != 1) scratch_.resize(Bytes(w,h));
  } catch (const std::bad_alloc&) { return false; }
  layout_ = layout; w_ = w; h_ = h; sw_ = sw; sh_ = sh; rotation_ = rotation;
  configured_ = true;
  return true;
}
void DualFrameGate::Publish(const uint8_t* pixels, size_t size, int w, int h) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!configured_ || closed_ || !pixels || w != sw_ || h != sh_ || size != second_.size()) return;
  std::memcpy(second_.data(), pixels, size);
  last_ = std::chrono::steady_clock::now();
  received_ = true;
}
bool DualFrameGate::Composite(uint8_t* primary, size_t size, int w, int h) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_ || !received_ || !primary || w != w_ || h != h_ || size != Bytes(w,h)) return false;
  // After the first composite, a stalled secondary must not freeze the primary
  // stream: keep emitting primary frames and leave the secondary slot empty.
  const bool stale = std::chrono::steady_clock::now() - last_ > std::chrono::seconds(1);
  if (stale && !composed_) return false;
  if (layout_ == 1) {
    int pw = std::max(2, Even(w / 3)), ph = std::max(2, Even(h / 3));
    if (!stale) Draw(second_.data(), sw_, sh_, rotation_, primary, w, h, {w - pw, 0, pw, ph});
  } else {
    const size_t y = static_cast<size_t>(w) * h;
    std::memset(scratch_.data(), 16, y);
    std::memset(scratch_.data() + y, 128, size - y);
    const int split = Even((layout_ == 2 ? w : h) / 2);
    Rect a = layout_ == 2 ? Rect{0,0,split,h} : Rect{0,0,w,split};
    Rect b = layout_ == 2 ? Rect{split,0,w-split,h} : Rect{0,split,w,h-split};
    Draw(primary, w, h, 0, scratch_.data(), w, h, a);
    if (!stale) Draw(second_.data(), sw_, sh_, rotation_, scratch_.data(), w, h, b);
    std::memcpy(primary, scratch_.data(), size);
  }
  if (!composed_) { composed_ = true; cv_.notify_all(); }
  return true;
}
bool DualFrameGate::Wait(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait_for(lk, timeout, [this]{return closed_ || composed_;});
  return composed_ && !closed_;
}
void DualFrameGate::Close() {
  std::lock_guard<std::mutex> lk(mu_);
  closed_ = true;
  cv_.notify_all();
}
} // namespace ipcam
