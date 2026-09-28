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

// Minimal HTTP/1.1 server exposing the IP-camera web console, mirroring the
// original Android app's endpoints:
//   / /index /main.html      simple dashboard
//   /snapshot.jpg /getsnapshot  latest JPEG still
//   /video                   multipart/x-mixed-replace MJPEG
//   /serverinfo /size        plain-text status
//   /light /camswitch        torch + camera facing actions (via command sink)
//   /getarchives /get/ipc_*  recording archives
//   /put_voice               talk-back audio upload (AAC ADTS body)
// All routes require Basic auth.
class HttpServer {
 public:
  enum class VoiceResult {
    Accepted,
    Invalid,
    Busy,
    Unavailable,
  };

  using CommandSink = std::function<void(const std::string& command)>;
  using JpegSource = std::function<std::vector<uint8_t>()>;
  using InfoSource = std::function<std::string()>;
  using VoiceSink = std::function<VoiceResult(const std::vector<uint8_t>& body)>;
  using ArchiveSource = std::function<std::string()>;  // list text
  using ArchiveFile =
      std::function<bool(const std::string& name, std::vector<uint8_t>* out)>;

  HttpServer();
  ~HttpServer();

  bool Start(int port, const std::string& user, const std::string& password,
             const std::string& archiveDir);
  void Stop();
  bool IsRunning() const { return running_.load(); }

  void SetJpegSource(JpegSource source);
  void SetInfoSource(InfoSource source);
  void SetCommandSink(CommandSink sink);
  void SetVoiceSink(VoiceSink sink);
  void SetArchiveSource(ArchiveSource source);
  void SetArchiveFile(ArchiveFile file);

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
  void Respond(int fd, int code, const std::string& contentType,
               const std::vector<uint8_t>& body,
               const std::string& extraHeaders = std::string());
  void Respond401(int fd);
  void RespondText(int fd, const std::string& text);
  bool CheckAuth(const std::string& headerValue) const;

  std::atomic<bool> running_{false};
  std::atomic<int> listenFd_{-1};
  std::thread acceptThread_;
  std::string user_;
  std::string password_;
  std::string archiveDir_;

  std::mutex lifecycleMu_;
  std::condition_variable lifecycleCv_;
  bool lifecycleBusy_ = false;

  std::mutex workersMu_;
  std::condition_variable workersCv_;
  std::vector<std::unique_ptr<Worker>> workers_;

  std::mutex cbMu_;
  JpegSource jpegSource_;
  InfoSource infoSource_;
  CommandSink commandSink_;
  VoiceSink voiceSink_;
  ArchiveSource archiveSource_;
  ArchiveFile archiveFile_;
};

}  // namespace ipcam

#endif  // IPCAMERA_HTTP_SERVER_H
