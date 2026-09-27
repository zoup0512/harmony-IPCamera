#ifndef IPCAMERA_HTTP_SERVER_H
#define IPCAMERA_HTTP_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
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
  using CommandSink = std::function<void(const std::string& command)>;
  using JpegSource = std::function<std::vector<uint8_t>()>;
  using InfoSource = std::function<std::string()>;
  using VoiceSink = std::function<void(const std::vector<uint8_t>& body)>;
  using ArchiveSource = std::function<std::string()>;             // list text
  using ArchiveFile = std::function<bool(const std::string& name, std::vector<uint8_t>* out)>;

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
  void AcceptLoop();
  void HandleConnection(int fd);
  bool HandleRequest(int fd, const std::string& method, const std::string& path,
                     const std::vector<uint8_t>& body);
  void Respond(int fd, int code, const std::string& contentType,
               const std::vector<uint8_t>& body, bool closeConn);
  void Respond401(int fd);
  void RespondText(int fd, const std::string& text);
  bool CheckAuth(const std::string& headerValue) const;
  bool Authorized(const std::string& request) const;

  std::atomic<bool> running_{false};
  int listenFd_ = -1;
  std::thread acceptThread_;
  std::string user_;
  std::string password_;
  std::string archiveDir_;

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
