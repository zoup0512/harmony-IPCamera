#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

#include <hilog/log.h>

#include "opus_stream.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "HttpServer"

namespace ipcam {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxHeaderBytes = 64u * 1024u;
constexpr size_t kMaxVoiceBodyBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxWorkers = 32;
constexpr std::chrono::seconds kHeaderDeadline(5);
constexpr std::chrono::seconds kBodyDeadline(5);
constexpr std::chrono::seconds kSendDeadline(5);

const char* kDashboardHtml =
    "<html><head><meta charset='utf-8'>"
    "<style>body{font-family:monospace;background:#111;color:#0d0}img{max-width:100%}</style>"
    "<meta http-equiv='refresh' content='0'>"
    "<title>IPCamera</title></head><body>"
    "<h2>IPCamera Live</h2>"
    "<img src='/video'><p>"
    "<audio src='/audio.opus' controls preload='none'></audio><p>"
    "<a href='/snapshot.jpg'>snapshot.jpg</a> | "
    "<a href='/serverinfo'>serverinfo</a> | "
    "<a href='/size'>size</a> | "
    "<a href='/getarchives'>archives</a></p>"
    "<p><a href='/light'>/light (torch)</a> | "
    "<a href='/camswitch'>/camswitch</a></p>"
    "</body></html>\r\n";

const char* kMjpegBoundary = "ipcameraframe";

struct CaseInsensitiveLess {
  bool operator()(const std::string& lhs, const std::string& rhs) const {
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](unsigned char a, unsigned char b) {
          return std::tolower(a) < std::tolower(b);
        });
  }
};

using HeaderMap = std::map<std::string, std::vector<std::string>, CaseInsensitiveLess>;

struct ParsedRequest {
  std::string method;
  std::string path;
  std::string version;
  HeaderMap headers;
};

std::string ToLower(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool IsTokenChar(unsigned char c) {
  if (std::isalnum(c) != 0) return true;
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

bool IsValidToken(const std::string& value) {
  if (value.empty()) return false;
  for (unsigned char c : value) {
    if (!IsTokenChar(c)) return false;
  }
  return true;
}

bool IsValidTarget(const std::string& value) {
  if (value.empty() || value[0] != '/') return false;
  for (unsigned char c : value) {
    if (c <= 0x20 || c == 0x7f) return false;
  }
  return true;
}

std::string TrimOws(const std::string& value) {
  size_t begin = 0;
  while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
  size_t end = value.size();
  while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) --end;
  return value.substr(begin, end - begin);
}

int RemainingMillis(Clock::time_point deadline) {
  auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  if (remaining <= 0) return 0;
  if (remaining > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
  return static_cast<int>(remaining);
}

bool WaitForEvent(int fd, short events, Clock::time_point deadline) {
  for (;;) {
    int timeout = RemainingMillis(deadline);
    if (timeout <= 0) return false;
    pollfd p{fd, events, 0};
    int result = ::poll(&p, 1, timeout);
    if (result > 0) {
      if ((p.revents & POLLNVAL) != 0) return false;
      if ((p.revents & events) != 0) return true;
      if ((p.revents & (POLLERR | POLLHUP)) != 0) return false;
      continue;
    }
    if (result == 0) return false;
    if (errno != EINTR) return false;
  }
}

bool SendAll(int fd, const uint8_t* data, size_t len,
             Clock::time_point deadline = Clock::now() + kSendDeadline) {
  size_t offset = 0;
  while (offset < len) {
    if (!WaitForEvent(fd, POLLOUT, deadline)) return false;
    ssize_t sent = ::send(fd, data + offset, len - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) {
      offset += static_cast<size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    return false;
  }
  return true;
}

bool SendAll(int fd, const std::string& value,
             Clock::time_point deadline = Clock::now() + kSendDeadline) {
  return SendAll(fd, reinterpret_cast<const uint8_t*>(value.data()), value.size(), deadline);
}

bool SendAll(int fd, const std::vector<uint8_t>& value,
             Clock::time_point deadline = Clock::now() + kSendDeadline) {
  if (value.empty()) return true;
  return SendAll(fd, value.data(), value.size(), deadline);
}

ssize_t PeekSome(int fd, uint8_t* data, size_t capacity, Clock::time_point deadline) {
  if (capacity == 0) return -1;
  for (;;) {
    if (!WaitForEvent(fd, POLLIN, deadline)) return -1;
    ssize_t received = ::recv(fd, data, capacity, MSG_DONTWAIT | MSG_PEEK);
    if (received > 0) return received;
    if (received == 0) return -1;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    return -1;
  }
}

bool ReceiveExact(int fd, uint8_t* data, size_t size, Clock::time_point deadline) {
  size_t offset = 0;
  while (offset < size) {
    if (!WaitForEvent(fd, POLLIN, deadline)) return false;
    ssize_t received = ::recv(fd, data + offset, size - offset, MSG_DONTWAIT);
    if (received > 0) {
      offset += static_cast<size_t>(received);
      continue;
    }
    if (received == 0) return false;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    return false;
  }
  return true;
}

bool ReadHeaderOnly(int fd, std::string* header, Clock::time_point deadline,
                    bool* tooLarge) {
  if (header == nullptr || tooLarge == nullptr) return false;
  *tooLarge = false;
  header->clear();
  header->reserve(8192);
  uint8_t peeked[8192];

  for (;;) {
    if (header->size() >= kMaxHeaderBytes) {
      *tooLarge = true;
      return false;
    }
    size_t capacity = std::min(sizeof(peeked), kMaxHeaderBytes - header->size());
    ssize_t count = PeekSome(fd, peeked, capacity, deadline);
    if (count <= 0) return false;

    size_t previousSize = header->size();
    header->append(reinterpret_cast<const char*>(peeked), static_cast<size_t>(count));
    size_t searchStart = previousSize > 3 ? previousSize - 3 : 0;
    size_t end = header->find("\r\n\r\n", searchStart);
    if (end != std::string::npos) {
      size_t bytesToConsume = end + 4 - previousSize;
      header->resize(previousSize);
      std::vector<uint8_t> consumed(bytesToConsume);
      if (!ReceiveExact(fd, consumed.data(), consumed.size(), deadline)) return false;
      header->append(reinterpret_cast<const char*>(consumed.data()), consumed.size());
      return true;
    }

    header->resize(previousSize);
    if (previousSize + static_cast<size_t>(count) >= kMaxHeaderBytes) {
      *tooLarge = true;
      return false;
    }
    std::vector<uint8_t> consumed(static_cast<size_t>(count));
    if (!ReceiveExact(fd, consumed.data(), consumed.size(), deadline)) return false;
    header->append(reinterpret_cast<const char*>(consumed.data()), consumed.size());
  }
}

bool ParseRequestHead(const std::string& head, ParsedRequest* request) {
  if (request == nullptr) return false;
  size_t lineEnd = head.find("\r\n");
  std::string requestLine = lineEnd == std::string::npos ? head : head.substr(0, lineEnd);
  if (requestLine.empty() || requestLine.find('\t') != std::string::npos) return false;

  size_t firstSpace = requestLine.find(' ');
  if (firstSpace == std::string::npos || firstSpace == 0) return false;
  size_t secondSpace = requestLine.find(' ', firstSpace + 1);
  if (secondSpace == std::string::npos || secondSpace == firstSpace + 1 ||
      requestLine.find(' ', secondSpace + 1) != std::string::npos) {
    return false;
  }

  request->method = requestLine.substr(0, firstSpace);
  request->path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
  request->version = requestLine.substr(secondSpace + 1);
  if (!IsValidToken(request->method) || !IsValidTarget(request->path) ||
      (request->version != "HTTP/1.1" && request->version != "HTTP/1.0")) {
    return false;
  }

  request->headers.clear();
  size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
  while (pos < head.size()) {
    size_t end = head.find("\r\n", pos);
    if (end == std::string::npos) end = head.size();
    std::string line = head.substr(pos, end - pos);
    if (line.empty() || line[0] == ' ' || line[0] == '\t') return false;
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    std::string name = line.substr(0, colon);
    if (!IsValidToken(name)) return false;
    std::string value = TrimOws(line.substr(colon + 1));
    for (unsigned char c : value) {
      if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
    }
    request->headers[name].push_back(std::move(value));
    if (end == head.size()) break;
    pos = end + 2;
  }
  return true;
}

bool ParseContentLength(const HeaderMap& headers, size_t* value) {
  auto it = headers.find("Content-Length");
  if (it == headers.end() || it->second.size() != 1 || value == nullptr) return false;
  const std::string& text = it->second.front();
  if (text.empty()) return false;
  size_t parsed = 0;
  for (unsigned char c : text) {
    if (c < '0' || c > '9') return false;
    size_t digit = static_cast<size_t>(c - '0');
    if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  *value = parsed;
  return true;
}

int B64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool Base64Decode(const std::string& input, std::string* output) {
  if (output == nullptr || input.empty() || input.size() % 4 != 0) return false;
  output->clear();
  output->reserve((input.size() / 4) * 3);
  for (size_t i = 0; i < input.size(); i += 4) {
    int a = B64Val(input[i]);
    int b = B64Val(input[i + 1]);
    if (a < 0 || b < 0) return false;
    bool pad2 = input[i + 2] == '=';
    bool pad3 = input[i + 3] == '=';
    if (pad2 && !pad3) return false;
    if ((pad2 || pad3) && i + 4 != input.size()) return false;
    int c = pad2 ? 0 : B64Val(input[i + 2]);
    int d = pad3 ? 0 : B64Val(input[i + 3]);
    if (c < 0 || d < 0) return false;
    uint32_t block = (static_cast<uint32_t>(a) << 18) |
                     (static_cast<uint32_t>(b) << 12) |
                     (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
    output->push_back(static_cast<char>((block >> 16) & 0xff));
    if (!pad2) output->push_back(static_cast<char>((block >> 8) & 0xff));
    if (!pad3) output->push_back(static_cast<char>(block & 0xff));
    if (pad2 && (b & 0x0f) != 0) return false;
    if (pad3 && !pad2 && (c & 0x03) != 0) return false;
  }
  return true;
}

const char* StatusText(int code) {
  switch (code) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 411:
      return "Length Required";
    case 413:
      return "Payload Too Large";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

}  // namespace

struct HttpServer::Worker {
  std::thread thread;
  std::shared_ptr<std::atomic<bool>> finished =
      std::make_shared<std::atomic<bool>>(false);
  std::atomic<int> fd{-1};
};

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { Stop(); }

void HttpServer::SetJpegSource(JpegSource source) {
  std::lock_guard<std::mutex> lock(cbMu_);
  jpegSource_ = std::move(source);
}

void HttpServer::SetInfoSource(InfoSource source) {
  std::lock_guard<std::mutex> lock(cbMu_);
  infoSource_ = std::move(source);
}

void HttpServer::SetCommandSink(CommandSink sink) {
  std::lock_guard<std::mutex> lock(cbMu_);
  commandSink_ = std::move(sink);
}

void HttpServer::SetVoiceSink(VoiceSink sink) {
  std::lock_guard<std::mutex> lock(cbMu_);
  voiceSink_ = std::move(sink);
}

void HttpServer::SetArchiveSource(ArchiveSource source) {
  std::lock_guard<std::mutex> lock(cbMu_);
  archiveSource_ = std::move(source);
}

void HttpServer::SetArchiveFile(ArchiveFile file) {
  std::lock_guard<std::mutex> lock(cbMu_);
  archiveFile_ = std::move(file);
}

void HttpServer::SetOpusStream(OpusStream* stream) {
  std::lock_guard<std::mutex> lock(cbMu_);
  opusStream_ = stream;
}

void HttpServer::BeginLifecycleOperation() {
  std::unique_lock<std::mutex> lock(lifecycleMu_);
  lifecycleCv_.wait(lock, [this]() { return !lifecycleBusy_; });
  lifecycleBusy_ = true;
}

void HttpServer::EndLifecycleOperation() {
  {
    std::lock_guard<std::mutex> lock(lifecycleMu_);
    lifecycleBusy_ = false;
  }
  lifecycleCv_.notify_one();
}

bool HttpServer::Start(int port, const std::string& user, const std::string& password,
                       const std::string& archiveDir) {
  BeginLifecycleOperation();
  if (running_.load()) {
    EndLifecycleOperation();
    return false;
  }
  StopInternal();

  user_ = user;
  password_ = password;
  archiveDir_ = archiveDir;

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    EndLifecycleOperation();
    return false;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(fd, 8) < 0) {
    ::close(fd);
    EndLifecycleOperation();
    return false;
  }

  listenFd_.store(fd);
  running_.store(true);
  try {
    acceptThread_ = std::thread(&HttpServer::AcceptLoop, this);
  } catch (...) {
    running_.store(false);
    listenFd_.store(-1);
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    EndLifecycleOperation();
    return false;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "http server on :%{public}d", port);
  EndLifecycleOperation();
  return true;
}

void HttpServer::Stop() {
  BeginLifecycleOperation();
  StopInternal();
  EndLifecycleOperation();
}

void HttpServer::StopInternal() {
  running_.store(false);

  int listener = listenFd_.exchange(-1);
  if (listener >= 0) {
    ::shutdown(listener, SHUT_RDWR);
    ::close(listener);
  }

  if (acceptThread_.joinable()) acceptThread_.join();

  std::vector<std::unique_ptr<Worker>> workers;
  {
    std::unique_lock<std::mutex> lock(workersMu_);
    for (const auto& worker : workers_) {
      int client = worker->fd.load();
      if (client >= 0) ::shutdown(client, SHUT_RDWR);
    }
    workersCv_.wait(lock, [this]() {
      for (const auto& worker : workers_) {
        if (!worker->finished->load()) return false;
      }
      return true;
    });
    workers.swap(workers_);
  }
  for (auto& worker : workers) {
    if (worker->thread.joinable()) worker->thread.join();
  }
}

void HttpServer::ReapFinishedWorkers() {
  std::vector<std::unique_ptr<Worker>> finished;
  {
    std::lock_guard<std::mutex> lock(workersMu_);
    auto it = workers_.begin();
    while (it != workers_.end()) {
      if ((*it)->finished->load()) {
        finished.push_back(std::move(*it));
        it = workers_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& worker : finished) {
    if (worker->thread.joinable()) worker->thread.join();
  }
}

void HttpServer::AcceptLoop() {
  while (running_.load()) {
    ReapFinishedWorkers();
    int listener = listenFd_.load();
    if (listener < 0) break;
    pollfd p{listener, POLLIN, 0};
    int result = ::poll(&p, 1, 250);
    if (result < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (result == 0) continue;
    if (!running_.load()) break;
    if ((p.revents & POLLIN) == 0) {
      if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) break;
      continue;
    }

    int client = ::accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      if (!running_.load()) break;
      continue;
    }
    int one = 1;
    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    {
      std::lock_guard<std::mutex> lock(workersMu_);
      if (workers_.size() >= kMaxWorkers) {
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
        continue;
      }
    }

    auto worker = std::make_unique<Worker>();
    Worker* raw = worker.get();
    raw->fd.store(client);
    try {
      raw->thread = std::thread(&HttpServer::WorkerMain, this, raw);
    } catch (...) {
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
      raw->fd.store(-1);
      continue;
    }

    bool keepWorker = false;
    {
      std::lock_guard<std::mutex> lock(workersMu_);
      if (running_.load()) {
        workers_.push_back(std::move(worker));
        keepWorker = true;
      }
    }
    if (!keepWorker) {
      std::unique_lock<std::mutex> lock(workersMu_);
      int active = worker->fd.load();
      if (active >= 0) ::shutdown(active, SHUT_RDWR);
      workersCv_.wait(lock, [&worker]() { return worker->finished->load(); });
      lock.unlock();
      if (worker->thread.joinable()) worker->thread.join();
    }
  }
  ReapFinishedWorkers();
}

void HttpServer::WorkerMain(Worker* worker) {
  try {
    int fd = worker->fd.load();
    HandleConnection(fd);
  } catch (...) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "connection worker failed");
  }
  int owned = worker->fd.exchange(-1);
  if (owned >= 0) ::close(owned);
  {
    std::lock_guard<std::mutex> lock(workersMu_);
    worker->finished->store(true);
  }
  workersCv_.notify_all();
}

bool HttpServer::CheckAuth(const std::string& headerValue) const {
  std::string value = TrimOws(headerValue);
  size_t separator = value.find_first_of(" \t");
  if (separator == std::string::npos) return false;
  if (ToLower(value.substr(0, separator)) != "basic") return false;
  size_t tokenStart = value.find_first_not_of(" \t", separator);
  if (tokenStart == std::string::npos) return false;
  std::string token = value.substr(tokenStart);
  if (token.find_first_of(" \t") != std::string::npos) return false;
  std::string decoded;
  if (!Base64Decode(token, &decoded)) return false;
  return decoded == user_ + ":" + password_;
}

void HttpServer::Respond(int fd, int code, const std::string& contentType,
                         const std::vector<uint8_t>& body,
                         const std::string& extraHeaders) {
  std::string head = "HTTP/1.1 " + std::to_string(code) + " " + StatusText(code) + "\r\n";
  if (!contentType.empty()) head += "Content-Type: " + contentType + "\r\n";
  head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  if (!extraHeaders.empty()) {
    head += extraHeaders;
    if (head.size() < 2 || head.compare(head.size() - 2, 2, "\r\n") != 0) head += "\r\n";
  }
  head += "Connection: close\r\n\r\n";
  auto deadline = Clock::now() + kSendDeadline;
  if (SendAll(fd, head, deadline) && !body.empty()) SendAll(fd, body, deadline);
}

void HttpServer::Respond401(int fd) {
  Respond(fd, 401, "text/plain", {}, "WWW-Authenticate: Basic realm=\"IPCamera\"\r\n");
}

void HttpServer::RespondText(int fd, const std::string& text) {
  Respond(fd, 200, "text/plain", std::vector<uint8_t>(text.begin(), text.end()));
}

bool HttpServer::HandleRequest(int fd, const std::string& method, const std::string& path,
                               const std::vector<uint8_t>& body) {
  JpegSource jpegSource;
  InfoSource infoSource;
  CommandSink commandSink;
  VoiceSink voiceSink;
  ArchiveSource archiveSource;
  ArchiveFile archiveFile;
  OpusStream* opusStream = nullptr;
  {
    std::lock_guard<std::mutex> lock(cbMu_);
    jpegSource = jpegSource_;
    infoSource = infoSource_;
    commandSink = commandSink_;
    voiceSink = voiceSink_;
    archiveSource = archiveSource_;
    archiveFile = archiveFile_;
    opusStream = opusStream_;
  }

  if (path == "/" || path == "/index" || path == "/main.html") {
    std::string html(kDashboardHtml);
    Respond(fd, 200, "text/html", std::vector<uint8_t>(html.begin(), html.end()));
    return true;
  }
  if (path == "/snapshot.jpg" || path == "/getsnapshot") {
    std::vector<uint8_t> jpg = jpegSource ? jpegSource() : std::vector<uint8_t>();
    if (jpg.empty()) {
      RespondText(fd, "no frame yet");
    } else {
      Respond(fd, 200, "image/jpeg", jpg);
    }
    return true;
  }
  if (path == "/video") {
    std::string head =
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace;boundary=" +
        std::string(kMjpegBoundary) + "\r\nConnection: close\r\n\r\n";
    if (!SendAll(fd, head)) return false;
    auto lastSend = Clock::now();
    while (running_.load()) {
      auto now = Clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSend);
      if (elapsed < std::chrono::milliseconds(100)) {
        pollfd p{fd, POLLIN, 0};
        int wait = static_cast<int>((std::chrono::milliseconds(100) - elapsed).count());
        int result = ::poll(&p, 1, std::min(wait, 50));
        if (result > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
        continue;
      }
      lastSend = now;
      std::vector<uint8_t> jpg = jpegSource ? jpegSource() : std::vector<uint8_t>();
      if (jpg.empty()) continue;
      std::string part = "--" + std::string(kMjpegBoundary) +
                         "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                         std::to_string(jpg.size()) + "\r\n\r\n";
      auto deadline = Clock::now() + kSendDeadline;
      if (!SendAll(fd, part, deadline) || !SendAll(fd, jpg, deadline) ||
          !SendAll(fd, "\r\n", deadline)) {
        return false;
      }
    }
    return false;
  }
  if (path == "/audio.opus") {
    if (opusStream == nullptr || !opusStream->SourceActive()) {
      // mirror of the Android behavior: no audio source -> service unavailable
      Respond(fd, 503, "text/plain", std::vector<uint8_t>({'n', 'o', ' ', 'a', 'u', 'd', 'i', 'o'}));
      return true;
    }
    if (!opusStream->EnsureReady()) {
      Respond(fd, 501, "text/plain",
              std::vector<uint8_t>({'o', 'p', 'u', 's', ' ', 'u', 'n', 'a', 'v', 'a', 'i', 'l'}));
      return true;
    }
    std::string head =
        "HTTP/1.1 200 OK\r\nContent-Type: audio/ogg\r\nConnection: close\r\n\r\n";
    if (!SendAll(fd, head)) return false;
    // Blocks this worker until the client disconnects (like /video).
    return opusStream->Serve(fd, running_);
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
      Respond(fd, 200, "video/mp4", data);
    } else {
      Respond(fd, 404, "text/plain", {});
    }
    return true;
  }
  if (path == "/put_voice") {
    VoiceResult result = voiceSink ? voiceSink(body) : VoiceResult::Unavailable;
    switch (result) {
      case VoiceResult::Accepted:
        RespondText(fd, "voice received");
        break;
      case VoiceResult::Invalid:
        Respond(fd, 400, "text/plain", {});
        break;
      case VoiceResult::Busy:
      case VoiceResult::Unavailable:
        Respond(fd, 503, "text/plain", {});
        break;
    }
    return true;
  }
  (void)method;
  Respond(fd, 404, "text/plain", {});
  return true;
}

void HttpServer::HandleConnection(int fd) {
  std::string header;
  auto headerDeadline = Clock::now() + kHeaderDeadline;
  bool headerTooLarge = false;
  if (!ReadHeaderOnly(fd, &header, headerDeadline, &headerTooLarge)) {
    if (headerTooLarge) {
      Respond(fd, 431, "text/plain", {});
    } else if (Clock::now() >= headerDeadline) {
      Respond(fd, 408, "text/plain", {});
    }
    return;
  }
  if (header.size() < 4 || header.size() > kMaxHeaderBytes) {
    Respond(fd, 431, "text/plain", {});
    return;
  }
  size_t headerEnd = header.size() - 4;

  ParsedRequest request;
  if (!ParseRequestHead(header.substr(0, headerEnd), &request)) {
    Respond(fd, 400, "text/plain", {});
    return;
  }

  std::string route = request.path;
  size_t query = route.find('?');
  std::string queryString = query == std::string::npos ? "" : route.substr(query + 1);
  if (query != std::string::npos) route.erase(query);
  route = ToLower(route);
  const bool isVoice = route == "/put_voice";

  // Token auth for media routes: /video?pw=<password> and
  // /snapshot.jpg?pw=<password> bypass Basic auth (embedded <img>/<audio>
  // tags cannot send headers). Equivalent access level, narrower surface.
  const bool isMediaRoute = route == "/video" || route == "/snapshot.jpg" ||
                            route == "/getsnapshot" || route == "/audio.opus";
  bool tokenOk = false;
  if (isMediaRoute) {
    size_t pwPos = queryString.find("pw=");
    if (pwPos != std::string::npos) {
      std::string pw = queryString.substr(pwPos + 3);
      size_t amp = pw.find('&');
      if (amp != std::string::npos) pw = pw.substr(0, amp);
      tokenOk = pw == password_;
    }
  }

  auto auth = request.headers.find("Authorization");
  if (!tokenOk &&
      (auth == request.headers.end() || auth->second.size() != 1 ||
       !CheckAuth(auth->second.front()))) {
    Respond401(fd);
    return;
  }

  auto transferEncoding = request.headers.find("Transfer-Encoding");
  if (transferEncoding != request.headers.end()) {
    Respond(fd, 400, "text/plain", {});
    return;
  }

  auto contentLengthHeader = request.headers.find("Content-Length");
  if (!isVoice) {
    if (contentLengthHeader != request.headers.end()) {
      size_t ignored = 0;
      if (!ParseContentLength(request.headers, &ignored) || ignored != 0) {
        Respond(fd, 400, "text/plain", {});
        return;
      }
    }
    HandleRequest(fd, request.method, route, {});
    return;
  }

  if (request.method != "PUT" && request.method != "POST") {
    Respond(fd, 405, "text/plain", {}, "Allow: PUT, POST\r\n");
    return;
  }
  if (contentLengthHeader == request.headers.end()) {
    Respond(fd, 411, "text/plain", {});
    return;
  }
  size_t contentLength = 0;
  if (!ParseContentLength(request.headers, &contentLength)) {
    Respond(fd, 400, "text/plain", {});
    return;
  }
  if (contentLength > kMaxVoiceBodyBytes) {
    Respond(fd, 413, "text/plain", {});
    return;
  }

  std::vector<uint8_t> body(contentLength);
  auto bodyDeadline = Clock::now() + kBodyDeadline;
  if (!body.empty() && !ReceiveExact(fd, body.data(), body.size(), bodyDeadline)) {
    if (Clock::now() >= bodyDeadline) Respond(fd, 408, "text/plain", {});
    return;
  }
  HandleRequest(fd, request.method, route, body);
}

}  // namespace ipcam
