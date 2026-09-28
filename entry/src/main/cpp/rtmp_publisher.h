#ifndef IPCAMERA_RTMP_PUBLISHER_H
#define IPCAMERA_RTMP_PUBLISHER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ipcam {

// Minimal RTMP client + FLV muxing (the HarmonyOS replacement of the original
// libRTMPPublisher.so / FlvPublisher). Supports the subset needed to publish
// H.264/H.265 + AAC to standard servers (nginx-rtmp, SRS, ...).
class RtmpPublisher {
 public:
  using ErrorSink = std::function<void(const std::string&)>;

  RtmpPublisher() = default;
  ~RtmpPublisher();
  RtmpPublisher(const RtmpPublisher&) = delete;
  RtmpPublisher& operator=(const RtmpPublisher&) = delete;

  // Synchronous for use by a NAPI async worker. DNS, TCP connect, handshake and
  // publish share one absolute 10-second deadline and can be interrupted by
  // Cancel()/Stop().
  bool Start(const std::string& url, int width, int height, bool h265Hint,
             ErrorSink onError);
  // Requests cancellation without waiting for a synchronous Start worker. The
  // async completion path or Stop() performs the final join and cleanup.
  void Cancel();
  void Stop();
  bool IsRunning() const { return running_.load(); }
  std::string LastError() const;

  // Inject parameter sets cached by the shared encoder before starting RTMP in
  // the middle of a stream. NAL units must not include Annex-B start codes.
  void SetVideoParams(bool h265, const std::vector<uint8_t>& vps,
                      const std::vector<uint8_t>& sps,
                      const std::vector<uint8_t>& pps);

  // Annex-B H.264/H.265 frame and one ADTS AAC frame respectively. A timestamp
  // of -1 means missing and is replaced with the internal monotonic clock;
  // zero is a valid timestamp.
  void SendH264Packet(const uint8_t* data, size_t size, int64_t tsUs);
  void SendAdtsPacket(const uint8_t* data, size_t size, int64_t tsUs);

 private:
  enum class LifecycleState { kIdle, kStarting, kRunning, kStopping };

  struct OutMsg {
    uint8_t type = 0;
    uint8_t csid = 0;
    uint32_t streamId = 0;
    uint32_t tsMs = 0;
    std::vector<uint8_t> payload;
  };

  bool ConnectAndPublish(const std::string& host, int port,
                         const std::string& app, const std::string& stream,
                         int fd, int64_t deadlineUs, std::string* error);
  void SenderLoop(int fd);
  bool EnqueueVideo(OutMsg frame, OutMsg sequenceHeader, bool keyFrame,
                    uint64_t configVersion);
  bool EnqueueAudio(OutMsg frame, OutMsg sequenceHeader, uint32_t configKey);
  bool EnqueueControl(OutMsg msg);
  bool WaitingForKeyframe();
  bool TimestampToMs(int64_t tsUs, bool video, uint32_t* tsMs);
  void FailOnce(const std::string& detail, bool startup);
  void AbortRuntime(const std::string& detail);
  void InterruptSocket();
  void CloseSocketIf(int expectedFd);
  void ResetQueue();
  void ResetQueueLocked();
  void ResetSessionState();
  static size_t QueuedBytes(const OutMsg& msg);
  static std::vector<uint8_t> BuildWireMessage(const OutMsg& msg);

  std::atomic<bool> running_{false};
  std::atomic<bool> cancelRequested_{false};
  std::atomic<bool> errorReported_{false};

  std::mutex lifecycleMu_;
  std::mutex stopMu_;
  std::condition_variable lifecycleCv_;
  LifecycleState lifecycleState_ = LifecycleState::kIdle;
  bool startWorkerActive_ = false;
  bool startAttempted_ = false;
  std::thread senderThread_;

  std::mutex sockMu_;
  int sock_ = -1;

  ErrorSink onError_;
  std::string lastError_;
  mutable std::mutex errMu_;

  std::mutex queueMu_;
  std::condition_variable queueCv_;
  std::deque<OutMsg> queue_;
  size_t queuedBytes_ = 0;
  size_t inFlightBytes_ = 0;
  bool waitingForKeyframe_ = true;
  bool queuedVideoConfigSet_ = false;
  uint64_t queuedVideoConfigVersion_ = 0;
  bool queuedAudioConfigSet_ = false;
  uint32_t queuedAudioConfig_ = 0;

  std::mutex paramMu_;
  int width_ = 0;
  int height_ = 0;
  int audioRate_ = 48000;
  int audioChannels_ = 1;
  bool videoIsH265_ = false;  // enhanced-RTMP (fourCC hvc1) when true
  std::vector<uint8_t> vps_;  // active H.265 VPS
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
  uint64_t videoConfigVersion_ = 1;

  bool presetVideoParamsValid_ = false;
  bool presetVideoIsH265_ = false;
  std::vector<uint8_t> presetVps_;
  std::vector<uint8_t> presetSps_;
  std::vector<uint8_t> presetPps_;

  int64_t tsBaseUs_ = 0;
  bool tsBaseSet_ = false;
  int64_t lastVideoTsUs_ = 0;
  int64_t lastAudioTsUs_ = 0;
  bool lastVideoTsSet_ = false;
  bool lastAudioTsSet_ = false;
};

}  // namespace ipcam

#endif  // IPCAMERA_RTMP_PUBLISHER_H
