#ifndef IPCAMERA_RTMP_PUBLISHER_H
#define IPCAMERA_RTMP_PUBLISHER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ipcam {

// Minimal RTMP client + FLV muxing (the HarmonyOS replacement of the original
// libRTMPPublisher.so / FlvPublisher). Supports the subset needed to publish
// H.264 + AAC to standard servers (nginx-rtmp, SRS, ...):
//   handshake, set chunk size, connect/createStream/publish (AMF0),
//   @setDataFrame onMetaData, AVC sequence header + AVCC media tags,
//   AAC sequence header + raw frames, server-message skipping with acks.
// Frames are handed in Annex-B (video) / ADTS (audio) exactly like the RTSP
// path, so both servers can share the same encoder output.
class RtmpPublisher {
 public:
  using ErrorSink = std::function<void(const std::string&)>;

  RtmpPublisher() = default;
  ~RtmpPublisher();
  RtmpPublisher(const RtmpPublisher&) = delete;
  RtmpPublisher& operator=(const RtmpPublisher&) = delete;

  // Blocks for up to ~6s while connecting/handshaking. url:
  // rtmp://host[:port]/app/streamKey (streamKey may contain slashes/query).
  // h265Hint selects enhanced-RTMP (hvc1) framing for the video track.
  bool Start(const std::string& url, int width, int height, bool h265Hint,
             ErrorSink onError);
  void Stop();
  bool IsRunning() const { return running_.load(); }

  // Annex-B H.264/H.265 frame. tsUs==0 -> stamped with the internal clock.
  void SendH264Packet(const uint8_t* data, size_t size, uint64_t tsUs);
  // One ADTS frame. tsUs==0 -> stamped with the internal clock.
  void SendAdtsPacket(const uint8_t* data, size_t size, uint64_t tsUs);

 private:
  struct OutMsg {
    uint8_t type = 0;
    uint8_t csid = 0;
    uint32_t streamId = 0;
    uint32_t tsMs = 0;
    std::vector<uint8_t> payload;
  };

  bool ConnectAndPublish(const std::string& host, int port, const std::string& app,
                         const std::string& stream);
  void SenderLoop(int fd);
  void Enqueue(OutMsg msg);

  int sock_ = -1;
  std::atomic<bool> running_{false};
  std::thread senderThread_;
  ErrorSink onError_;
  std::mutex errMu_;

  std::mutex queueMu_;
  std::vector<OutMsg> queue_;

  std::mutex paramMu_;
  int width_ = 0;
  int height_ = 0;
  int audioRate_ = 48000;
  int audioChannels_ = 1;
  bool videoSeqSent_ = false;
  bool audioSeqSent_ = false;
  bool videoIsH265_ = false;      // enhanced-RTMP (fourCC hvc1) when true
  std::vector<uint8_t> vps_;      // H.265 only
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
  uint64_t tsBaseUs_ = 0;
  bool tsBaseSet_ = false;
};

}  // namespace ipcam

#endif  // IPCAMERA_RTMP_PUBLISHER_H
