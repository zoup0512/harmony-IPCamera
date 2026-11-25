#include "rtsp_server.h"

#include <arpa/inet.h>
#include "tcp_listeners.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "RtspServer"
#include <cstring>
#include <ctime>
#include <map>
#include <new>
#include <random>
#include <sstream>

namespace ipcam {
namespace {

constexpr int kVideoPt = 96;
constexpr int kAudioPt = 97;
constexpr size_t kRtpPayloadMax = 1400;
constexpr size_t kSessionQueueMaxBytes = 2 * 1024 * 1024;
constexpr size_t kControlQueueReserveBytes = 64 * 1024;
constexpr int kTcpSendDeadlineMs = 1500;
constexpr size_t kMaxSessions = 16;  // shared across IPv4 and IPv6, even before SETUP
constexpr size_t kMaxQueueItems = 4096;  // byte limits alone don't bound tiny frame overhead

// Keep the kernel-supplied sockaddr, including sin6_scope_id for link-local
// interfaces. Numeric text is for SDP/logging only (no libc zone-name lookup).
struct Endpoint {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  sockaddr* ptr() { return reinterpret_cast<sockaddr*>(&addr); }
  void SetPort(int port) {
    if (addr.ss_family == AF_INET6) reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port = htons(port);
    else reinterpret_cast<sockaddr_in*>(&addr)->sin_port = htons(port);
  }
  int Port() const {
    if (addr.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
  }
  std::string Ip(bool scope = false) const {
    char text[INET6_ADDRSTRLEN];
    if (addr.ss_family == AF_INET6) {
      const auto& v6 = *reinterpret_cast<const sockaddr_in6*>(&addr);
      if (!inet_ntop(AF_INET6, &v6.sin6_addr, text, sizeof(text))) return {};
      if (scope && v6.sin6_scope_id) return std::string(text) + "%" + std::to_string(v6.sin6_scope_id);
    } else if (addr.ss_family != AF_INET ||
               !inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&addr)->sin_addr, text, sizeof(text))) return {};
    return text;
  }
};

const int kAacSampleRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000,  7350,  0, 0, 0};

// Both media tracks use the common monotonic sampling domain (media_time.h).
// Divide first so large timestamps retain their modulo-2^32 RTP value.
uint32_t RtpTime(TimestampUs us, uint32_t rate) {
  const auto t = static_cast<uint64_t>(us);
  return static_cast<uint32_t>((t / 1000000) * rate + (t % 1000000) * rate / 1000000);
}

struct NalView {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

struct AdtsHeader {
  int profile = 1;
  int sfIndex = 4;
  int channels = 2;
  int sampleRate = 44100;
  int headerLen = 7;
  int frameLength = 0;
};

bool ParseAdts(const uint8_t* d, size_t len, AdtsHeader* out) {
  if (d == nullptr || len < 7) return false;
  if (d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return false;  // syncword + layer==0
  out->profile = (d[2] >> 6) & 0x03;
  out->sfIndex = (d[2] >> 2) & 0x0F;
  out->channels = ((d[2] & 0x01) << 2) | ((d[3] >> 6) & 0x03);
  out->frameLength = ((d[3] & 0x03) << 11) | (d[4] << 3) | ((d[5] >> 5) & 0x07);
  bool protectionAbsent = (d[1] & 0x01) != 0;
  out->headerLen = protectionAbsent ? 7 : 9;
  if (out->sfIndex >= 16) return false;
  out->sampleRate = kAacSampleRates[out->sfIndex];
  if (out->sampleRate == 0 || out->channels == 0) return false;
  if (static_cast<size_t>(out->headerLen) > len ||
      out->frameLength < out->headerLen + 1 || out->frameLength > 8192 ||
      static_cast<size_t>(out->frameLength) > len) {
    return false;
  }
  return true;
}

std::vector<NalView> SplitAnnexB(const uint8_t* data, size_t size) {
  std::vector<NalView> nals;
  size_t i = 0;
  size_t nalStart = std::string::npos;
  while (i + 3 <= size) {
    if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
      if (nalStart != std::string::npos && i > nalStart) {
        nals.push_back({data + nalStart, i - nalStart});
      }
      i += 3;
      nalStart = i;
    } else {
      ++i;
    }
  }
  if (nalStart != std::string::npos && size > nalStart) {
    nals.push_back({data + nalStart, size - nalStart});
  }
  return nals;
}

const char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < len) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += kBase64Chars[(v >> 18) & 0x3F];
    out += kBase64Chars[(v >> 12) & 0x3F];
    out += kBase64Chars[(v >> 6) & 0x3F];
    out += kBase64Chars[v & 0x3F];
    i += 3;
  }
  if (i + 1 == len) {
    uint32_t v = data[i] << 16;
    out += kBase64Chars[(v >> 18) & 0x3F];
    out += kBase64Chars[(v >> 12) & 0x3F];
    out += "==";
  } else if (i + 2 == len) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
    out += kBase64Chars[(v >> 18) & 0x3F];
    out += kBase64Chars[(v >> 12) & 0x3F];
    out += kBase64Chars[(v >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

bool Base64Decode(const std::string& in, std::string* out) {
  static const auto table = [] {
    std::array<int8_t, 256> values;
    values.fill(-1);
    for (int i = 0; i < 64; ++i) values[static_cast<uint8_t>(kBase64Chars[i])] = static_cast<int8_t>(i);
    return values;
  }();
  out->clear();
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
    int8_t v = table[static_cast<uint8_t>(c)];
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

std::string ToHex(const uint8_t* data, size_t len) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[(data[i] >> 4) & 0xF];
    out += hex[data[i] & 0xF];
  }
  return out;
}

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> SplitString(const std::string& s, char delim) {
  std::vector<std::string> parts;
  std::string item;
  std::istringstream iss(s);
  while (std::getline(iss, item, delim)) parts.push_back(item);
  return parts;
}

std::string Trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

struct TransportSpec {
  bool tcp = false;
  int rtp = -1, rtcp = -1;
};

bool ParsePair(const std::string& s, int min, int max, TransportSpec& spec) {
  auto number = [&](size_t& i, int& out) {
    if (i == s.size() || s[i] < '0' || s[i] > '9') return false;
    out = 0;
    do {
      out = out * 10 + (s[i++] - '0');
      if (out > max) return false;
    } while (i < s.size() && s[i] >= '0' && s[i] <= '9');
    return out >= min;
  };
  size_t i = 0;
  if (!number(i, spec.rtp)) return false;
  if (i == s.size()) { spec.rtcp = spec.rtp + 1; return spec.rtcp <= max; }
  return s[i++] == '-' && number(i, spec.rtcp) && i == s.size() && spec.rtp != spec.rtcp;
}

// Only unicast PLAY. Never honor a client-selected destination/source. Reject
// malformed/duplicate pair parameters rather than atoi-truncating or wrapping.
bool ParseTransport(const std::string& header, bool audio, TransportSpec& spec) {
  for (const auto& choice : SplitString(ToLower(header), ',')) {
    const auto fields = SplitString(choice, ';');
    if (fields.empty()) continue;
    const auto proto = Trim(fields[0]);
    TransportSpec t;
    t.tcp = proto == "rtp/avp/tcp";
    if (!t.tcp && proto != "rtp/avp" && proto != "rtp/avp/udp") continue;
    bool ok = true;
    for (size_t i = 1; i < fields.size() && ok; ++i) {
      const auto f = Trim(fields[i]);
      if (f == "unicast" || f == "mode=play" || f == "mode=\"play\"") continue;
      const std::string key = t.tcp ? "interleaved=" : "client_port=";
      ok = t.rtp < 0 && f.rfind(key, 0) == 0 && ParsePair(f.substr(key.size()), t.tcp ? 0 : 1,
                                                           t.tcp ? 255 : 65535, t);
    }
    if (!ok || (!t.tcp && t.rtp < 0)) continue;
    if (t.rtp < 0) { t.rtp = audio ? 2 : 0; t.rtcp = t.rtp + 1; }
    spec = t;
    return true;
  }
  return false;
}

uint32_t RandomU32() {
  static thread_local std::mt19937 rng(static_cast<uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count() ^ getpid()));
  return rng();
}

bool SendAll(int fd, const uint8_t* data, size_t len,
             const std::atomic<bool>& active) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(kTcpSendDeadlineMs);
  size_t off = 0;
  while (off < len && active.load()) {
    ssize_t n = ::send(fd, data + off, len - off,
                       MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) return false;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return false;

    const auto now = Clock::now();
    if (now >= deadline) return false;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    int timeoutMs = static_cast<int>(std::max<int64_t>(1, remaining.count()));
    pollfd p{fd, POLLOUT, 0};
    int pr = ::poll(&p, 1, timeoutMs);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (pr == 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
  }
  return off == len;
}

// Whether an RTSP request URL addresses the configured stream path
// (`rtsp://host:port/<path>[/track...]`); relative control URLs and an
// empty configured path always match.
bool RtspPathMatches(const std::string& url, const std::string& path) {
  if (path.empty()) return true;
  std::string p = url;
  const size_t scheme = p.find("://");
  if (scheme != std::string::npos) {
    const size_t slash = p.find('/', scheme + 3);
    p = slash == std::string::npos ? "/" : p.substr(slash);
  } else if (!p.empty() && p.front() != '/') {
    return true;  // relative control url like "trackID=0" — applies to us
  }
  const size_t q = p.find('?');
  if (q != std::string::npos) p.resize(q);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  std::string want = path;
  while (!want.empty() && want.front() == '/') want.erase(want.begin());
  while (!want.empty() && want.back() == '/') want.pop_back();
  if (want.empty()) return true;
  want = "/" + want;
  return p == want || p.rfind(want + "/", 0) == 0;
}

// SDP advertises relative track controls: resolve them below the stream path,
// not next to the last segment of the DESCRIBE URI (RFC 2326 Appendix C).
std::string StreamBase(const std::string& url) {
  std::string base = url.substr(0, url.find_first_of("?#"));
  if (!base.empty() && base.back() != '/') base += '/';
  return base;
}

std::string TrackUrl(const std::string& setup, const std::string& base) {
  if (setup.empty() || setup.find("://") != std::string::npos) return setup;
  if (setup.front() != '/') return base + setup;
  const auto scheme = base.find("://");
  if (scheme == std::string::npos) return setup;
  return base.substr(0, base.find('/', scheme + 3)) + setup;
}

}  // namespace

struct RtspServer::Session : std::enable_shared_from_this<Session> {
  enum class QueueItemType { kResponse, kVideo, kAudio, kTimer };
  enum class PlaybackChange { kNone, kStart, kStop };

  struct SharedFrame {
    std::vector<uint8_t> bytes;
    TimestampUs tsUs = kNoTimestampUs;
    bool h265 = false;
  };

  struct QueueItem {
    QueueItemType type = QueueItemType::kResponse;
    std::shared_ptr<const SharedFrame> frame;
    std::string response;
    size_t queuedBytes = 0;
    PlaybackChange playbackChange = PlaybackChange::kNone;
    bool playbackWasActive = false;
    bool closeAfter = false;
    std::string videoUrl, audioUrl;  // SETUP targets as of this PLAY
  };

  struct UdpSocket {
    explicit UdpSocket(int socketFd) : fd(socketFd) {}
    ~UdpSocket() {
      if (fd >= 0) ::close(fd);
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    int fd = -1;
  };

  struct Track {
    bool setup = false;
    TransportSpec spec;
    std::shared_ptr<UdpSocket> rtp, rtcp;
  };
  struct Sender {  // writer-owned; SR counts wrap modulo 2^32, per SSRC
    uint32_t packets = 0, octets = 0, rate = 0;
    TimestampUs nextReport = 0;
    bool dirty = false;
    void Sent(size_t payloadBytes, uint32_t clockRate) {
      ++packets;
      octets += static_cast<uint32_t>(payloadBytes);
      rate = clockRate;
      dirty = true;
      if (!nextReport) nextReport = NowMonotonicUs() + 1000000;
    }
  };
  Sender videoSender, audioSender;

  RtspServer* server = nullptr;
  std::atomic<int> fd{-1};
  Endpoint peer, local;
  std::string peerIp;
  std::string id;
  std::atomic<bool> active{true};
  std::atomic<bool> playing{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> finalized{false};
  std::atomic<int> workersRemaining{2};

  uint16_t videoSeq = 0;
  uint32_t videoSsrc = 0;
  uint16_t audioSeq = 0;
  uint32_t audioSsrc = 0;

  mutable std::mutex stateMu;
  Track videoTrack, audioTrack;
  // Reader-owned; the writer receives snapshots on the PLAY queue item.
  std::string contentBase, videoSetupUrl, audioSetupUrl;

  std::mutex queueMu;
  std::condition_variable queueCv;
  std::deque<QueueItem> queue;
  size_t queuedBytes = 0;
  size_t queuedMediaBytes = 0;
  bool closeQueued = false;
  std::atomic<bool> gracefulClosePending{false};

  std::mutex stopMu;
  std::string stopDetail;

  std::thread readerThread;
  std::thread writerThread;

  bool Start();
  void RequestStop(const std::string& detail = "");
  void InterruptSocket();
  void Join();
  bool IsFinished() const { return finished.load(); }
  bool IsActive() const { return active.load(); }
  bool IsPlaying() const { return playing.load(); }
  bool EnqueueVideo(const std::shared_ptr<const SharedFrame>& frame);
  bool EnqueueAudio(const std::shared_ptr<const SharedFrame>& frame);

  void ReaderLoop();
  void WriterLoop();
  void WorkerDone();
  void Finalize(int clientsAfterRemoval);
  void CloseUdp();
  bool HandleRequestText(const std::string& text);
  bool CheckAuth(const std::map<std::string, std::string>& headers);
  bool StreamUrlAllowed(const std::string& url);
  bool SendResponse(const std::string& cseq, int code, const std::string& reason,
                    const std::string& extraHeaders, const std::string& body,
                    PlaybackChange playbackChange = PlaybackChange::kNone,
                    bool closeAfter = false);
  bool EnqueueMedia(QueueItemType type,
                    const std::shared_ptr<const SharedFrame>& frame);
  bool EnqueueControl(QueueItem item);
  bool PopQueueItem(QueueItem* item, TimestampUs waitUs);
  TimestampUs ReportWait();
  bool OpenUdpPair(Track& track, int& serverPort);
  void HandleSlowConsumer();
  void SendVideoFrame(const SharedFrame& frame);
  void SendAudioFrame(const SharedFrame& frame);
  bool DispatchPacket(bool tcp, int chan,
                      const std::shared_ptr<UdpSocket>& udpSocket,
                      const uint8_t* pkt, size_t len, bool report = false);
};

RtspServer::~RtspServer() { Stop(); }

TimestampUs RtspServer::NowUs() { return NowMonotonicUs(); }

bool RtspServer::Start(const RtspConfig& config, RtspStatusCallback callback) {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMu_);
  if (running_.load()) return false;
  if (acceptThread_.joinable()) acceptThread_.join();
  std::vector<std::shared_ptr<Session>> finishedSessions;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if ((*it)->IsFinished()) {
        finishedSessions.push_back(*it);
        it = sessions_.erase(it);
      } else {
        ++it;
      }
    }
    if (!sessions_.empty()) {
      for (const auto& session : finishedSessions) sessions_.push_back(session);
      return false;
    }
  }
  for (const auto& session : finishedSessions) {
    session->Join();
    session->Finalize(0);
  }
  {
    std::lock_guard<std::mutex> lk(configMu_);
    config_ = config;
  }
  {
    std::lock_guard<std::mutex> lk(cbMu_);
    callback_ = std::move(callback);
  }
  CloseListeners();
  listenerCount_ = OpenTcpListeners(config.port, 8, listeners_, config.loopbackOnly);
  if (!listenerCount_) {
    Notify(RtspEvent::kServerError, 0, "bind/listen failed");
    return false;
  }
  boundV4Port_ = PublicV4ListenerPort(listeners_);
  running_ = true;
  try {
    acceptThread_ = std::thread(&RtspServer::AcceptLoop, this);
  } catch (...) {
    running_.store(false);
    CloseListeners();
    Notify(RtspEvent::kServerError, 0, "accept worker start failed");
    return false;
  }
  return true;
}

void RtspServer::Stop() {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMu_);
  running_.store(false);
  for (int i = 0; i < listenerCount_; ++i) ::shutdown(listeners_[i], SHUT_RDWR);
  if (acceptThread_.joinable()) acceptThread_.join();
  CloseListeners();

  std::vector<std::shared_ptr<Session>> snapshot;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    snapshot = sessions_;
  }
  for (const auto& session : snapshot) {
    session->RequestStop("server stop");
    session->InterruptSocket();
  }
  for (const auto& session : snapshot) session->Join();
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    sessions_.clear();
  }
  for (const auto& session : snapshot) session->Finalize(0);
  std::lock_guard<std::mutex> lk(cbMu_);
  callback_ = nullptr;
}

int RtspServer::ClientCount() {
  std::lock_guard<std::mutex> lk(sessionsMu_);
  return static_cast<int>(sessions_.size());
}

void RtspServer::CloseListeners() {
  for (int i = 0; i < listenerCount_; ++i) { ::close(listeners_[i]); listeners_[i] = -1; }
  listenerCount_ = 0;
}

void RtspServer::AcceptLoop() {
  pollfd p[2]{};
  for (int i = 0; i < listenerCount_; ++i) p[i] = {listeners_[i], POLLIN, 0};
  while (running_.load()) {
    ReapFinishedSessions();
    int pr = ::poll(p, listenerCount_, 250);
    if (!running_.load()) break;
    if (pr < 0 && errno == EINTR) continue;
    bool failed = pr < 0;
    for (int i = 0; !failed && i < listenerCount_; ++i) {
      failed = p[i].revents & (POLLERR | POLLHUP | POLLNVAL);
      if (!failed && (p[i].revents & POLLIN)) AcceptOne(p[i].fd);
    }
    if (failed) {
      Notify(RtspEvent::kServerError, ClientCount(), "listener poll failed");
      running_.store(false);
      break;
    }
  }
  ReapFinishedSessions();
}

void RtspServer::AcceptOne(int listenFd) {
  Endpoint peer, local;
  int cfd = ::accept(listenFd, peer.ptr(), &peer.len);
  if (cfd < 0) return;
  if (!running_.load() || ::getsockname(cfd, local.ptr(), &local.len) < 0) { ::close(cfd); return; }
  int one = 1;
  ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  auto s = std::make_shared<Session>();
  s->server = this;
  s->fd = cfd;
  s->peer = peer;
  s->local = local;
  s->peerIp = peer.Ip(true);
  s->videoSsrc = RandomU32() | 1;
  do { s->audioSsrc = RandomU32() | 1; } while (s->audioSsrc == s->videoSsrc);
  char sid[16];
  snprintf(sid, sizeof(sid), "%08x", RandomU32());
  s->id = sid;
  if (!TryAddSession(s)) { ::close(cfd); return; }
  if (s->Start()) {
    Notify(RtspEvent::kClientConnected, ClientCount(), s->peerIp);
  } else {
    ReapFinishedSessions();
    Notify(RtspEvent::kServerError, ClientCount(), "session start failed");
  }
}

bool RtspServer::TryBeginVideoChange() {
  std::lock_guard<std::mutex> lk(sessionsMu_);
  if (videoChangeInProgress_) return false;
  for (const auto& session : sessions_) {
    if (!session->IsFinished()) return false;
  }
  videoChangeInProgress_ = true;
  return true;
}

void RtspServer::EndVideoChange() {
  std::lock_guard<std::mutex> lk(sessionsMu_);
  videoChangeInProgress_ = false;
}

bool RtspServer::TryAddSession(const std::shared_ptr<Session>& session) {
  std::lock_guard<std::mutex> lk(sessionsMu_);
  if (videoChangeInProgress_ || sessions_.size() >= kMaxSessions) return false;
  sessions_.push_back(session);
  return true;
}

void RtspServer::ReapFinishedSessions() {
  std::vector<std::shared_ptr<Session>> finished;
  int clientsAfterRemoval = 0;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if ((*it)->IsFinished()) {
        finished.push_back(*it);
        it = sessions_.erase(it);
      } else {
        ++it;
      }
    }
    clientsAfterRemoval = static_cast<int>(sessions_.size());
  }
  for (const auto& session : finished) {
    session->Join();
    session->Finalize(clientsAfterRemoval);
  }
}

void RtspServer::Notify(RtspEvent event, int clients, const std::string& detail) {
  RtspStatusCallback cb;
  {
    std::lock_guard<std::mutex> lk(cbMu_);
    cb = callback_;
  }
  if (cb) cb(event, clients, detail);
}

bool RtspServer::DetectH265(const uint8_t* data, size_t size) {
  // H.265 NAL header is 2 bytes; VPS has type 32 -> first byte 0x40..0x41.
  // H.264 SPS/IDR have types 7/5 in the low 5 bits of the first byte.
  if (data == nullptr || size < 2) return false;
  uint8_t h264Type = data[0] & 0x1F;
  if (h264Type == 7 || h264Type == 8 || h264Type == 5 || h264Type == 1) return false;
  uint8_t h265Type = (data[0] >> 1) & 0x3F;
  return h265Type == 32 || h265Type == 33 || h265Type == 34 || h265Type == 19 ||
         h265Type == 20 || h265Type == 0 || h265Type == 1;
}

void RtspServer::UpdateParamSetsLocked(const uint8_t* nal, size_t size) {
  if (nal == nullptr || size < 2) return;
  if (codec_ == VideoCodec::H265) {
    uint8_t type = (nal[0] >> 1) & 0x3F;
    if (type == 32) {
      params_.vps.assign(nal, nal + size);
    } else if (type == 33) {
      params_.sps.assign(nal, nal + size);
    } else if (type == 34) {
      params_.pps.assign(nal, nal + size);
    }
  } else {
    uint8_t type = nal[0] & 0x1F;
    if (type == 7) {
      params_.sps.assign(nal, nal + size);
    } else if (type == 8) {
      params_.pps.assign(nal, nal + size);
    }
  }
}

void RtspServer::ResetVideoParams(bool hevc) {
  std::lock_guard<std::mutex> lk(paramMu_);
  codec_ = hevc ? VideoCodec::H265 : VideoCodec::H264;
  codecDetected_ = false;
  params_ = {};
}

RtspServer::VideoParams RtspServer::GetVideoParams() const {
  std::lock_guard<std::mutex> lk(paramMu_);
  VideoParams vp;
  vp.h265 = (codec_ == VideoCodec::H265);
  vp.vps = params_.vps;
  vp.sps = params_.sps;
  vp.pps = params_.pps;
  vp.ready = codecDetected_ && !vp.sps.empty() && !vp.pps.empty() &&
             (!vp.h265 || !vp.vps.empty());
  return vp;
}

void RtspServer::ResetAudioParams() {
  std::lock_guard<std::mutex> lk(paramMu_);
  aac_ = {};
}

void RtspServer::UpdateAacInfo(int profile, int sfIndex, int channels) {
  std::lock_guard<std::mutex> lk(paramMu_);
  aac_.valid = true;
  aac_.profile = profile;
  aac_.sfIndex = sfIndex;
  aac_.channels = channels;
}

RtspServer::AacInfo RtspServer::GetAacInfo() const {
  std::lock_guard<std::mutex> lk(paramMu_);
  return aac_;
}

void RtspServer::PushH264(const uint8_t* data, size_t size, TimestampUs tsUs) {
  if (!running_.load() || data == nullptr || size < 5) return;
  if (tsUs == kNoTimestampUs) tsUs = NowUs();
  if (tsUs < 0) return;
  std::vector<NalView> nals = SplitAnnexB(data, size);
  if (nals.empty()) return;
  bool h265 = false;
  bool codecDetected = false;
  {
    // Parameter-set NALs identify the codec. Encoders lead every access unit
    // with an AUD, so scan all NALs of the frame for unambiguous anchors
    // (H.264 SPS = h264Type 7; H.265 VPS/SPS/PPS = h265Type 32/33/34 with
    // layer/tid byte 0x01). This also picks up a live encoder switch.
    std::lock_guard<std::mutex> lk(paramMu_);
    for (auto& nal : nals) {
      if (nal.size < 2) continue;
      uint8_t h264Type = nal.data[0] & 0x1F;
      uint8_t h265Type = (nal.data[0] >> 1) & 0x3F;
      bool h265Param = (h265Type == 32 || h265Type == 33 || h265Type == 34) &&
                       nal.data[1] == 0x01;
      if (h264Type == 7 && codec_ != VideoCodec::H264) {
        codec_ = VideoCodec::H264;
        codecDetected_ = true;
        params_ = ParamSets{};
        {
          char msg[64];
          snprintf(msg, sizeof(msg), "codec anchor -> H264 (%02x %02x)", nal.data[0],
                   nal.data[1]);
          OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "%{public}s", msg);
        }
      } else if (h265Param && codec_ != VideoCodec::H265) {
        codec_ = VideoCodec::H265;
        codecDetected_ = true;
        params_ = ParamSets{};
        {
          char msg[64];
          snprintf(msg, sizeof(msg), "codec anchor -> H265 (%02x %02x)", nal.data[0],
                   nal.data[1]);
          OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "%{public}s", msg);
        }
      } else if (!codecDetected_ && (h264Type == 7 || h265Param)) {
        codec_ = h264Type == 7 ? VideoCodec::H264 : VideoCodec::H265;
        codecDetected_ = true;
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "codec first detect");
      }
    }
    h265 = codec_ == VideoCodec::H265;
    codecDetected = codecDetected_;
    for (const auto& nal : nals) {
      size_t len = nal.size;
      while (len > 1 && nal.data[len - 1] == 0x00) --len;
      UpdateParamSetsLocked(nal.data, len);
    }
  }
  if (!codecDetected) {
    for (const auto& nal : nals) {
      if (nal.size >= 2 && DetectH265(nal.data, nal.size)) {
        h265 = true;
        break;
      }
    }
  }

  auto frame = std::make_shared<Session::SharedFrame>();
  frame->bytes.assign(data, data + size);
  frame->tsUs = tsUs;
  frame->h265 = h265;
  std::shared_ptr<const Session::SharedFrame> immutableFrame = frame;

  std::vector<std::shared_ptr<Session>> snapshot;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    snapshot = sessions_;
  }
  for (const auto& session : snapshot) session->EnqueueVideo(immutableFrame);
}

void RtspServer::PushAdts(const uint8_t* data, size_t size, TimestampUs tsUs) {
  if (!running_.load() || data == nullptr || size < 8) return;
  if (tsUs == kNoTimestampUs) tsUs = NowUs();
  if (tsUs < 0) return;
  AdtsHeader h;
  if (!ParseAdts(data, size, &h)) return;
  size_t payloadLen = static_cast<size_t>(h.frameLength) - static_cast<size_t>(h.headerLen);
  if (payloadLen == 0 || payloadLen > kRtpPayloadMax) return;
  UpdateAacInfo(h.profile, h.sfIndex, h.channels);

  auto frame = std::make_shared<Session::SharedFrame>();
  frame->bytes.assign(data, data + h.frameLength);
  frame->tsUs = tsUs;
  std::shared_ptr<const Session::SharedFrame> immutableFrame = frame;

  std::vector<std::shared_ptr<Session>> snapshot;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    snapshot = sessions_;
  }
  for (const auto& session : snapshot) session->EnqueueAudio(immutableFrame);
}

std::string RtspServer::BuildSdp(bool ipv6, const std::string& localIp) const {
  const VideoParams spsPps = GetVideoParams();
  const AacInfo aac = GetAacInfo();
  const bool h265 = spsPps.h265;
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "BuildSdp codec=%{public}s vps=%{public}zu sps=%{public}zu pps=%{public}zu",
               h265 ? "H265" : "H264", spsPps.vps.size(), spsPps.sps.size(),
               spsPps.pps.size());
  std::ostringstream sdp;
  const char* family = ipv6 ? "IP6 " : "IP4 ";
  const char* unspecified = ipv6 ? "::" : "0.0.0.0";
  sdp << "v=0\r\n";
  sdp << "o=- " << NowUs() << " 0 IN " << family << localIp << "\r\n";
  sdp << "s=IPCamera Live\r\n";
  sdp << "i=harmony-ipcamera streaming\r\n";
  sdp << "t=0 0\r\n";
  sdp << "a=tool:harmony-ipcamera-poc\r\n";
  sdp << "a=type:broadcast\r\n";
  sdp << "a=control:*\r\n";
  sdp << "m=video 0 RTP/AVP " << kVideoPt << "\r\n";
  sdp << "c=IN " << family << unspecified << "\r\n";
  if (h265) {
    sdp << "a=rtpmap:" << kVideoPt << " H265/90000\r\n";
    if (spsPps.ready) {
      sdp << "a=fmtp:" << kVideoPt
          << " sprop-vps=" << Base64Encode(spsPps.vps.data(), spsPps.vps.size())
          << ";sprop-sps=" << Base64Encode(spsPps.sps.data(), spsPps.sps.size())
          << ";sprop-pps=" << Base64Encode(spsPps.pps.data(), spsPps.pps.size()) << "\r\n";
    }
  } else {
    sdp << "a=rtpmap:" << kVideoPt << " H264/90000\r\n";
    sdp << "a=fmtp:" << kVideoPt << " packetization-mode=1";
    if (spsPps.ready && spsPps.sps.size() >= 4) {
      char plid[8];
      snprintf(plid, sizeof(plid), "%02x%02x%02x", spsPps.sps[1], spsPps.sps[2], spsPps.sps[3]);
      sdp << ";profile-level-id=" << plid;
      sdp << ";sprop-parameter-sets=" << Base64Encode(spsPps.sps.data(), spsPps.sps.size());
      sdp << "," << Base64Encode(spsPps.pps.data(), spsPps.pps.size());
    }
    sdp << "\r\n";
  }
  sdp << "a=control:trackID=0\r\n";
  if (aac.valid) {
    int rate = aac.sfIndex < 16 ? kAacSampleRates[aac.sfIndex] : 0;
    if (rate > 0) {
      uint8_t cfg[2];
      int aot = aac.profile + 1;
      cfg[0] = static_cast<uint8_t>((aot << 3) | (aac.sfIndex >> 1));
      cfg[1] = static_cast<uint8_t>(((aac.sfIndex & 1) << 7) | (aac.channels << 3));
      sdp << "m=audio 0 RTP/AVP " << kAudioPt << "\r\n";
      sdp << "c=IN " << family << unspecified << "\r\n";
      sdp << "a=rtpmap:" << kAudioPt << " MPEG4-generic/" << rate << "/" << aac.channels << "\r\n";
      sdp << "a=fmtp:" << kAudioPt
          << " streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;"
             "indexdeltalength=3;config="
          << ToHex(cfg, sizeof(cfg)) << "\r\n";
      sdp << "a=control:trackID=1\r\n";
    }
  }
  return sdp.str();
}

bool RtspServer::Session::Start() {
  auto self = shared_from_this();
  try {
    writerThread = std::thread([self]() { self->WriterLoop(); });
    readerThread = std::thread([self]() { self->ReaderLoop(); });
    return true;
  } catch (...) {
    RequestStop("worker start failed");
    if (!readerThread.joinable()) WorkerDone();
    if (!writerThread.joinable()) WorkerDone();
    Join();
    return false;
  }
}

void RtspServer::Session::RequestStop(const std::string& detail) {
  {
    std::lock_guard<std::mutex> lk(queueMu);
    active.store(false);
    playing.store(false);
  }
  if (!detail.empty()) {
    std::lock_guard<std::mutex> lk(stopMu);
    if (stopDetail.empty()) stopDetail = detail;
  }
  queueCv.notify_all();
}

void RtspServer::Session::InterruptSocket() {
  int socketFd = fd.load();
  if (socketFd >= 0) ::shutdown(socketFd, SHUT_RDWR);
}

void RtspServer::Session::Join() {
  if (readerThread.joinable() && readerThread.get_id() != std::this_thread::get_id()) {
    readerThread.join();
  }
  if (writerThread.joinable() && writerThread.get_id() != std::this_thread::get_id()) {
    writerThread.join();
  }
}

bool RtspServer::Session::EnqueueVideo(
    const std::shared_ptr<const SharedFrame>& frame) {
  if (!IsActive() || !IsPlaying() || !frame) return true;
  {
    std::lock_guard<std::mutex> lk(stateMu);
    if (!videoTrack.setup) return true;
  }
  return EnqueueMedia(QueueItemType::kVideo, frame);
}

bool RtspServer::Session::EnqueueAudio(
    const std::shared_ptr<const SharedFrame>& frame) {
  if (!IsActive() || !IsPlaying() || !frame) return true;
  {
    std::lock_guard<std::mutex> lk(stateMu);
    if (!audioTrack.setup) return true;
  }
  return EnqueueMedia(QueueItemType::kAudio, frame);
}

bool RtspServer::Session::EnqueueMedia(
    QueueItemType type, const std::shared_ptr<const SharedFrame>& frame) {
  if (!frame) return true;
  bool overflow = false;
  {
    std::lock_guard<std::mutex> lk(queueMu);
    if (!active.load() || !playing.load() || closeQueued) return true;
    const size_t frameBytes = frame->bytes.size();
    const size_t mediaLimit = kSessionQueueMaxBytes - kControlQueueReserveBytes;
    if (queue.size() >= kMaxQueueItems || frameBytes > mediaLimit || queuedMediaBytes > mediaLimit - frameBytes ||
        queuedBytes > kSessionQueueMaxBytes - frameBytes) {
      overflow = true;
    } else {
      QueueItem item;
      item.type = type;
      item.frame = frame;
      item.queuedBytes = frameBytes;
      queue.push_back(std::move(item));
      queuedBytes += frameBytes;
      queuedMediaBytes += frameBytes;
    }
  }
  if (overflow) {
    HandleSlowConsumer();
    return false;
  }
  queueCv.notify_one();
  return true;
}

bool RtspServer::Session::EnqueueControl(QueueItem item) {
  bool overflow = false;
  {
    std::lock_guard<std::mutex> lk(queueMu);
    if (!active.load() || closeQueued) return false;
    item.queuedBytes = item.response.size() + item.videoUrl.size() + item.audioUrl.size();
    if (queue.size() >= kMaxQueueItems || item.queuedBytes > kSessionQueueMaxBytes ||
        queuedBytes > kSessionQueueMaxBytes - item.queuedBytes) {
      overflow = true;
    } else {
      if (item.playbackChange == PlaybackChange::kStart) {
        item.playbackWasActive = playing.load();
        playing.store(true);
      } else if (item.playbackChange == PlaybackChange::kStop) {
        playing.store(false);
        for (auto it = queue.begin(); it != queue.end();) {
          if (it->type == QueueItemType::kVideo || it->type == QueueItemType::kAudio) {
            queuedBytes -= it->queuedBytes;
            queuedMediaBytes -= it->queuedBytes;
            it = queue.erase(it);
          } else {
            ++it;
          }
        }
      }
      if (item.closeAfter) {
        closeQueued = true;
        gracefulClosePending.store(true);
      }
      queuedBytes += item.queuedBytes;
      queue.push_back(std::move(item));
    }
  }
  if (overflow) {
    HandleSlowConsumer();
    return false;
  }
  queueCv.notify_one();
  return true;
}

// Do not queue RTCP behind media or add a per-track timer thread. The sole
// writer checks deadlines between frames/responses and also wakes when idle.
TimestampUs RtspServer::Session::ReportWait() {
  TimestampUs wait = -1;
  if (!active.load() || !playing.load()) return wait;
  auto report = [&](Sender& sender, bool audio) {
    if (!sender.dirty) return;
    auto remaining = sender.nextReport - NowMonotonicUs();
    if (remaining <= 0) {
      Track track;
      {
        std::lock_guard<std::mutex> lk(stateMu);
        track = audio ? audioTrack : videoTrack;
      }
      timespec wall{};
      if (track.setup && clock_gettime(CLOCK_REALTIME, &wall) == 0) {
        const auto samplingTime = NowMonotonicUs();
        const uint32_t ssrc = audio ? audioSsrc : videoSsrc;
        // RFC 3550: SR (no reception blocks), followed by one SDES CNAME
        // chunk shared in name, not SSRC, between audio and video. END + pad.
        uint8_t pkt[64]{};
        auto put32 = [&](size_t off, uint32_t value) {
          value = htonl(value);
          memcpy(pkt + off, &value, sizeof(value));
        };
        pkt[0] = 0x80; pkt[1] = 200; pkt[3] = 6;
        put32(4, ssrc);
        put32(8, static_cast<uint32_t>(wall.tv_sec) + 2208988800U);
        put32(12, static_cast<uint32_t>((static_cast<uint64_t>(wall.tv_nsec) << 32) / 1000000000));
        put32(16, RtpTime(samplingTime, sender.rate));
        put32(20, sender.packets); put32(24, sender.octets);
        pkt[28] = 0x81; pkt[29] = 202;
        put32(32, ssrc);
        const std::string cname = id + "@ipcam";  // 8 hex digits plus suffix
        pkt[36] = 1; pkt[37] = static_cast<uint8_t>(cname.size());
        memcpy(pkt + 38, cname.data(), cname.size());
        const size_t len = (38 + cname.size() + 1 + 3) & ~size_t(3);
        pkt[31] = static_cast<uint8_t>((len - 28) / 4 - 1);
        DispatchPacket(track.spec.tcp, track.spec.rtcp, track.rtcp, pkt, len, true);
      }
      sender.dirty = false;
      sender.nextReport = NowMonotonicUs() + 5000000;
    } else if (wait < 0 || remaining < wait) {
      wait = remaining;
    }
  };
  report(videoSender, false);
  report(audioSender, true);
  return wait;
}

bool RtspServer::Session::PopQueueItem(QueueItem* item, TimestampUs waitUs) {
  std::unique_lock<std::mutex> lk(queueMu);
  auto ready = [this] { return !queue.empty() || !active.load(); };
  if (waitUs < 0) queueCv.wait(lk, ready);
  else if (!queueCv.wait_for(lk, std::chrono::microseconds(waitUs), ready)) {
    item->type = QueueItemType::kTimer;
    return true;
  }
  if (!active.load() || queue.empty()) return false;
  *item = std::move(queue.front());
  queue.pop_front();
  queuedBytes -= item->queuedBytes;
  if (item->type == QueueItemType::kVideo || item->type == QueueItemType::kAudio) {
    queuedMediaBytes -= item->queuedBytes;
  }
  return true;
}

void RtspServer::Session::HandleSlowConsumer() {
  bool shouldNotify;
  {
    std::lock_guard<std::mutex> lk(queueMu);
    shouldNotify = active.exchange(false);
    playing.store(false);
  }
  {
    std::lock_guard<std::mutex> lk(stopMu);
    if (stopDetail.empty()) stopDetail = "slow consumer";
  }
  queueCv.notify_all();
  if (shouldNotify && server != nullptr) {
    server->Notify(RtspEvent::kServerError, server->ClientCount(),
                   "slow consumer: " + peerIp);
  }
}

void RtspServer::Session::ReaderLoop() {
  std::string buf;
  char tmp[4096];
  int socketFd = fd.load();
  while (active.load() && socketFd >= 0) {
    pollfd p{socketFd, POLLIN, 0};
    int pr = ::poll(&p, 1, 500);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    if (!(p.revents & POLLIN)) continue;
    ssize_t n = ::recv(socketFd, tmp, sizeof(tmp), MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (n <= 0) break;
    buf.append(tmp, static_cast<size_t>(n));
    bool fatal = false;
    while (true) {
      if (buf.empty()) break;
      if (buf[0] == '$') {  // interleaved binary (client RTCP) - discard
        if (buf.size() < 4) break;
        size_t blen = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
        if (buf.size() < 4 + blen) break;
        buf.erase(0, 4 + blen);
        continue;
      }
      size_t hdrEnd = buf.find("\r\n\r\n");
      if (hdrEnd == std::string::npos) {
        if (buf.size() > 64 * 1024) fatal = true;
        break;
      }
      if (hdrEnd > 64 * 1024) { fatal = true; break; }
      std::string req = buf.substr(0, hdrEnd);
      buf.erase(0, hdrEnd + 4);
      if (!HandleRequestText(req)) {
        fatal = true;
        break;
      }
      if (gracefulClosePending.load()) break;
    }
    if (fatal || gracefulClosePending.load()) break;
  }
  if (!gracefulClosePending.load()) RequestStop();
  WorkerDone();
}

void RtspServer::Session::WriterLoop() {
  const int socketFd = fd.load();
  QueueItem item;
  while (socketFd >= 0 && PopQueueItem(&item, ReportWait())) {
    if (item.type == QueueItemType::kResponse) {
      if (item.playbackChange == PlaybackChange::kStop) {
        videoSender.dirty = audioSender.dirty = false;
        videoSender.nextReport = audioSender.nextReport = 0;
      }
      if (item.playbackChange == PlaybackChange::kStart) {
        // Only the writer knows the next sequence after all preceding RTP.
        item.response += "RTP-Info: ";
        if (!item.videoUrl.empty()) {
          item.response += "url=" + item.videoUrl + ";seq=" + std::to_string(videoSeq);
        }
        if (!item.audioUrl.empty()) {
          if (!item.videoUrl.empty()) item.response += ',';
          item.response += "url=" + item.audioUrl + ";seq=" + std::to_string(audioSeq);
        }
        item.response += "\r\n\r\n";
      }
      if (!SendAll(socketFd,
                   reinterpret_cast<const uint8_t*>(item.response.data()),
                   item.response.size(), active)) {
        if (item.playbackChange == PlaybackChange::kStart &&
            !item.playbackWasActive) {
          playing.store(false);
        }
        RequestStop("RTSP send failed");
        break;
      }
      if (item.playbackChange == PlaybackChange::kStart &&
          !item.playbackWasActive && server != nullptr) {
        server->Notify(RtspEvent::kClientPlaying, server->ClientCount(), peerIp);
      }
      if (item.closeAfter) {
        RequestStop("teardown");
        InterruptSocket();
        break;
      }
      continue;
    }
    if (item.type == QueueItemType::kVideo && item.frame) {
      SendVideoFrame(*item.frame);
    } else if (item.type == QueueItemType::kAudio && item.frame) {
      SendAudioFrame(*item.frame);
    }
    if (!active.load()) break;
  }
  RequestStop();
  WorkerDone();
}

void RtspServer::Session::WorkerDone() {
  if (workersRemaining.fetch_sub(1) == 1) {
    // Finalize closes fd only after join, so Stop never shuts down a recycled fd.
    finished.store(true);
  }
}

void RtspServer::Session::Finalize(int clientsAfterRemoval) {
  if (finalized.exchange(true)) return;
  active.store(false);
  playing.store(false);
  CloseUdp();
  int socketFd = fd.exchange(-1);
  if (socketFd >= 0) ::close(socketFd);
  finished.store(true);
  if (server != nullptr) {
    std::string detail = peerIp;
    {
      std::lock_guard<std::mutex> lk(stopMu);
      if (!stopDetail.empty()) detail += " (" + stopDetail + ")";
    }
    server->Notify(RtspEvent::kClientDisconnected, clientsAfterRemoval, detail);
  }
}

void RtspServer::Session::CloseUdp() {
  std::lock_guard<std::mutex> lk(stateMu);
  videoTrack = {};
  audioTrack = {};
}

bool RtspServer::Session::OpenUdpPair(Track& track, int& serverPort) {
  auto makeSocket = [&] {
    auto sock = std::make_shared<UdpSocket>(-1);
    sock->fd = ::socket(peer.addr.ss_family, SOCK_DGRAM, 0);
    return sock;
  };
  // Bind both on the TCP connection's actual local interface, without reuse.
  // The kernel chooses one ephemeral port; reserve its even/odd partner before
  // replying. On a collision retry only a bounded number of times.
  for (int i = 0; i < 16; ++i) {
    auto first = makeSocket(), second = makeSocket();
    if (first->fd < 0 || second->fd < 0) return false;
    Endpoint a = local, dst = peer;
    a.SetPort(0);
    if (::bind(first->fd, a.ptr(), a.len) < 0 || ::getsockname(first->fd, a.ptr(), &a.len) < 0) return false;
    const int port = a.Port();
    a.SetPort(port ^ 1);
    if (::bind(second->fd, a.ptr(), a.len) < 0) {
      if (errno == EADDRINUSE) continue;
      return false;
    }
    if (port & 1) std::swap(first, second);
    dst.SetPort(track.spec.rtp);
    if (::connect(first->fd, dst.ptr(), dst.len) < 0) return false;
    dst.SetPort(track.spec.rtcp);
    if (::connect(second->fd, dst.ptr(), dst.len) < 0) return false;
    track.rtp = std::move(first);
    track.rtcp = std::move(second);
    serverPort = port & ~1;
    return true;
  }
  return false;
}

bool RtspServer::Session::HandleRequestText(const std::string& text) {
  std::istringstream iss(text);
  std::string line;
  if (!std::getline(iss, line)) return true;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::istringstream fl(line);
  std::string method, url, version;
  fl >> method >> url >> version;

  std::map<std::string, std::string> headers;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = ToLower(line.substr(0, colon));
    size_t start = line.find_first_not_of(" \t", colon + 1);
    headers[key] = start == std::string::npos ? "" :
        line.substr(start, line.find_last_not_of(" \t") - start + 1);
  }
  std::string cseq = headers.count("cseq") ? headers["cseq"] : "0";

  if (method == "OPTIONS") {
    return SendResponse(cseq, 200, "OK",
                        "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, "
                        "GET_PARAMETER, SET_PARAMETER\r\n",
                        "");
  }
  if (!CheckAuth(headers)) {
    return SendResponse(cseq, 401, "Unauthorized",
                        "WWW-Authenticate: Basic realm=\"IPCamera\"\r\n", "");
  }
  if (!StreamUrlAllowed(url)) {
    return SendResponse(cseq, 404, "Stream not found", "", "");
  }
  // Older clients omit Session on this persistent connection. If supplied,
  // however, it must refer to an established SETUP on this connection.
  const auto session = headers.find("session");
  if (session != headers.end() &&
      ((videoSetupUrl.empty() && audioSetupUrl.empty()) ||
       session->second.substr(0, session->second.find(';')) != id)) {
    return SendResponse(cseq, 454, "Session Not Found", "", "");
  }
  if (method == "DESCRIBE") {
    contentBase = url.find("://") == std::string::npos ? "" : StreamBase(url);
    std::string extra = "Content-Type: application/sdp\r\n";
    if (!contentBase.empty()) extra += "Content-Base: " + contentBase + "\r\n";
    return SendResponse(cseq, 200, "OK", extra, server->BuildSdp(local.addr.ss_family == AF_INET6, local.Ip()));
  }
  if (method == "SETUP") {
    auto path = url.substr(0, url.find_first_of("?#"));
    while (!path.empty() && path.back() == '/') path.pop_back();
    const auto trackName = path.substr(path.rfind('/') + 1);
    const bool audio = trackName == "trackID=1";
    if ((!audio && trackName != "trackID=0") || (audio && !server->GetAacInfo().valid))
      return SendResponse(cseq, 404, "Track not found", "", "");
    if (playing.load()) return SendResponse(cseq, 455, "Method Not Valid in This State", "", "");
    Track next;
    if (!ParseTransport(headers["transport"], audio, next.spec))
      return SendResponse(cseq, 461, "Unsupported transport", "", "");
    const auto& t = next.spec;
    std::ostringstream extra;
    if (t.tcp) {
      // Track changes only on this reader. Writer takes read-only snapshots.
      const Track& other = audio ? videoTrack : audioTrack;
      if (other.setup && other.spec.tcp &&
          (other.spec.rtp == t.rtp || other.spec.rtp == t.rtcp ||
           other.spec.rtcp == t.rtp || other.spec.rtcp == t.rtcp))
        return SendResponse(cseq, 461, "Channel in use", "", "");
      extra << "Transport: RTP/AVP/TCP;unicast;interleaved=" << t.rtp << '-' << t.rtcp;
    } else {
      int serverPort;
      if (!OpenUdpPair(next, serverPort)) return SendResponse(cseq, 500, "UDP pair unavailable", "", "");
      extra << "Transport: RTP/AVP;unicast;client_port=" << t.rtp << '-' << t.rtcp
            << ";server_port=" << serverPort << '-' << serverPort + 1;
    }
    extra << "\r\nSession: " << id << "\r\n";
    next.setup = true;
    {
      std::lock_guard<std::mutex> lk(stateMu);
      (audio ? audioTrack : videoTrack) = std::move(next);
    }
    (audio ? audioSetupUrl : videoSetupUrl) = TrackUrl(url, contentBase);
    return SendResponse(cseq, 200, "OK", extra.str(), "");
  }
  if (method == "PLAY") {
    if (videoSetupUrl.empty() && audioSetupUrl.empty()) {
      return SendResponse(cseq, 455, "Method Not Valid in This State", "", "");
    }
    if (contentBase.empty()) contentBase = StreamBase(url);
    return SendResponse(cseq, 200, "OK", "Session: " + id + "\r\n", "", PlaybackChange::kStart);
  }
  if (method == "PAUSE") {
    return SendResponse(cseq, 200, "OK", "Session: " + id + "\r\n", "",
                        PlaybackChange::kStop);
  }
  if (method == "TEARDOWN") {
    return SendResponse(cseq, 200, "OK", "Session: " + id + "\r\n", "",
                        PlaybackChange::kStop, true);
  }
  if (method == "GET_PARAMETER" || method == "SET_PARAMETER") {
    return SendResponse(cseq, 200, "OK", session == headers.end() ? "" : "Session: " + id + "\r\n", "");
  }
  return SendResponse(cseq, 501, "Not implemented", "", "");
}

bool RtspServer::Session::StreamUrlAllowed(const std::string& url) {
  std::string path;
  {
    std::lock_guard<std::mutex> lk(server->configMu_);
    path = server->config_.path;
  }
  return RtspPathMatches(url, path);
}

bool RtspServer::Session::CheckAuth(const std::map<std::string, std::string>& headers) {
  RtspConfig cfg;
  {
    std::lock_guard<std::mutex> lk(server->configMu_);
    cfg = server->config_;
  }
  if (cfg.user.empty()) return true;
  auto it = headers.find("authorization");
  if (it == headers.end()) return false;
  const std::string& auth = it->second;
  // RFC 7617: the scheme token is case-insensitive.
  if (auth.size() < 6 || ToLower(auth.substr(0, 6)) != "basic ") return false;
  std::string decoded;
  if (!Base64Decode(auth.substr(6), &decoded)) return false;
  std::string expect = cfg.user + ":" + cfg.password;
  return decoded == expect;
}

bool RtspServer::Session::SendResponse(const std::string& cseq, int code,
                                       const std::string& reason,
                                       const std::string& extraHeaders,
                                       const std::string& body,
                                       PlaybackChange playbackChange,
                                       bool closeAfter) {
  std::ostringstream rsp;
  rsp << "RTSP/1.0 " << code << " " << reason << "\r\n";
  rsp << "CSeq: " << cseq << "\r\n";
  rsp << "Server: harmony-ipcamera\r\n";
  rsp << extraHeaders;
  QueueItem item;
  if (playbackChange == PlaybackChange::kStart) {
    // WriterLoop appends RTP-Info and the terminating blank line.
    item.videoUrl = TrackUrl(videoSetupUrl, contentBase);
    item.audioUrl = TrackUrl(audioSetupUrl, contentBase);
  } else {
    if (!body.empty()) rsp << "Content-Length: " << body.size() << "\r\n";
    rsp << "\r\n" << body;
  }
  item.type = QueueItemType::kResponse;
  item.response = rsp.str();
  item.playbackChange = playbackChange;
  item.closeAfter = closeAfter;
  return EnqueueControl(std::move(item));
}

void RtspServer::Session::SendVideoFrame(const SharedFrame& frame) {
  if (!active.load() || fd.load() < 0 || frame.bytes.empty()) return;
  Track track;
  {
    std::lock_guard<std::mutex> lk(stateMu);
    track = videoTrack;
  }
  if (!track.setup) return;

  std::vector<NalView> nals = SplitAnnexB(frame.bytes.data(), frame.bytes.size());
  if (nals.empty()) return;
  for (auto& nal : nals) {
    while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
  }
  const bool h265 = frame.h265;
  if (frame.tsUs < 0) return;
  const uint32_t ts90k = RtpTime(frame.tsUs, 90000);
  uint8_t pkt[16 + kRtpPayloadMax];
  auto writeRtp = [&](bool marker, const uint8_t* body, size_t bodyLen) {
    pkt[0] = 0x80;
    pkt[1] = static_cast<uint8_t>(kVideoPt | (marker ? 0x80 : 0x00));
    pkt[2] = static_cast<uint8_t>(videoSeq >> 8);
    pkt[3] = static_cast<uint8_t>(videoSeq & 0xFF);
    pkt[4] = static_cast<uint8_t>(ts90k >> 24);
    pkt[5] = static_cast<uint8_t>((ts90k >> 16) & 0xFF);
    pkt[6] = static_cast<uint8_t>((ts90k >> 8) & 0xFF);
    pkt[7] = static_cast<uint8_t>(ts90k & 0xFF);
    pkt[8] = static_cast<uint8_t>(videoSsrc >> 24);
    pkt[9] = static_cast<uint8_t>((videoSsrc >> 16) & 0xFF);
    pkt[10] = static_cast<uint8_t>((videoSsrc >> 8) & 0xFF);
    pkt[11] = static_cast<uint8_t>(videoSsrc & 0xFF);
    videoSeq++;
    memcpy(pkt + 12, body, bodyLen);
    if (!DispatchPacket(track.spec.tcp, track.spec.rtp, track.rtp, pkt, 12 + bodyLen)) return false;
    videoSender.Sent(bodyLen, 90000);
    return true;
  };
  for (size_t ni = 0; ni < nals.size(); ++ni) {
    const NalView& nal = nals[ni];
    if (nal.data == nullptr || nal.size == 0) continue;
    bool lastNal = (ni + 1 == nals.size());
    size_t nalHdrLen = h265 ? 2 : 1;
    if (nal.size <= nalHdrLen) continue;
    if (nal.size <= kRtpPayloadMax) {
      if (!writeRtp(lastNal, nal.data, nal.size)) return;
    } else if (!h265) {
      uint8_t indicator = static_cast<uint8_t>((nal.data[0] & 0xE0) | 28);
      uint8_t nalType = static_cast<uint8_t>(nal.data[0] & 0x1F);
      size_t offset = 1;
      bool first = true;
      while (offset < nal.size) {
        size_t chunk = std::min(kRtpPayloadMax - 2, nal.size - offset);
        bool lastFrag = (offset + chunk == nal.size);
        uint8_t fuh = static_cast<uint8_t>((first ? 0x80 : 0x00) |
                                           (lastFrag ? 0x40 : 0x00) | nalType);
        uint8_t body[2 + kRtpPayloadMax];
        body[0] = indicator;
        body[1] = fuh;
        memcpy(body + 2, nal.data + offset, chunk);
        if (!writeRtp(lastFrag && lastNal, body, 2 + chunk)) return;
        offset += chunk;
        first = false;
      }
    } else {
      uint8_t ph[2];
      ph[0] = static_cast<uint8_t>((nal.data[0] & 0x81) | (49 << 1));
      ph[1] = nal.data[1];
      uint8_t nalType = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      size_t offset = 2;
      bool first = true;
      while (offset < nal.size) {
        size_t chunk = std::min(kRtpPayloadMax - 3, nal.size - offset);
        bool lastFrag = (offset + chunk == nal.size);
        uint8_t fuh = static_cast<uint8_t>((first ? 0x80 : 0x00) |
                                           (lastFrag ? 0x40 : 0x00) | nalType);
        uint8_t body[3 + kRtpPayloadMax];
        body[0] = ph[0];
        body[1] = ph[1];
        body[2] = fuh;
        memcpy(body + 3, nal.data + offset, chunk);
        if (!writeRtp(lastFrag && lastNal, body, 3 + chunk)) return;
        offset += chunk;
        first = false;
      }
    }
  }
}

void RtspServer::Session::SendAudioFrame(const SharedFrame& frame) {
  if (!active.load() || fd.load() < 0 || frame.bytes.empty()) return;
  Track track;
  {
    std::lock_guard<std::mutex> lk(stateMu);
    track = audioTrack;
  }
  if (!track.setup) return;

  AdtsHeader h;
  if (!ParseAdts(frame.bytes.data(), frame.bytes.size(), &h)) return;
  size_t size = static_cast<size_t>(h.frameLength) - static_cast<size_t>(h.headerLen);
  if (size == 0 || size > kRtpPayloadMax) return;
  const uint8_t* payload = frame.bytes.data() + h.headerLen;
  if (frame.tsUs < 0) return;
  const uint32_t tsSamples = RtpTime(frame.tsUs, h.sampleRate);

  uint8_t pkt[12 + 4 + kRtpPayloadMax];
  pkt[0] = 0x80;
  pkt[1] = static_cast<uint8_t>(kAudioPt | 0x80);
  pkt[2] = static_cast<uint8_t>(audioSeq >> 8);
  pkt[3] = static_cast<uint8_t>(audioSeq & 0xFF);
  pkt[4] = static_cast<uint8_t>(tsSamples >> 24);
  pkt[5] = static_cast<uint8_t>((tsSamples >> 16) & 0xFF);
  pkt[6] = static_cast<uint8_t>((tsSamples >> 8) & 0xFF);
  pkt[7] = static_cast<uint8_t>(tsSamples & 0xFF);
  pkt[8] = static_cast<uint8_t>(audioSsrc >> 24);
  pkt[9] = static_cast<uint8_t>((audioSsrc >> 16) & 0xFF);
  pkt[10] = static_cast<uint8_t>((audioSsrc >> 8) & 0xFF);
  pkt[11] = static_cast<uint8_t>(audioSsrc & 0xFF);
  audioSeq++;
  pkt[12] = 0x00;
  pkt[13] = 0x10;
  pkt[14] = static_cast<uint8_t>(size >> 5);
  pkt[15] = static_cast<uint8_t>((size & 0x1F) << 3);
  memcpy(pkt + 16, payload, size);
  if (DispatchPacket(track.spec.tcp, track.spec.rtp, track.rtp, pkt, 16 + size))
    audioSender.Sent(4 + size, h.sampleRate);
}

bool RtspServer::Session::DispatchPacket(
    bool tcp, int chan, const std::shared_ptr<UdpSocket>& udpSocket,
    const uint8_t* pkt, size_t len, bool report) {
  if (!active.load()) return false;
  if (tcp) {
    const int socketFd = fd.load();
    if (socketFd < 0 || len > 0xFFFF) return false;
    uint8_t interleaved[4 + 12 + 4 + kRtpPayloadMax];
    if (len > sizeof(interleaved) - 4) return false;
    interleaved[0] = '$';
    interleaved[1] = static_cast<uint8_t>(chan);
    interleaved[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    interleaved[3] = static_cast<uint8_t>(len & 0xFF);
    memcpy(interleaved + 4, pkt, len);
    if (!SendAll(socketFd, interleaved, len + 4, active)) {
      RequestStop("RTP/TCP send failed");
      return false;
    }
    return true;
  }
  if (!udpSocket || udpSocket->fd < 0) return false;
  ssize_t n = ::send(udpSocket->fd, pkt, len,
                     MSG_NOSIGNAL | MSG_DONTWAIT);
  if (n == static_cast<ssize_t>(len)) return true;
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOBUFS)) {
    return false;  // dropped datagram; not part of SR counters
  }
  // A receiver/firewall can drop RTCP only (including connected-UDP ICMP
  // errors). A lost report is not grounds for stopping a healthy RTP stream.
  if (!report) RequestStop("RTP/UDP send failed");
  return false;
}

}  // namespace ipcam
