#ifndef IPCAMERA_WEB_STREAM_H
#define IPCAMERA_WEB_STREAM_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ipcam {

// Consumes the shared encoded stream (Annex-B H.264/H.265), caches codec
// parameters and the latest keyframe, and periodically decodes to NV12 →
// JPEG for the web console (MJPEG + snapshot) and motion detection.
// Uses the proven "cold decode" pattern (create → push → EOS → output →
// destroy) at ~1–2 fps — verified to work on this device.
class WebStream {
 public:
  using MotionSink = std::function<void(bool motion)>;

  ~WebStream();

  void FeedVideo(const uint8_t* data, size_t size);  // Annex-B, both codecs

  void SetParams(bool h265, const std::vector<uint8_t>& paramsAnnexB, int width, int height);
  void SetMotionEnabled(bool enabled);
  void SetMotionTimeout(int seconds);
  bool MotionActive() const { return motionLatch_.load(); }
  void SetMotionSink(MotionSink sink);
  std::vector<uint8_t> LatestJpeg() const;

  void IncClients();
  void DecClients();

 private:
  void DecodeLoop();
  bool DecodeOne(const std::vector<uint8_t>& params, const std::vector<uint8_t>& idr,
                 int w, int h, std::vector<uint8_t>* nv12Out);
  void CheckMotion(const uint8_t* nv12, int w, int h);

  std::atomic<bool> h265_{false};
  std::atomic<int> width_{1280};
  std::atomic<int> height_{720};
  std::atomic<bool> motionEnabled_{false};
  std::atomic<bool> motionLatch_{false};
  std::atomic<bool> paramsSet_{false};

  mutable std::mutex stateMu_;
  std::vector<uint8_t> params_;   // Annex-B VPS/SPS/PPS or SPS/PPS
  std::vector<uint8_t> lastIdr_;  // Annex-B IDR frame
  std::vector<uint8_t> lastJpeg_;

  std::atomic<bool> clientsActive_{false};
  std::atomic<bool> workerRunning_{false};
  std::thread workerThread_;

  std::mutex motionMu_;
  std::vector<uint8_t> prevGrid_;
  int64_t lastMotionUs_ = 0;
  int64_t motionHoldUs_ = 5000000;  // default 5 s
  MotionSink motionSink_;
  std::mutex sinkMu_;
};

}  // namespace ipcam

#endif  // IPCAMERA_WEB_STREAM_H
