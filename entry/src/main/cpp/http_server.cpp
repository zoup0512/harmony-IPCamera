#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dirent.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>

#include <hilog/log.h>

#include "jpeg_encoder.h"

#define LOG_DOMAIN 0xC010
#define LOG_TAG "HttpServer"

namespace ipcam {
namespace {

const char* kDashboardHtml =
    "<html><head><meta charset='utf-8'>"
    "<style>body{font-family:monospace;background:#111;color:#0d0}img{max-width:100%}</style>"
    "<meta http-equiv='refresh' content='0'>"
    "<title>IPCamera</title></head><body>"
    "<h2>IPCamera Live</h2>"
    "<img src='/video'><p>"
    "<a href='/snapshot.jpg'>snapshot.jpg</a> | "
    "<a href='/serverinfo'>serverinfo</a> | "
    "<a href='/size'>size</a> | "
    "<a href='/getarchives'>archives</a></p>"
    "<p><a href='/light'>/light (torch)</a> | "
    "<a href='/camswitch'>/camswitch</a></p>"
    "</body></html>\r\n";

const char* kMjpegBoundary = "ipcameraframe";

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool SendAll(int fd, const uint8_t* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool SendAll(int fd, const std::string& s) {
  return SendAll(fd, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

bool SendAll(int fd, const std::vector<uint8_t>& v) {
  return SendAll(fd, v.data(), v.size());
}

int B64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool Base64Decode(const std::string& in, std::string* out) {
  out->clear();
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\r' || c == '\n') continue;
    int v = B64Val(c);
    if (v < 0) return false;
    buf = (buf << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<char>((buf >> bits) & 0xFF));
    }
  }
  return true;
}

std::string Base64Encode(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i + 2 < in.size()) {
    uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) |
                 static_cast<uint8_t>(in[i + 2]);
    out += t[(v >> 18) & 63];
    out += t[(v >> 12) & 63];
    out += t[(v >> 6) & 63];
    out += t[v & 63];
    i += 3;
  }
  if (i + 1 == in.size()) {
    uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    out += t[(v >> 18) & 63];
    out += t[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
    out += t[(v >> 18) & 63];
    out += t[(v >> 12) & 63];
    out += t[(v >> 6) & 63];
    out += "=";
  }
  return out;
}

}  // namespace

HttpServer::~HttpServer() { Stop(); }

void HttpServer::SetJpegSource(JpegSource source) {
  std::lock_guard<std::mutex> lk(cbMu_);
  jpegSource_ = std::move(source);
}

void HttpServer::SetInfoSource(InfoSource source) {
  std::lock_guard<std::mutex> lk(cbMu_);
  infoSource_ = std::move(source);
}

void HttpServer::SetCommandSink(CommandSink sink) {
  std::lock_guard<std::mutex> lk(cbMu_);
  commandSink_ = std::move(sink);
}

void HttpServer::SetVoiceSink(VoiceSink sink) {
  std::lock_guard<std::mutex> lk(cbMu_);
  voiceSink_ = std::move(sink);
}

void HttpServer::SetArchiveSource(ArchiveSource source) {
  std::lock_guard<std::mutex> lk(cbMu_);
  archiveSource_ = std::move(source);
}

void HttpServer::SetArchiveFile(ArchiveFile file) {
  std::lock_guard<std::mutex> lk(cbMu_);
  archiveFile_ = std::move(file);
}

bool HttpServer::Start(int port, const std::string& user, const std::string& password,
                       const std::string& archiveDir) {
  if (running_.load()) return false;
  user_ = user;
  password_ = password;
  archiveDir_ = archiveDir;

  listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) return false;
  int one = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(listenFd_, 8) < 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  running_ = true;
  acceptThread_ = std::thread(&HttpServer::AcceptLoop, this);
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "http server on :%{public}d", port);
  return true;
}

void HttpServer::Stop() {
  if (!running_.exchange(false)) return;
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
  if (acceptThread_.joinable()) acceptThread_.join();
}

void HttpServer::AcceptLoop() {
  while (running_.load()) {
    pollfd p{listenFd_, POLLIN, 0};
    if (::poll(&p, 1, 300) <= 0) continue;
    if (!running_.load()) break;
    int cfd = ::accept(listenFd_, nullptr, nullptr);
    if (cfd < 0) continue;
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    std::thread([this, cfd]() { HandleConnection(cfd); }).detach();
  }
}

bool HttpServer::Authorized(const std::string& request) const {
  // find Authorization header
  size_t pos = request.find("Authorization:");
  if (pos == std::string::npos) return false;
  size_t end = request.find("\r\n", pos);
  std::string line =
      request.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  const std::string prefix = "Basic ";
  size_t vp = line.find(prefix);
  if (vp == std::string::npos) return false;
  std::string b64 = line.substr(vp + prefix.size());
  size_t sp = b64.find(' ');
  if (sp != std::string::npos) b64 = b64.substr(0, sp);
  std::string decoded;
  if (!Base64Decode(b64, &decoded)) return false;
  return decoded == user_ + ":" + password_;
}

void HttpServer::Respond(int fd, int code, const std::string& contentType,
                         const std::vector<uint8_t>& body, bool closeConn) {
  std::string head = "HTTP/1.1 " + std::to_string(code);
  switch (code) {
    case 200: head += " OK"; break;
    case 401: head += " Unauthorized"; break;
    case 404: head += " Not Found"; break;
    case 411: head += " Length Required"; break;
    default: break;
  }
  head += "\r\nContent-Type: " + contentType;
  head += "\r\nContent-Length: " + std::to_string(body.size());
  if (closeConn) head += "\r\nConnection: close";
  head += "\r\n\r\n";
  SendAll(fd, head);
  if (!body.empty()) SendAll(fd, body);
}

void HttpServer::Respond401(int fd) {
  std::string head =
      "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"IPCamera\"\r\n"
      "Content-Length: 0\r\nConnection: close\r\n\r\n";
  SendAll(fd, head);
}

void HttpServer::RespondText(int fd, const std::string& text) {
  Respond(fd, 200, "text/plain",
          std::vector<uint8_t>(text.begin(), text.end()), true);
}

bool HttpServer::HandleRequest(int fd, const std::string& method, const std::string& path,
                               const std::vector<uint8_t>& body) {
  (void)method;
  JpegSource jpegSource;
  InfoSource infoSource;
  CommandSink commandSink;
  VoiceSink voiceSink;
  ArchiveSource archiveSource;
  ArchiveFile archiveFile;
  {
    std::lock_guard<std::mutex> lk(cbMu_);
    jpegSource = jpegSource_;
    infoSource = infoSource_;
    commandSink = commandSink_;
    voiceSink = voiceSink_;
    archiveSource = archiveSource_;
    archiveFile = archiveFile_;
  }

  if (path == "/" || path == "/index" || path == "/main.html") {
    std::string html(kDashboardHtml);
    Respond(fd, 200, "text/html", std::vector<uint8_t>(html.begin(), html.end()), true);
    return true;
  }
  if (path == "/snapshot.jpg" || path == "/getsnapshot") {
    std::vector<uint8_t> jpg = jpegSource ? jpegSource() : std::vector<uint8_t>();
    if (jpg.empty()) {
      RespondText(fd, "no frame yet");
      return true;
    }
    Respond(fd, 200, "image/jpeg", jpg, true);
    return true;
  }
  if (path == "/video") {
    // multipart/x-mixed-replace MJPEG
    std::string head =
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace;boundary=" +
        std::string(kMjpegBoundary) + "\r\n\r\n";
    if (!SendAll(fd, head)) return false;
    auto lastSend = std::chrono::steady_clock::now();
    while (running_.load()) {
      auto now = std::chrono::steady_clock::now();
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSend).count();
      if (ms < 100) {
        usleep(20 * 1000);
        continue;
      }
      lastSend = now;
      std::vector<uint8_t> jpg = jpegSource ? jpegSource() : std::vector<uint8_t>();
      if (jpg.empty()) continue;
      std::string part = "--" + std::string(kMjpegBoundary) +
                         "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                         std::to_string(jpg.size()) + "\r\n\r\n";
      if (!SendAll(fd, part) || !SendAll(fd, jpg) || !SendAll(fd, "\r\n")) return false;
    }
    return false;
  }
  if (path == "/serverinfo") {
    RespondText(fd, infoSource ? infoSource() : "no info");
    return true;
  }
  if (path.rfind("/size", 0) == 0) {
    RespondText(fd, infoSource ? infoSource() : "no info");
    return true;
  }
  if (path == "/light") {
    if (commandSink) commandSink("light");
    RespondText(fd, "light toggled");
    return true;
  }
  if (path == "/camswitch") {
    if (commandSink) commandSink("camswitch");
    RespondText(fd, "camera switched");
    return true;
  }
  if (path == "/getarchives") {
    RespondText(fd, archiveSource ? archiveSource() : "");
    return true;
  }
  if (path.rfind("/get/ipc_", 0) == 0) {
    std::string name = path.substr(5);
    std::vector<uint8_t> data;
    if (archiveFile && archiveFile(name, &data) && !data.empty()) {
      Respond(fd, 200, "video/mp4", data, true);
    } else {
      Respond(fd, 404, "text/plain", std::vector<uint8_t>(), true);
    }
    return true;
  }
  if (path == "/put_voice") {
    if (voiceSink && !body.empty()) voiceSink(body);
    RespondText(fd, "voice received");
    return true;
  }
  Respond(fd, 404, "text/plain", std::vector<uint8_t>(), true);
  return true;
}

void HttpServer::HandleConnection(int fd) {
  std::string buf;
  char tmp[8192];
  while (running_.load()) {
    // read until we have full headers
    size_t hdrEnd;
    while ((hdrEnd = buf.find("\r\n\r\n")) == std::string::npos) {
      pollfd p{fd, POLLIN, 0};
      int pr = ::poll(&p, 1, 5000);
      if (pr <= 0) {
        ::close(fd);
        return;
      }
      ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        ::close(fd);
        return;
      }
      buf.append(tmp, static_cast<size_t>(n));
      if (buf.size() > 512 * 1024) {
        ::close(fd);
        return;
      }
    }
    std::string head = buf.substr(0, hdrEnd);
    size_t bodyStart = hdrEnd + 4;

    std::istringstream iss(head);
    std::string method, path, version;
    iss >> method >> path >> version;
    size_t contentLength = 0;
    bool keepAlive = true;
    std::string line;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string lower = ToLower(line);
      if (lower.rfind("content-length:", 0) == 0) {
        contentLength = static_cast<size_t>(atoll(line.substr(15).c_str()));
      }
      if (lower.rfind("connection: close", 0) == 0) keepAlive = false;
    }

    // read body (for /put_voice)
    while (buf.size() - bodyStart < contentLength) {
      pollfd p{fd, POLLIN, 0};
      int pr = ::poll(&p, 1, 5000);
      if (pr <= 0) {
        ::close(fd);
        return;
      }
      ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        ::close(fd);
        return;
      }
      buf.append(tmp, static_cast<size_t>(n));
      if (buf.size() > 8 * 1024 * 1024) {
        ::close(fd);
        return;
      }
    }
    std::vector<uint8_t> body(buf.begin() + static_cast<long>(bodyStart),
                              buf.begin() + static_cast<long>(bodyStart + contentLength));
    buf.erase(0, bodyStart + contentLength);

    bool authed = Authorized(head);
    std::string lowerPath = ToLower(path);
    bool isVoice = lowerPath == "/put_voice";
    if (!authed && !isVoice) {
      // /put_voice from hardware devices skips auth (original parity);
      // everything else needs Basic auth
      Respond401(fd);
      if (!keepAlive) {
        ::close(fd);
        return;
      }
      continue;
    }

    bool cont = HandleRequest(fd, method, lowerPath, body);
    if (!cont || !keepAlive) {
      ::close(fd);
      return;
    }
  }
  ::close(fd);
}

}  // namespace ipcam
