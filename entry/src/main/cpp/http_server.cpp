#include "http_server.h"
#include "tcp_listeners.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

#include <hilog/log.h>

#include "flv_stream.h"
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
    "<title>IPCamera</title></head><body>"
    "<h2>IPCamera Live</h2>"
    "<img src='/video'><p>"
    "<audio src='/audio.opus' controls preload='none'></audio><p>"
    "<a href='/live.flv'>HTTP-FLV stream</a> (H.264, optional AAC; copy link into VLC's Open Network Stream)"
    "</p><p><a href='/snapshot.jpg'>snapshot.jpg</a> | "
    "<a href='/serverinfo'>serverinfo</a> | "
    "<a href='/size'>size</a> | "
    "<a href='/recordings'>Recordings (play / download)</a> | "
    "<a href='/getarchives'>archives</a></p>"
    "<p><a href='/light'>/light (torch)</a> | "
    "<a href='/camswitch'>/camswitch</a></p>"
    R"HTML(<fieldset id="sizes"><legend>Video resolution</legend>
<button type="button" id="refresh">Refresh sizes</button> <span id="choices"></span>
<span id="feedback" role="status"></span></fieldset>
<script>
const sizes = document.getElementById('sizes'), choices = document.getElementById('choices'),
      feedback = document.getElementById('feedback');
async function request(path) {
  const response = await fetch(path, {cache: 'no-store'});
  if (!response.ok) throw new Error((await response.text()) || ('HTTP ' + response.status));
  return response;
}
async function showSizes(selectIndex) {
  sizes.disabled = true;
  feedback.textContent = selectIndex === undefined ? 'Loading sizes…' : 'Switching…';
  choices.replaceChildren();
  try {
    if (selectIndex !== undefined) await request('/size' + selectIndex);
    const data = await (await request('/size?format=json')).json();
    data.sizes.forEach((size, index) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.disabled = index === data.currentIndex;
      button.textContent = size.width + 'x' + size.height + (button.disabled ? ' (current)' : '');
      button.addEventListener('click', () => showSizes(index));
      choices.appendChild(button);
    });
    feedback.textContent = data.sizes.length ? '' : 'No sizes available.';
  } catch (error) {
    feedback.textContent = error.message;
  } finally {
    sizes.disabled = false;
  }
}
document.getElementById('refresh').addEventListener('click', () => showSizes());
showSizes();
</script></body></html>)HTML";

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
             Clock::time_point deadline = Clock::now() + kSendDeadline, bool renewOnProgress = true) {
  size_t offset = 0;
  while (offset < len) {
    if (!WaitForEvent(fd, POLLOUT, deadline)) return false;
    ssize_t sent = ::send(fd, data + offset, len - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) {
      offset += static_cast<size_t>(sent);
      // The deadline is a stall timeout, not a budget for the whole body:
      // renewed on progress so a multi-hundred-MB recording download cannot be
      // cut off mid-transfer, while a client that stops reading still aborts.
      if (renewOnProgress) deadline = Clock::now() + kSendDeadline;
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

// ArchiveSource returns "<name> <bytes>\n"; only the recorder's two namespaces
// are addressable. Never treat a name from the listing or the URL as HTML.
const char* kRecordingPage =
    "<!doctype html><html lang='en'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>IPCamera recordings</title><style>body{font:1rem system-ui;margin:2rem auto;"
    "max-width:60rem;padding:0 1rem}li{margin:.75rem 0;overflow-wrap:anywhere}"
    "video{display:block;width:100%;max-height:75vh;background:#111}</style><body>";

bool ValidRecordingName(const std::string& name) {
  size_t base = name.rfind("MD/IPS_", 0) == 0 ? 3 : 0;
  if (!base && name.rfind("ipc_", 0) != 0) return false;
  return name.size() >= base + 8 && name.compare(name.size() - 4, 4, ".mp4") == 0 &&
         name.find('/', base) == std::string::npos && name.find('\\') == std::string::npos &&
         name.find("..") == std::string::npos &&
         std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

std::string HtmlText(const std::string& value) {
  std::string text;
  for (char c : value) {
    switch (c) {
      case '&': text += "&amp;"; break;
      case '<': text += "&lt;"; break;
      case '>': text += "&gt;"; break;
      case '\"': text += "&quot;"; break;
      case '\'': text += "&#39;"; break;
      default: text += c;
    }
  }
  return text;
}

std::string RecordingUrlPath(const std::string& name) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string path;
  for (unsigned char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      path += static_cast<char>(c);
    } else {
      path += '%'; path += hex[c >> 4]; path += hex[c & 15];
    }
  }
  return path;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool DecodeRecordingPath(const std::string& path, std::string& name) {
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] != '%') { name += path[i]; continue; }
    if (i + 2 >= path.size()) return false;
    int hi = HexDigit(path[i + 1]), lo = HexDigit(path[i + 2]);
    if (hi < 0 || lo < 0 || (hi * 16 + lo) == '/') return false;
    name += static_cast<char>(hi * 16 + lo);
    i += 2;
  }
  return ValidRecordingName(name);
}

std::string RecordingList(const std::string& listing) {
  std::string html = std::string(kRecordingPage) + "<a href='/'>Live dashboard</a><h1>Recordings</h1>";
  std::istringstream lines(listing);
  bool any = false;
  for (std::string line; std::getline(lines, line);) {
    size_t sep = line.rfind(' ');
    if (sep == std::string::npos || sep + 1 == line.size() ||
        line.find_first_not_of("0123456789", sep + 1) != std::string::npos) continue;
    std::string name = line.substr(0, sep);
    if (!ValidRecordingName(name)) continue;
    if (!any) html += "<ul>";
    any = true;
    std::string url = RecordingUrlPath(name);
    html += "<li>" + HtmlText(name) + " (" + line.substr(sep + 1) + " bytes) &mdash; "
            "<a href='/recordings/play/" + url + "'>Play</a> | "
            "<a href='/recordings/download/" + url + "'>Download</a></li>";
  }
  html += any ? "</ul>" : "<p>No recordings yet.</p>";
  return html + "</body></html>";
}

// Saturate rather than wrap: an enormous end/suffix covers the rest of the file,
// whereas an enormous start cannot be satisfied.
bool RangeNumber(const std::string& text, uint64_t& value) {
  if (text.empty()) return false;
  value = 0;
  constexpr auto max = std::numeric_limits<uint64_t>::max();
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    unsigned digit = c - '0';
    value = value > (max - digit) / 10 ? max : value * 10 + digit;
  }
  return true;
}

bool RecordingRange(const std::string& range, uint64_t size, uint64_t& first, uint64_t& last) {
  if (!size || ToLower(range.substr(0, 6)) != "bytes=") return false;
  size_t dash = range.find('-', 6);
  if (dash == std::string::npos) return false;
  last = size - 1;
  if (dash == 6) {
    uint64_t suffix;
    if (!RangeNumber(range.substr(dash + 1), suffix) || !suffix) return false;
    first = size - std::min(size, suffix);
  } else {
    if (!RangeNumber(range.substr(6, dash - 6), first) || first >= size) return false;
    if (dash + 1 != range.size()) {
      uint64_t end;
      if (!RangeNumber(range.substr(dash + 1), end) || end < first) return false;
      last = std::min(last, end);
    }
  }
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

bool WriteSnapshot(int fd, const std::vector<uint8_t>& jpg) {
  size_t pos = 0;
  while (pos < jpg.size()) {
    ssize_t n = ::write(fd, jpg.data() + pos, jpg.size() - pos);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    pos += static_cast<size_t>(n);
  }
  return ::fsync(fd) == 0;
}

// Only the trusted Start(..., filesDir) argument selects the destination.
// Stage privately and publish without replacement, even across concurrent GETs.
std::string SaveSnapshot(const std::string& dir, const std::vector<uint8_t>& jpg) {
  timespec now{};
  tm local{};
  char date[32], millis[8];
  if (dir.empty() || dir.front() != '/' || ::clock_gettime(CLOCK_REALTIME, &now) != 0 ||
      !::localtime_r(&now.tv_sec, &local) ||
      !std::strftime(date, sizeof(date), "%Y-%m-%d.%H.%M.%S", &local)) return {};
  std::snprintf(millis, sizeof(millis), ".%04d", static_cast<int>(now.tv_nsec / 1000000));
  std::string stem = std::string("IPC_") + date + millis;
  int root = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root < 0) return {};
  std::string part, saved;
  int fd = -1;
  for (int i = 0; i < 128; ++i) {
    part = "." + stem + "_" + std::to_string(i) + ".part";
    fd = ::openat(root, part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd >= 0) {
    bool ok = WriteSnapshot(fd, jpg);
    if (::close(fd) != 0) ok = false;
    for (int i = 0; ok && i < 128; ++i) {
      std::string name = stem + (i ? "_" + std::to_string(i) : "") + ".jpg";
      if (::linkat(root, part.c_str(), root, name.c_str(), 0) == 0) {
        saved = name;
        break;
      }
      if (errno != EEXIST) break;
    }
    ::unlinkat(root, part.c_str(), 0);
  }
  ::close(root);
  return saved;
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
    case 409:
      return "Conflict";
    case 411:
      return "Length Required";
    case 413:
      return "Payload Too Large";
    case 416:
      return "Range Not Satisfiable";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
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

void HttpServer::SetFreshJpegSource(FreshJpegSource source) {
  std::lock_guard<std::mutex> lock(cbMu_);
  freshJpegSource_ = std::move(source);
}

void HttpServer::SetJpegSource(JpegSource source) {
  std::lock_guard<std::mutex> lock(cbMu_);
  jpegSource_ = std::move(source);
}

void HttpServer::SetInfoSource(InfoSource source) {
  std::lock_guard<std::mutex> lock(cbMu_);
  infoSource_ = std::move(source);
}

void HttpServer::SetSizeCallbacks(SizeSource list, SizeSink select) {
  std::lock_guard<std::mutex> lock(cbMu_);
  sizeSource_ = std::move(list);
  sizeSink_ = std::move(select);
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

void HttpServer::SetFlvStream(std::shared_ptr<FlvStream> stream) {
  std::lock_guard<std::mutex> lock(cbMu_);
  flvStream_ = std::move(stream);
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
                       const std::string& archiveDir, bool allowLegacyVideoPassword) {
  BeginLifecycleOperation();
  if (running_.load()) {
    EndLifecycleOperation();
    return false;
  }
  StopInternal();

  user_ = user;
  password_ = password;
  allowLegacyVideoPassword_ = allowLegacyVideoPassword && !password.empty();
  if (allowLegacyVideoPassword_) {
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                 "Legacy video password enabled: URL history and proxy logs can expose the account password");
  }
  archiveDir_ = archiveDir;

  int fds[2];
  if (OpenTcpListeners(port, 8, fds) == 0) {
    EndLifecycleOperation();
    return false;
  }

  boundV4Port_ = PublicV4ListenerPort(fds);
  listenFd_.store(fds[0]);
  secondListenFd_.store(fds[1]);
  running_.store(true);
  try {
    acceptThread_ = std::thread(&HttpServer::AcceptLoop, this);
  } catch (...) {
    StopInternal();
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

  const int listeners[] = {listenFd_.exchange(-1), secondListenFd_.exchange(-1)};
  for (int fd : listeners) {
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
  }
  if (acceptThread_.joinable()) acceptThread_.join();
  // The poller can no longer observe a closed (and potentially reused) fd.
  for (int fd : listeners) {
    if (fd >= 0) ::close(fd);
  }

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
  pollfd listeners[] = {{listenFd_.load(), POLLIN, 0}, {secondListenFd_.load(), POLLIN, 0}};
  while (running_.load() && (listeners[0].fd >= 0 || listeners[1].fd >= 0)) {
    ReapFinishedWorkers();
    // Alternate priority when both families have connections queued.
    std::swap(listeners[0], listeners[1]);
    int result = ::poll(listeners, 2, 250);
    if (result < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (result == 0) continue;
    if (!running_.load()) break;
    int client = -1;
    for (auto& listener : listeners) {
      if (listener.revents & POLLIN) {
        client = ::accept(listener.fd, nullptr, nullptr);
        if (client >= 0) break;
      }
      if (listener.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        boundV4Port_ = 0;  // Conservative: any listener error revokes eligibility for this generation.
        listener.fd = -1;
      }
    }
    if (client < 0) continue;
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
  boundV4Port_ = 0;
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

bool HttpServer::RespondFile(int fd, int fileFd) {
  struct FileCloser {
    int fd;
    ~FileCloser() { ::close(fd); }
  } file{fileFd};
  struct stat st{};
  if (::fstat(fileFd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    Respond(fd, 404, "text/plain", {});
    return false;
  }
  uint64_t remaining = static_cast<uint64_t>(st.st_size);
  std::string head = "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: " +
                     std::to_string(remaining) + "\r\nConnection: close\r\n\r\n";
  if (!SendAll(fd, head)) return false;
  uint8_t buffer[64 * 1024];
  while (remaining > 0 && running_.load()) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(buffer)));
    ssize_t readBytes = ::read(fileFd, buffer, count);
    if (readBytes < 0 && errno == EINTR) continue;
    if (readBytes <= 0) return false;
    if (!SendAll(fd, buffer, static_cast<size_t>(readBytes))) return false;
    remaining -= static_cast<uint64_t>(readBytes);
  }
  return remaining == 0;
}

bool HttpServer::RespondRecording(int fd, int fileFd, const std::vector<std::string>* ranges,
                                  bool headOnly, const std::string& disposition) {
  struct FileCloser {
    int fd;
    ~FileCloser() { ::close(fd); }
  } file{fileFd};
  struct stat st{};
  if (::fstat(fileFd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    Respond(fd, 404, "text/plain", {});
    return false;
  }

  const uint64_t size = static_cast<uint64_t>(st.st_size);
  uint64_t offset = 0, count = size;
  std::string head = "HTTP/1.1 200 OK\r\n";
  // Only byte ranges are supported; ignore unrecognized range units and Range on HEAD.
  if (ranges && !headOnly &&
      (ranges->size() != 1 || ToLower(ranges->front().substr(0, 6)) == "bytes=")) {
    uint64_t last;
    if (ranges->size() != 1 || !RecordingRange(ranges->front(), size, offset, last)) {
      Respond(fd, 416, "text/plain", {}, "Content-Range: bytes */" + std::to_string(size) + "\r\n");
      return false;
    }
    count = last - offset + 1;
    head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(offset) +
           "-" + std::to_string(last) + "/" + std::to_string(size) + "\r\n";
  }
  head += "Content-Type: video/mp4\r\nAccept-Ranges: bytes\r\nX-Content-Type-Options: nosniff\r\n" +
          disposition + "Content-Length: " + std::to_string(count) + "\r\nConnection: close\r\n\r\n";
  if (!SendAll(fd, head)) return false;
  if (headOnly) return true;
  uint8_t buffer[64 * 1024];
  while (count && running_.load()) {
    ssize_t n = ::pread(fileFd, buffer, std::min<uint64_t>(count, sizeof(buffer)), static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || !SendAll(fd, buffer, static_cast<size_t>(n))) return false;
    offset += static_cast<uint64_t>(n);
    count -= static_cast<uint64_t>(n);
  }
  return count == 0;
}

void HttpServer::Respond401(int fd) {
  Respond(fd, 401, "text/plain", {}, "WWW-Authenticate: Basic realm=\"IPCamera\"\r\n");
}

void HttpServer::RespondText(int fd, const std::string& text) {
  Respond(fd, 200, "text/plain", std::vector<uint8_t>(text.begin(), text.end()));
}

void HttpServer::HandleRecordings(int fd, const std::string& method, const std::string& subpath,
                                  const std::vector<std::string>* ranges) {
  // The action is a route, but the remaining filename is case-sensitive.
  const size_t slash = subpath.find('/', 1);
  const std::string action = ToLower(subpath.substr(0, slash));
  const bool list = subpath.empty() || subpath == "/";
  const bool play = action == "/play";
  const bool download = action == "/download";
  const bool file = download || action == "/media";
  if (!list && ((!file && !play) || slash == std::string::npos)) {
    Respond(fd, 404, "text/plain", {});
    return;
  }
  if (method != "GET" && !(file && method == "HEAD")) {
    Respond(fd, 405, "text/plain", {}, file ? "Allow: GET, HEAD" : "Allow: GET");
    return;
  }

  std::string html;
  if (list) {
    ArchiveSource source;
    {
      std::lock_guard<std::mutex> lock(cbMu_);
      source = archiveSource_;
    }
    html = RecordingList(source ? source() : "");
  } else {
    std::string name;
    if (!DecodeRecordingPath(subpath.substr(slash + 1), name)) {
      Respond(fd, 404, "text/plain", {});
      return;
    }
    std::string url = RecordingUrlPath(name);
    if (file) {
      ArchiveFile openFile;
      {
        std::lock_guard<std::mutex> lock(cbMu_);
        openFile = archiveFile_;
      }
      // The existing resolver restores the recorder's uppercase MD/IPS_ prefix.
      std::string key = name.rfind("MD/IPS_", 0) == 0 ? "md/ips_" + name.substr(7) : name;
      std::string disposition;
      if (download) {
        disposition = "Content-Disposition: attachment; filename*=UTF-8''" +
                      url.substr(url.rfind('/') + 1) + "\r\n";
      }
      int fileFd = openFile ? openFile(key) : -1;
      if (fileFd < 0) {
        Respond(fd, 404, "text/plain", {});
      } else {
        RespondRecording(fd, fileFd, ranges, method == "HEAD", disposition);
      }
      return;
    }
    html = std::string(kRecordingPage) + "<a href='/recordings'>All recordings</a><h1>" +
           HtmlText(name) + "</h1><video controls playsinline preload='metadata' src='/recordings/media/" +
           url + "'></video><p><a href='/recordings/download/" + url +
           "'>Download MP4</a> if your browser cannot play this recording.</p></body></html>";
  }
  Respond(fd, 200, "text/html; charset=utf-8", std::vector<uint8_t>(html.begin(), html.end()),
          "Cache-Control: no-store\r\nContent-Security-Policy: default-src 'none'; "
          "style-src 'unsafe-inline'; media-src 'self'; base-uri 'none'\r\n");
}

void HttpServer::HandleSize(int fd, const std::string& method, const std::string& path, bool json) {
  if (method != "GET") {
    Respond(fd, 405, "text/plain", {}, "Allow: GET\r\nCache-Control: no-store\r\n");
    return;
  }
  auto reply = [&](int code, const char* type, const std::string& text) {
    Respond(fd, code, type, {text.begin(), text.end()}, "Cache-Control: no-store\r\n");
  };
  SizeResult result;
  SizeList list;
  const bool listing = path == "/size";
  if (listing) {
    SizeSource source;
    { std::lock_guard<std::mutex> lock(cbMu_); source = sizeSource_; }
    result = source ? source(list) : SizeResult::CameraClosed;
  } else {
    uint64_t index = 0;
    if (!RangeNumber(path.substr(5), index) || index > std::numeric_limits<int>::max() ||
        (path.size() > 6 && path[5] == '0')) {
      reply(400, "text/plain", "Invalid size index");
      return;
    }
    SizeSink select;
    { std::lock_guard<std::mutex> lock(cbMu_); select = sizeSink_; }
    result = mjpegViewers_.load() > 1 ? SizeResult::Busy :
             select ? select(static_cast<int>(index)) : SizeResult::CameraClosed;
  }
  switch (result) {
    case SizeResult::NotFound: reply(404, "text/plain", "Size not available"); return;
    case SizeResult::CameraClosed: reply(503, "text/plain", "Camera not open"); return;
    case SizeResult::Busy: reply(409, "text/plain", "Camera busy"); return;
    case SizeResult::RestartFailed: reply(500, "text/plain", "Camera restart failed"); return;
    case SizeResult::Ok: break;
  }
  if (!listing) {
    reply(200, "text/plain", "");  // Legacy clients refresh only on an empty success body.
    return;
  }
  std::string text = json ? "{\"currentIndex\":" + std::to_string(list.currentIndex) + ",\"sizes\":[" : "";
  for (size_t i = 0; i < list.sizes.size(); ++i) {
    const auto width = std::to_string(list.sizes[i].width), height = std::to_string(list.sizes[i].height);
    if (json) {
      if (i) text += ',';
      text += "{\"width\":" + width + ",\"height\":" + height + "}";
    } else {
      // Android's picker parses a[href] in order. Its web console calls onItemClick('sizeN').
      // Only decimal integers are interpolated, never provider-controlled HTML/JS strings.
      const std::string label = width + "x" + height;
      if (list.currentIndex >= 0 && i == static_cast<size_t>(list.currentIndex)) {
        text += "<li><a href=\"#\" data-sel=\"yes\" onclick=\"return false;\"><strong>" + label + "</strong></a></li>";
      } else {
        text += "<li><a href=\"#\" onclick=\"return onItemClick('size" + std::to_string(i) + "')\">" + label + "</a></li>";
      }
    }
  }
  if (json) text += "]}";
  reply(200, json ? "application/json" : "text/plain", text);
}

void HttpServer::HandleFlv(int fd, const std::string& method) {
  if (method != "GET") {
    Respond(fd, 405, "text/plain", {}, "Allow: GET\r\n");
    return;
  }
  std::shared_ptr<FlvStream> stream;
  {
    std::lock_guard<std::mutex> lock(cbMu_);
    stream = flvStream_;
  }
  std::shared_ptr<FlvStream::Reader> reader;
  auto status = stream ? stream->Open(reader) : FlvStream::OpenResult::NotReady;
  auto unavailable = [&](int code, const std::string& reason) {
    Respond(fd, code, "text/plain", {reason.begin(), reason.end()}, "Cache-Control: no-store\r\n");
  };
  if (status == FlvStream::OpenResult::Unsupported) {
    unavailable(501, "HTTP-FLV requires H.264; switch the camera from HEVC to AVC.");
    return;
  }
  if (status != FlvStream::OpenResult::Ok) {
    unavailable(503, status == FlvStream::OpenResult::Busy ? "FLV busy; retry later." : "No AVC source ready.");
    return;
  }
  // Never commit a successful HTTP response until a decodable IDR is ready.
  bool started = false, hasAudio = false;
  auto idleDeadline = Clock::now() + std::chrono::seconds(10);
  while (running_.load()) {
    FlvStream::Tag tag{};
    auto result = stream->Read(reader, tag, hasAudio);
    if (result != FlvStream::ReadResult::TagReady) {
      uint8_t b;
      const ssize_t n = ::recv(fd, &b, 1, MSG_DONTWAIT | MSG_PEEK);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return;
      if (result == FlvStream::ReadResult::Closed || Clock::now() >= idleDeadline) {
        if (!started && running_.load()) unavailable(503, "No AVC keyframe available; retry later.");
        return;
      }
      continue;
    }
    if (!started) {
      if (!SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n") ||
          !SendAll(fd, FlvStream::Header(hasAudio))) return;
      started = true;
    }
    auto bytes = tag.Serialize();
    if (!SendAll(fd, bytes.data(), bytes.size(), Clock::now() + kSendDeadline, false)) return;
    idleDeadline = Clock::now() + std::chrono::seconds(10);
  }
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
  if (path == "/getsnapshot") {
    if (method != "GET") {
      Respond(fd, 405, "text/plain", {}, "Allow: GET\r\n");
      return true;
    }
    FreshJpegSource capture;
    {
      std::lock_guard<std::mutex> lock(cbMu_);
      capture = freshJpegSource_;
    }
    std::vector<uint8_t> jpg;
    try {
      if (capture && running_) jpg = capture(running_);
    } catch (...) {
      // Source failure is unavailability, never a reason to save the cache.
    }
    // The in-process encoder supplies complete files, not arbitrary uploads.
    if (!running_ || jpg.size() <= 4 || jpg[0] != 0xff || jpg[1] != 0xd8 ||
        jpg[jpg.size() - 2] != 0xff || jpg.back() != 0xd9) {
      Respond(fd, 503, "text/plain", {}, "Cache-Control: no-store\r\n");
      return true;
    }
    auto name = SaveSnapshot(archiveDir_, jpg);
    if (name.empty()) {
      Respond(fd, 500, "text/plain", {}, "Cache-Control: no-store\r\n");
    } else {
      std::string message = "Snapshot " + name;
      Respond(fd, 200, "text/plain", {message.begin(), message.end()}, "Cache-Control: no-store\r\n");
    }
    return true;
  }
  if (path == "/snapshot.jpg") {
    std::vector<uint8_t> jpg = jpegSource ? jpegSource() : std::vector<uint8_t>();
    if (jpg.empty()) {
      RespondText(fd, "no frame yet");
    } else {
      Respond(fd, 200, "image/jpeg", jpg);
    }
    return true;
  }
  if (path == "/video") {
    struct Viewer {
      std::atomic<unsigned>& count;
      explicit Viewer(std::atomic<unsigned>& n) : count(n) { ++count; }
      ~Viewer() { --count; }
    } viewer(mjpegViewers_);
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
      if (jpg.empty()) {
        // Without frames there is no send() to notice a vanished browser. Do
        // not leave a disconnected viewer counted throughout a camera outage.
        uint8_t byte;
        if (::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0) return false;
        continue;
      }
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
  if (path == "/live.flv") {
    HandleFlv(fd, method);
    return true;
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
  if (path == "/light" || path == "/camswitch") {
    if (commandSink && commandSink(path.substr(1))) {
      RespondText(fd, path == "/light" ? "light toggled" : "camera switched");
    } else {
      Respond(fd, 503, "text/plain", {});
    }
    return true;
  }
  if (path == "/getarchives") {
    RespondText(fd, archiveSource ? archiveSource() : "");
    return true;
  }
  // Manual records (ipc_*) and motion-detection records (MD/IPS_*) — the names
  // are exactly what /getarchives lists. Paths reach the handler lowercased, so
  // the MD prefix is matched in its folded form; the archive resolver restores
  // the casing the recorder wrote.
  if (path.rfind("/get/ipc_", 0) == 0 || path.rfind("/get/md/ips_", 0) == 0) {
    std::string name = path.substr(5);
    int fileFd = archiveFile ? archiveFile(name) : -1;
    if (fileFd >= 0) return RespondFile(fd, fileFd);
    Respond(fd, 404, "text/plain", {});
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

  size_t query = request.path.find('?');
  std::string queryString = query == std::string::npos ? "" : request.path.substr(query + 1);
  if (query != std::string::npos) request.path.erase(query);
  const std::string route = ToLower(request.path);
  const bool isVoice = route == "/put_voice";

  auto auth = request.headers.find("Authorization");
  // A supplied Authorization header must stand on its own. Only the exact
  // legacy MJPEG GET may use a password field instead, never as a second try.
  bool legacyOk = false;
  if (allowLegacyVideoPassword_ && auth == request.headers.end() &&
      request.method == "GET" && request.path == "/video") {
    bool seen = false;
    for (size_t i = 0; i < queryString.size();) {
      size_t end = queryString.find('&', i);
      if (end == std::string::npos) end = queryString.size();
      if (end - i >= 3 && queryString.compare(i, 3, "pw=") == 0) {
        if (seen) { legacyOk = false; break; }
        seen = true;
        legacyOk = queryString.compare(i + 3, end - i - 3, password_) == 0;
      }
      i = end + 1;
    }
  }

  if (!legacyOk &&
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
    // Unlike case-folded legacy routes, use the original path for size controls.
    if (request.path.rfind("/size", 0) == 0) {
      HandleSize(fd, request.method, request.path, queryString == "format=json");
    } else if (route == "/recordings" || route.rfind("/recordings/", 0) == 0) {
      auto range = request.headers.find("Range");
      // Without a validator, If-Range requires returning the whole file.
      bool useRange = range != request.headers.end() && request.headers.count("If-Range") == 0;
      HandleRecordings(fd, request.method, request.path.substr(sizeof("/recordings") - 1),
                       useRange ? &range->second : nullptr);
    } else {
      HandleRequest(fd, request.method, route, {});
    }
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

  // Clients that ask for 100-continue wait for the interim response before
  // sending the body (curl waits ~1s otherwise).
  auto expect = request.headers.find("Expect");
  if (expect != request.headers.end() && !expect->second.empty() &&
      ToLower(expect->second.front()).find("100-continue") !=
          std::string::npos) {
    static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
    SendAll(fd, reinterpret_cast<const uint8_t*>(kContinue), sizeof(kContinue) - 1,
            Clock::now() + kSendDeadline);
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
