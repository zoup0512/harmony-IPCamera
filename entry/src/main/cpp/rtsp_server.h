#ifndef IPCAMERA_RTSP_SERVER_H
#define IPCAMERA_RTSP_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ipcam {

struct RtspConfig {
  int port = 8554;
  std::string path = "live";
  std::string user;      // empty disables basic auth
  std::string password;
};

enum class RtspEvent : int {
  kClientConnected = 0,
  kClientDisconnected = 1,
  kClientPlaying = 2,
  kServerError = 3,
};

using RtspStatusCallback = std::function<void(RtspEvent event, int clients, const std::string& detail)>;

// Minimal RTSP/RTP 1.0 server.
// Video: H.264 Annex-B input, RFC 6184 packetization (single NAL + FU-A), RTP/AVP 96.
// Audio: ADTS AAC input, RFC 3640 MPEG4-generic (AAC-hbr), RTP/AVP 97.
// Transport: RTP/AVP over UDP and RTP/AVP/TCP (interleaved).
// This is the HarmonyOS replacement of the original libstreaming.so; the JNI
// surface (create/start/stop/isRunning/sendH264/sendAdts) is preserved 1:1.
class RtspServer {
 public:
  RtspServer() = default;
  ~RtspServer();
  RtspServer(const RtspServer&) = delete;
  RtspServer& operator=(const RtspServer&) = delete;

  bool Start(const RtspConfig& config, RtspStatusCallback callback);
  void Stop();
  bool IsRunning() const { return running_.load(); }
  int ClientCount();

  void PushH264(const uint8_t* data, size_t size, uint64_t tsUs);
  void PushAdts(const uint8_t* data, size_t size, uint64_t tsUs);

  struct VideoParams {
    bool h265 = false;
    bool ready = false;
    std::vector<uint8_t> vps;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
  };
  // Cached parameter sets (encoders emit them once at stream start; late
  // joiners like the recorder/snapshot need them injected).
  VideoParams GetVideoParams() const;

 private:
  struct Session;

  void AcceptLoop();
  void AddSession(const std::shared_ptr<Session>& session);
  void RemoveSession(Session* session);
  void Notify(RtspEvent event, int clients, const std::string& detail);
  std::string BuildSdp() const;

  enum class VideoCodec { H264, H265 };

  struct ParamSets {
    std::vector<uint8_t> vps;  // H.265 only
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
  };
  ParamSets GetParamSets() const;
  void UpdateParamSets(const uint8_t* nal, size_t size);
  static bool DetectH265(const uint8_t* data, size_t size);

  struct AacInfo {
    bool valid = false;
    int profile = 1;   // AAC-LC object type
    int sfIndex = 4;   // 44100 Hz
    int channels = 2;
  };
  AacInfo GetAacInfo() const;
  void UpdateAacInfo(int profile, int sfIndex, int channels);

  static uint64_t NowUs();

  std::atomic<bool> running_{false};
  int listenFd_ = -1;
  std::thread acceptThread_;
  RtspConfig config_{};

  mutable std::mutex sessionsMu_;
  std::vector<std::shared_ptr<Session>> sessions_;

  mutable std::mutex paramMu_;
  VideoCodec codec_ = VideoCodec::H264;
  bool codecDetected_ = false;
  ParamSets params_;
  AacInfo aac_;

  mutable std::mutex cbMu_;
  RtspStatusCallback callback_;
};

}  // namespace ipcam

#endif  // IPCAMERA_RTSP_SERVER_H
