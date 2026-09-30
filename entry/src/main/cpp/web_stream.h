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
  // Fires on both edges: true when motion starts, false once motion has been
  // absent for the hold time — the caller starts and stops its auto recording
  // off these two transitions (Android reports the same two states).
  using MotionSink = std::function<void(bool motion)>;

  ~WebStream();

  void FeedVideo(const uint8_t* data, size_t size);  // Annex-B, both codecs

  void SetParams(bool h265, const std::vector<uint8_t>& paramsAnnexB, int width, int height);
  // Enabling keeps the decode loop alive on its own (no web client needed) and
  // arms the detector kArmDelayUs later, so the user can leave the frame first.
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
  // The latch is time based, so it is re-evaluated on every loop pass even when
  // no new picture was decoded — the auto recording then stops within half a
  // second of the timeout instead of at the next keyframe.
  void UpdateMotionLatch();
  bool MotionArmed(uint64_t now) const;
  // The decode loop serves two independent consumers — JPEG frames for the web
  // console and NV12 frames for motion detection — so it runs while either one
  // wants it.
  void UpdateWorker();
  bool WorkerWanted() const;

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

  std::atomic<int> jpegClients_{0};    // web console consumers of LatestJpeg()
  std::atomic<bool> motionDemand_{false};
  std::atomic<uint64_t> idrGeneration_{0};
  uint64_t decodedIdrGeneration_ = 0;  // decode loop only
  std::atomic<bool> workerRunning_{false};
  std::mutex workerMu_;                // serialises UpdateWorker start/join
  std::thread workerThread_;

  std::mutex motionMu_;
  std::vector<uint8_t> prevGrid_;
  std::atomic<int64_t> lastMotionUs_{0};
  std::atomic<int64_t> armAtUs_{0};    // 0 = not armed (detector idle)
  int64_t motionHoldUs_ = 15000000;    // default 15 s, Android motion_timeout default
  MotionSink motionSink_;
  std::mutex sinkMu_;
};

}  // namespace ipcam

#endif  // IPCAMERA_WEB_STREAM_H
