#ifndef IPCAMERA_DUAL_CAMERA_LAYOUT_H
#define IPCAMERA_DUAL_CAMERA_LAYOUT_H
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ipcam {
// One gate per attempt. No SDK calls or callbacks under this lock. The secondary
// camera publishes owned pixels, never a pointer into a released queue buffer.
class DualFrameGate {
 public:
  bool Configure(int layout, int width, int height, int secondaryWidth, int secondaryHeight, int secondaryRotation);
  void Publish(const uint8_t* pixels, size_t size, int w, int h);
  bool Composite(uint8_t* primary, size_t size, int w, int h);
  bool Wait(std::chrono::milliseconds timeout);
  void Close();
 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<uint8_t> second_, scratch_;
  int layout_ = 0, w_ = 0, h_ = 0, sw_ = 0, sh_ = 0, rotation_ = 0;
  bool configured_ = false, closed_ = false, received_ = false, composed_ = false;
  std::chrono::steady_clock::time_point last_{};
};
} // namespace ipcam
#endif
