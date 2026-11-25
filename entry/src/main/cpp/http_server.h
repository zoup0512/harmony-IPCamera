#ifndef IPCAMERA_HTTP_SERVER_H
#define IPCAMERA_HTTP_SERVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ipcam {

class OpusStream;
class FlvStream;

// Minimal HTTP/1.1 server exposing the IP-camera web console, mirroring the
// original Android app's endpoints:
//   / /index /main.html      simple dashboard
//   /snapshot.jpg            read the latest JPEG; never saves a file
//   GET /getsnapshot         Basic auth; save one JPEG in Start's archiveDir (filesDir),
//                            return "Snapshot <basename>" after successful publication
//   GET /live.flv            live HTTP-FLV (AVC and optional AAC); no HEVC
//   /video                   multipart/x-mixed-replace MJPEG
//   /audio.opus              live Ogg Opus microphone stream (when mic runs)
//   /serverinfo              plain-text status
//   GET /size                Android list fragment; ?format=json for the dashboard
//   GET /sizeN               synchronously select the Nth advertised size (zero-based)
//   /light /camswitch        torch + camera facing actions (via command sink)
//   /getarchives /get/...    legacy recording list and file API
//   /recordings              browser list, player and downloads (Basic auth)
//   /put_voice               talk-back audio upload (AAC ADTS body)
// All routes require Basic by default. Only GET /video has an opt-in legacy exception.
class HttpServer {
 public:
  enum class VoiceResult {
    Accepted,
    Invalid,
    Busy,
    Unavailable,
  };

  using CommandSink = std::function<bool(const std::string& command)>;
  using JpegSource = std::function<std::vector<uint8_t>()>;
  using InfoSource = std::function<std::string()>;
  enum class SizeResult { Ok, NotFound, CameraClosed, Busy, RestartFailed };
  struct VideoSize { int width; int height; };
  struct SizeList {
    std::vector<VideoSize> sizes;  // Stable index order for the active camera.
    int currentIndex = -1;         // -1 if the active size is not listed.
  };
  using SizeSource = std::function<SizeResult(SizeList&)>;
  // Revalidate the index under the camera's control lock; return Ok only after
  // the target is running (including an already-active target), never on enqueue.
  using SizeSink = std::function<SizeResult(int index)>;
  using VoiceSink = std::function<VoiceResult(const std::vector<uint8_t>& body)>;
  using ArchiveSource = std::function<std::string()>;  // list text
  // Returns an opened read-only descriptor; ownership transfers to HttpServer.
  using ArchiveFile = std::function<int(const std::string& name)>;

  HttpServer();
  ~HttpServer();

  // Set allowLegacyVideoPassword only after explicit user confirmation: URL history
  // and proxy logs may expose the reusable account password, not just video access.
  bool Start(int port, const std::string& user, const std::string& password,
             const std::string& archiveDir, bool allowLegacyVideoPassword = false);
  void Stop();
  bool IsRunning() const { return running_.load(); }
  unsigned MjpegViewerCount() const { return mjpegViewers_.load(); }
  int BoundV4Port() const { return IsRunning() ? boundV4Port_.load() : 0; }

  void SetJpegSource(JpegSource source);
  // /getsnapshot only. From each invocation wait a bounded time for a newer
  // encoded input. Empty on failure/cancellation; never return the old cache.
  // running is borrowed only for the call; Stop clears it before joining workers.
  using FreshJpegSource = std::function<std::vector<uint8_t>(const std::atomic<bool>& running)>;
  void SetFreshJpegSource(FreshJpegSource source);
  void SetInfoSource(InfoSource source);
  void SetSizeCallbacks(SizeSource list, SizeSink select);
  void SetCommandSink(CommandSink sink);
  void SetVoiceSink(VoiceSink sink);
  void SetArchiveSource(ArchiveSource source);
  void SetArchiveFile(ArchiveFile file);
  void SetOpusStream(OpusStream* stream);
  void SetFlvStream(std::shared_ptr<FlvStream> stream);

 private:
  struct Worker;

  void BeginLifecycleOperation();
  void EndLifecycleOperation();
  void StopInternal();
  void AcceptLoop();
  void ReapFinishedWorkers();
  void WorkerMain(Worker* worker);
  void HandleConnection(int fd);
  bool HandleRequest(int fd, const std::string& method, const std::string& path,
                     const std::vector<uint8_t>& body);
  void HandleFlv(int fd, const std::string& method);
  void HandleSize(int fd, const std::string& method, const std::string& path, bool json);
  void HandleRecordings(int fd, const std::string& method, const std::string& subpath,
                        const std::vector<std::string>* ranges);
  void Respond(int fd, int code, const std::string& contentType,
               const std::vector<uint8_t>& body,
               const std::string& extraHeaders = std::string());
  bool RespondFile(int fd, int fileFd);
  bool RespondRecording(int fd, int fileFd, const std::vector<std::string>* ranges,
                        bool headOnly, const std::string& disposition);
  void Respond401(int fd);
  void RespondText(int fd, const std::string& text);
  bool CheckAuth(const std::string& headerValue) const;

  std::atomic<int> boundV4Port_{0};
  std::atomic<bool> running_{false};
  std::atomic<int> listenFd_{-1};  // Primary listener; may be injected alone by host tests.
  std::atomic<int> secondListenFd_{-1};
  std::atomic<unsigned> mjpegViewers_{0};
  std::thread acceptThread_;
  std::string user_;
  std::string password_;
  bool allowLegacyVideoPassword_ = false;
  std::string archiveDir_;

  std::mutex lifecycleMu_;
  std::condition_variable lifecycleCv_;
  bool lifecycleBusy_ = false;

  std::mutex workersMu_;
  std::condition_variable workersCv_;
  std::vector<std::unique_ptr<Worker>> workers_;

  std::mutex cbMu_;
  JpegSource jpegSource_;
  FreshJpegSource freshJpegSource_;
  InfoSource infoSource_;
  SizeSource sizeSource_;
  SizeSink sizeSink_;
  CommandSink commandSink_;
  VoiceSink voiceSink_;
  ArchiveSource archiveSource_;
  ArchiveFile archiveFile_;
  OpusStream* opusStream_ = nullptr;
  std::shared_ptr<FlvStream> flvStream_;
};

}  // namespace ipcam

#endif  // IPCAMERA_HTTP_SERVER_H
