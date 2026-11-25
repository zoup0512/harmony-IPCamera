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

#include "media_time.h"

namespace ipcam {

struct RtspConfig {
  int port = 8554;
  std::string path = "live";
  std::string user;      // empty disables basic auth
  std::string password;
  bool loopbackOnly = false;  // bind ::1 / 127.0.0.1 instead of wildcard addresses
};

enum class RtspEvent : int {
  kClientConnected = 0,
  kClientDisconnected = 1,
  kClientPlaying = 2,
  kServerError = 3,
  kCameraSizeChanged = 4,
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
  int BoundV4Port() const { return IsRunning() ? boundV4Port_.load() : 0; }
  int ClientCount();

  // Exclusive admission window, independent of Start/Stop. Fails without changes
  // if another window or any unfinished TCP session exists (even before PLAY).
  // Registrations during the window are closed without session events. Only a
  // successful caller must call EndVideoChange(), exactly once, on every exit.
  bool TryBeginVideoChange();
  void EndVideoChange();

  void PushH264(const uint8_t* data, size_t size, TimestampUs tsUs);
  void PushAdts(const uint8_t* data, size_t size, TimestampUs tsUs);

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
  // Caller fences the old producer first. hevc hints the SDP codec until the
  // next parameter anchor; it does not mark the empty sets ready. Keeps AAC.
  void ResetVideoParams(bool hevc = false);
  // Caller fences the microphone producer; SDP omits audio until fresh ADTS.
  // Does not renegotiate existing sessions or discard cached video parameters.
  void ResetAudioParams();

 private:
  struct Session;

  void AcceptLoop();
  void AcceptOne(int listenFd);
  void CloseListeners();
  bool TryAddSession(const std::shared_ptr<Session>& session);
  void ReapFinishedSessions();
  void Notify(RtspEvent event, int clients, const std::string& detail);
  std::string BuildSdp(bool ipv6, const std::string& localIp) const;

  enum class VideoCodec { H264, H265 };

  struct ParamSets {
    std::vector<uint8_t> vps;  // H.265 only
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
  };
  void UpdateParamSetsLocked(const uint8_t* nal, size_t size);
  static bool DetectH265(const uint8_t* data, size_t size);

  struct AacInfo {
    bool valid = false;
    int profile = 1;   // AAC-LC object type
    int sfIndex = 4;   // 44100 Hz
    int channels = 2;
  };
  AacInfo GetAacInfo() const;
  void UpdateAacInfo(int profile, int sfIndex, int channels);

  static TimestampUs NowUs();

  std::atomic<int> boundV4Port_{0};
  std::atomic<bool> running_{false};
  int listeners_[2]{-1, -1};  // immutable until acceptThread_ joined
  int listenerCount_ = 0;
  mutable std::mutex lifecycleMu_;
  std::thread acceptThread_;
  mutable std::mutex configMu_;
  RtspConfig config_{};

  mutable std::mutex sessionsMu_;
  bool videoChangeInProgress_ = false;  // guarded by sessionsMu_
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
