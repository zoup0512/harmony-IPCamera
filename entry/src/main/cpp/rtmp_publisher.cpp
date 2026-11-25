#include "rtmp_publisher.h"
#include "rtmp_tls_pipe.h"
#include "rtmp_chunk_parser.h"
#include "rtmp_command.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#define LOG_DOMAIN 0xC010
#define LOG_TAG "RtmpPublisher"
#include <hilog/log.h>

namespace ipcam {
namespace {

constexpr uint8_t MSG_SET_CHUNK_SIZE = 1;
constexpr uint8_t MSG_ACK = 3;
constexpr uint8_t MSG_AUDIO = 8;
constexpr uint8_t MSG_VIDEO = 9;
constexpr uint8_t MSG_AMF0_DATA = 18;
constexpr uint8_t MSG_AMF0_COMMAND = 20;

constexpr uint32_t kOutChunkSize = 60000;
constexpr int64_t kStartTimeoutUs = 10LL * 1000LL * 1000LL;
constexpr int64_t kSendTimeoutUs = 10LL * 1000LL * 1000LL;
constexpr int kCancelPollMs = 100;
constexpr size_t kMaxQueuedBytes = 4U * 1024U * 1024U;
constexpr size_t kMaxInboundBufferBytes = 4U * 1024U * 1024U;
constexpr int kMaxResolverWorkers = 4;

std::atomic<int> gResolverWorkers{0};

struct ResolveState {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  int error = EAI_SYSTEM;
  addrinfo* result = nullptr;

  ~ResolveState() {
    if (result != nullptr) freeaddrinfo(result);
  }
};

int64_t NowUs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000LL +
         static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

int RemainingPollMs(int64_t deadlineUs) {
  const int64_t leftUs = deadlineUs - NowUs();
  if (leftUs <= 0) return 0;
  const int64_t roundedMs = (leftUs + 999) / 1000;
  return static_cast<int>(std::min<int64_t>(roundedMs, kCancelPollMs));
}

std::string ErrnoDetail(const char* what, int err) {
  std::string detail(what);
  if (err != 0) {
    detail += ": ";
    detail += std::strerror(err);
  }
  return detail;
}

bool ResolveWithDeadline(const std::string& host, const std::string& port,
                         int64_t deadlineUs, const std::atomic<bool>& cancel,
                         addrinfo** result, std::string* error) {
  if (result == nullptr) return false;
  *result = nullptr;
  int current = gResolverWorkers.load();
  while (current < kMaxResolverWorkers &&
         !gResolverWorkers.compare_exchange_weak(current, current + 1)) {
  }
  if (current >= kMaxResolverWorkers) {
    if (error != nullptr) *error = "DNS resolver busy";
    return false;
  }

  auto state = std::make_shared<ResolveState>();
  try {
    std::thread([state, host, port]() {
      addrinfo hints {};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* resolved = nullptr;
      const int resolveError = getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved);
      {
        std::lock_guard<std::mutex> lk(state->mu);
        state->error = resolveError;
        state->result = resolved;
        state->done = true;
      }
      state->cv.notify_all();
      gResolverWorkers.fetch_sub(1);
    }).detach();
  } catch (...) {
    gResolverWorkers.fetch_sub(1);
    if (error != nullptr) *error = "failed to create DNS resolver";
    return false;
  }

  std::unique_lock<std::mutex> lk(state->mu);
  while (!state->done && !cancel.load()) {
    const int timeoutMs = RemainingPollMs(deadlineUs);
    if (timeoutMs <= 0) break;
    state->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs));
  }
  if (!state->done) {
    if (error != nullptr) *error = cancel.load() ? "cancelled" : "DNS lookup timed out";
    return false;
  }
  if (state->error != 0 || state->result == nullptr) {
    if (error != nullptr) {
      *error = std::string("DNS lookup failed: ") + gai_strerror(state->error);
    }
    return false;
  }
  *result = state->result;
  state->result = nullptr;
  return true;
}

bool SetNonBlocking(int fd, std::string* error) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    const int err = errno;
    if (error != nullptr) *error = ErrnoDetail("set nonblocking failed", err);
    return false;
  }
  return true;
}

bool WaitForFd(int fd, short events, int64_t deadlineUs,
               const std::atomic<bool>& cancel, std::string* error) {
  while (!cancel.load()) {
    const int timeoutMs = RemainingPollMs(deadlineUs);
    if (timeoutMs <= 0) {
      if (error != nullptr) *error = "operation timed out";
      return false;
    }
    pollfd p {fd, static_cast<short>(events | POLLERR | POLLHUP | POLLNVAL), 0};
    const int pr = ::poll(&p, 1, timeoutMs);
    if (pr > 0) {
      if ((p.revents & events) != 0) return true;
      if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        int socketError = 0;
        socklen_t len = sizeof(socketError);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &len) != 0 ||
            socketError == 0) {
          socketError = errno != 0 ? errno : ECONNRESET;
        }
        if (error != nullptr) {
          *error = ErrnoDetail("socket wait failed", socketError);
        }
        return false;
      }
      continue;
    }
    if (pr == 0) continue;
    if (errno == EINTR) continue;
    if (error != nullptr) *error = ErrnoDetail("poll failed", errno);
    return false;
  }
  if (error != nullptr) *error = "cancelled";
  return false;
}

bool SendAll(int fd, const uint8_t* data, size_t len, int64_t deadlineUs,
             const std::atomic<bool>& cancel, std::string* error) {
  size_t off = 0;
  while (off < len && !cancel.load()) {
    const ssize_t n = ::send(fd, data + off, len - off,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      if (error != nullptr) *error = "socket closed while sending";
      return false;
    }
    const int err = errno;
    if (err == EINTR) continue;
    if (err == EAGAIN || err == EWOULDBLOCK) {
      if (!WaitForFd(fd, POLLOUT, deadlineUs, cancel, error)) return false;
      continue;
    }
    if (error != nullptr) *error = ErrnoDetail("send failed", err);
    return false;
  }
  if (off == len) return true;
  if (error != nullptr) *error = "cancelled";
  return false;
}

bool ReadAll(int fd, uint8_t* dst, size_t len, int64_t deadlineUs,
             const std::atomic<bool>& cancel, std::string* error) {
  size_t off = 0;
  while (off < len && !cancel.load()) {
    const ssize_t n = ::recv(fd, dst + off, len - off, MSG_DONTWAIT);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      if (error != nullptr) *error = "server closed connection while reading";
      return false;
    }
    const int err = errno;
    if (err == EINTR) continue;
    if (err == EAGAIN || err == EWOULDBLOCK) {
      if (!WaitForFd(fd, POLLIN, deadlineUs, cancel, error)) return false;
      continue;
    }
    if (error != nullptr) *error = ErrnoDetail("recv failed", err);
    return false;
  }
  if (off == len) return true;
  if (error != nullptr) *error = "cancelled";
  return false;
}

bool ConnectWithDeadline(int fd, const sockaddr* address, socklen_t addressLen,
                         int64_t deadlineUs,
                         const std::atomic<bool>& cancel,
                         std::string* error) {
  if (::connect(fd, address, addressLen) == 0) return true;
  const int connectError = errno;
  if (connectError != EINPROGRESS && connectError != EALREADY &&
      connectError != EWOULDBLOCK) {
    if (error != nullptr) {
      *error = ErrnoDetail("connect failed", connectError);
    }
    return false;
  }

  if (!WaitForFd(fd, POLLOUT, deadlineUs, cancel, error)) return false;

  int socketError = 0;
  socklen_t len = sizeof(socketError);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &len) != 0) {
    if (error != nullptr) *error = ErrnoDetail("getsockopt failed", errno);
    return false;
  }
  if (socketError != 0) {
    if (error != nullptr) *error = ErrnoDetail("connect failed", socketError);
    return false;
  }
  return true;
}

void Put3(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>(x & 0xFF));
}

void Put4LE(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

void Put4(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>(x & 0xFF));
}

// ---- minimal AMF0 writer ----

void AmfString(std::vector<uint8_t>& v, const std::string& s) {
  v.push_back(0x02);
  v.push_back(static_cast<uint8_t>((s.size() >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>(s.size() & 0xFF));
  v.insert(v.end(), s.begin(), s.end());
}

void AmfNumber(std::vector<uint8_t>& v, double d) {
  v.push_back(0x00);
  uint64_t bits;
  memcpy(&bits, &d, 8);
  for (int i = 7; i >= 0; --i) {
    v.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
  }
}

void AmfNull(std::vector<uint8_t>& v) { v.push_back(0x05); }

void AmfMapBegin(std::vector<uint8_t>& v, uint32_t count) {
  v.push_back(0x08);  // ECMA array
  Put4(v, count);
}

void AmfKey(std::vector<uint8_t>& v, const std::string& k) {
  v.push_back(static_cast<uint8_t>((k.size() >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>(k.size() & 0xFF));
  v.insert(v.end(), k.begin(), k.end());
}

void AmfMapEnd(std::vector<uint8_t>& v) {
  AmfKey(v, "");
  v.push_back(0x09);
}

std::vector<uint8_t> ConnectCommand(const std::string& app,
                                    const std::string& tcUrl) {
  std::vector<uint8_t> v;
  AmfString(v, "connect");
  AmfNumber(v, 1);
  v.push_back(0x03);  // object
  AmfKey(v, "app");
  AmfString(v, app);
  AmfKey(v, "type");
  AmfString(v, "private");
  AmfKey(v, "flashVer");
  AmfString(v, "FMLE/3.0 (compatible; harmony-ipcamera)");
  AmfKey(v, "tcUrl");
  AmfString(v, tcUrl);
  AmfMapEnd(v);
  return v;
}

std::vector<uint8_t> CreateStreamCommand() {
  std::vector<uint8_t> v;
  AmfString(v, "createStream");
  AmfNumber(v, 2);
  AmfNull(v);
  return v;
}

std::vector<uint8_t> PublishCommand(const std::string& stream) {
  std::vector<uint8_t> v;
  AmfString(v, "publish");
  AmfNumber(v, 0);  // publish is acknowledged via onStatus, not _result
  AmfNull(v);
  AmfString(v, stream);
  AmfString(v, "live");
  return v;
}

std::vector<uint8_t> MetaDataCommand(int width, int height, int audioRate,
                                     int audioChannels, bool h265) {
  std::vector<uint8_t> v;
  AmfString(v, "@setDataFrame");
  AmfString(v, "onMetaData");
  AmfMapBegin(v, 8);
  AmfKey(v, "width");
  AmfNumber(v, width);
  AmfKey(v, "height");
  AmfNumber(v, height);
  AmfKey(v, "framerate");
  AmfNumber(v, 30);
  if (h265) {
    AmfKey(v, "videocodecid");
    AmfString(v, "hvc1");
  } else {
    AmfKey(v, "videocodecid");
    AmfNumber(v, 7);
  }
  AmfKey(v, "audiocodecid");
  AmfNumber(v, 10);
  AmfKey(v, "audiosamplerate");
  AmfNumber(v, audioRate);
  AmfKey(v, "audiochannels");
  AmfNumber(v, audioChannels);
  AmfKey(v, "encoder");
  AmfString(v, "harmony-ipcamera");
  AmfMapEnd(v);
  return v;
}

// ---- Annex-B NAL split (same semantics as the RTSP server) ----

struct NalView {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

std::vector<NalView> SplitAnnexB(const uint8_t* data, size_t size) {
  std::vector<NalView> nals;
  size_t i = 0;
  size_t nalStart = std::string::npos;
  while (i + 3 <= size) {
    size_t startCode = 0;
    if (i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 &&
        data[i + 2] == 0x00 && data[i + 3] == 0x01) {
      startCode = 4;
    } else if (data[i] == 0x00 && data[i + 1] == 0x00 &&
               data[i + 2] == 0x01) {
      startCode = 3;
    }
    if (startCode != 0) {
      if (nalStart != std::string::npos && i > nalStart) {
        nals.push_back({data + nalStart, i - nalStart});
      }
      i += startCode;
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

void Put2Len(std::vector<uint8_t>& v, size_t len) {
  v.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>(len & 0xFF));
}

std::vector<uint8_t> AvcSequenceHeader(const std::vector<uint8_t>& sps,
                                       const std::vector<uint8_t>& pps) {
  std::vector<uint8_t> rec;
  rec.push_back(0x01);
  rec.push_back(sps.size() > 1 ? sps[1] : 0x64);
  rec.push_back(sps.size() > 2 ? sps[2] : 0x00);
  rec.push_back(sps.size() > 3 ? sps[3] : 0x1F);
  rec.push_back(0xFF);
  rec.push_back(0xE1);
  Put2Len(rec, sps.size());
  rec.insert(rec.end(), sps.begin(), sps.end());
  rec.push_back(0x01);
  Put2Len(rec, pps.size());
  rec.insert(rec.end(), pps.begin(), pps.end());
  std::vector<uint8_t> body;
  body.push_back(0x17);  // keyframe + AVC
  body.push_back(0x00);  // AVC sequence header
  Put3(body, 0);
  body.insert(body.end(), rec.begin(), rec.end());
  return body;
}

std::vector<uint8_t> AacSequenceHeader(int profileBits, int sfIndex,
                                       int channels) {
  const int aot = profileBits + 1;
  std::vector<uint8_t> body;
  body.push_back(0xAF);  // AAC | rate flag | 16-bit | stereo flag
  body.push_back(0x00);  // sequence header
  body.push_back(static_cast<uint8_t>((aot << 3) | (sfIndex >> 1)));
  body.push_back(
      static_cast<uint8_t>(((sfIndex & 1) << 7) | (channels << 3)));
  return body;
}

// HEVCDecoderConfigurationRecord for enhanced-RTMP sequence start. Built from
// the VPS/SPS/PPS NALUs; profile fields come straight from the SPS
// profile_tier_level bytes.
std::vector<uint8_t> HevcConfigurationRecord(
    const std::vector<uint8_t>& vps, const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps) {
  std::vector<uint8_t> r;
  r.push_back(0x01);  // configurationVersion
  if (sps.size() > 13) {
    r.push_back(sps[2]);                                  // profile byte
    r.insert(r.end(), sps.begin() + 3, sps.begin() + 7);  // compatibility
    r.insert(r.end(), sps.begin() + 7, sps.begin() + 13); // constraints
    r.push_back(sps[13]);                                 // level_idc
  } else {
    r.insert(r.end(), 12, 0x01);
  }
  r.push_back(0xF0);
  r.push_back(0x00);
  r.push_back(0xFC);
  r.push_back(0xFD);
  r.push_back(0xE0);
  r.push_back(0xE0);
  r.push_back(0x00);
  r.push_back(0x00);
  r.push_back(0x1F);
  r.push_back(0x03);  // numOfArrays
  auto pushArray = [&r](uint8_t nalType, const std::vector<uint8_t>& nal) {
    r.push_back(static_cast<uint8_t>(0x80 | nalType));
    r.push_back(0x00);
    r.push_back(0x01);
    r.push_back(static_cast<uint8_t>((nal.size() >> 8) & 0xFF));
    r.push_back(static_cast<uint8_t>(nal.size() & 0xFF));
    r.insert(r.end(), nal.begin(), nal.end());
  };
  pushArray(32, vps);
  pushArray(33, sps);
  pushArray(34, pps);
  return r;
}

std::vector<uint8_t> HevcSequenceTag(const std::vector<uint8_t>& vps,
                                     const std::vector<uint8_t>& sps,
                                     const std::vector<uint8_t>& pps) {
  std::vector<uint8_t> body;
  body.push_back(0x90);  // enhanced keyframe + sequence start
  body.push_back('h');
  body.push_back('v');
  body.push_back('c');
  body.push_back('1');
  auto rec = HevcConfigurationRecord(vps, sps, pps);
  body.insert(body.end(), rec.begin(), rec.end());
  return body;
}

}  // namespace

struct RtmpPublisher::PeerSession {
  enum Stage { Connect, Create, Publish, Live } stage = Connect;
  bool confirmed = false;
  uint32_t streamId = 0, window = 0;
  uint64_t received = 0, lastAck = 0;
  RtmpChunkParser parser{static_cast<uint32_t>(kMaxInboundBufferBytes)};
  std::vector<uint8_t> buffer;
};

RtmpPublisher::~RtmpPublisher() { Stop(); }

std::string RtmpPublisher::LastError() const {
  std::lock_guard<std::mutex> lk(errMu_);
  return lastError_;
}

std::vector<uint8_t> RtmpPublisher::BuildWireMessage(const OutMsg& msg) {
  std::vector<uint8_t> wire;
  const bool ext = msg.tsMs >= 0xFFFFFF;
  wire.push_back(msg.csid & 0x3F);  // fmt=0
  Put3(wire, ext ? 0xFFFFFF : msg.tsMs);
  Put3(wire, static_cast<uint32_t>(msg.payload.size()));
  wire.push_back(msg.type);
  Put4LE(wire, msg.streamId);
  if (ext) Put4(wire, msg.tsMs);
  size_t off = 0;
  bool first = true;
  while (first || off < msg.payload.size()) {
    const size_t n =
        std::min<size_t>(kOutChunkSize, msg.payload.size() - off);
    if (!first) {
      wire.push_back(0xC0 | (msg.csid & 0x3F));
      if (ext) Put4(wire, msg.tsMs);
    }
    wire.insert(wire.end(), msg.payload.begin() + off,
                msg.payload.begin() + off + n);
    off += n;
    first = false;
  }
  return wire;
}

size_t RtmpPublisher::QueuedBytes(const OutMsg& msg) {
  return msg.payload.size() + 18U;
}

void RtmpPublisher::ResetQueueLocked() {
  queue_.clear();
  queuedBytes_ = 0;
  inFlightBytes_ = 0;
  waitingForKeyframe_ = true;
  queuedVideoConfigSet_ = false;
  queuedVideoConfigVersion_ = 0;
  queuedAudioConfigSet_ = false;
  queuedAudioConfig_ = 0;
}

void RtmpPublisher::ResetQueue() {
  {
    std::lock_guard<std::mutex> lk(queueMu_);
    ResetQueueLocked();
  }
  queueCv_.notify_all();
}

void RtmpPublisher::ResetSessionState() {
  ResetQueue();
  std::lock_guard<std::mutex> lk(paramMu_);
  audioRate_ = 48000;
  audioChannels_ = 1;
  videoIsH265_ = presetVideoParamsValid_ ? presetVideoIsH265_ : false;
  vps_ = presetVideoParamsValid_ ? presetVps_ : std::vector<uint8_t>();
  sps_ = presetVideoParamsValid_ ? presetSps_ : std::vector<uint8_t>();
  pps_ = presetVideoParamsValid_ ? presetPps_ : std::vector<uint8_t>();
  videoConfigVersion_ = 1;
  tsBaseUs_ = 0;
  tsBaseSet_ = false;
  lastVideoTsUs_ = 0;
  lastAudioTsUs_ = 0;
  lastVideoTsSet_ = false;
  lastAudioTsSet_ = false;
}

void RtmpPublisher::FailOnce(const std::string& detail, bool startup) {
  if (cancelRequested_.load()) return;
  bool expected = false;
  if (!errorReported_.compare_exchange_strong(expected, true)) return;

  const std::string message =
      std::string(startup ? "startup failed: " : "connection lost: ") +
      detail;
  ErrorSink callback;
  {
    std::lock_guard<std::mutex> lk(errMu_);
    lastError_ = message;
    callback = onError_;
  }
  if (callback) callback(message);
}

void RtmpPublisher::InterruptSocket() {
  std::lock_guard<std::mutex> lk(sockMu_);
  if (sock_ >= 0) ::shutdown(sock_, SHUT_RDWR);
}

void RtmpPublisher::CloseSocketIf(int expectedFd) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lk(sockMu_);
    if (sock_ != expectedFd) return;
    fd = sock_;
    sock_ = -1;
  }
  if (fd >= 0) ::close(fd);
}

void RtmpPublisher::AbortRuntime(const std::string& detail) {
  running_.store(false);
  FailOnce(detail, false);
  cancelRequested_.store(true);
  queueCv_.notify_all();
  InterruptSocket();
}

void RtmpPublisher::SetVideoParams(bool h265,
                                   const std::vector<uint8_t>& vps,
                                   const std::vector<uint8_t>& sps,
                                   const std::vector<uint8_t>& pps) {
  if (running_.load()) return;
  std::lock_guard<std::mutex> lk(paramMu_);
  presetVideoParamsValid_ = !sps.empty() && !pps.empty() &&
                            (!h265 || !vps.empty());
  presetVideoIsH265_ = h265;
  presetVps_ = vps;
  presetSps_ = sps;
  presetPps_ = pps;

  if (!running_.load() && presetVideoParamsValid_) {
    videoIsH265_ = h265;
    vps_ = vps;
    sps_ = sps;
    pps_ = pps;
    ++videoConfigVersion_;
  }
}

bool RtmpPublisher::Start(const std::string& url, int width, int height,
                          bool h265Hint, ErrorSink onError, uint32_t authenticatedTlsToken) {
  {
    std::lock_guard<std::mutex> stopLock(stopMu_);
    std::unique_lock<std::mutex> lk(lifecycleMu_);
    if (lifecycleState_ != LifecycleState::kIdle) return false;
    // Stop() on a never-started instance represents cancellation of pending
    // async work. Do not clear that latch and begin a late connection.
    if (cancelRequested_.load()) {
      std::lock_guard<std::mutex> errLock(errMu_);
      lastError_ = "startup cancelled before Start";
      onError_ = nullptr;
      startAttempted_ = true;
      return false;
    }
    startAttempted_ = true;
    startWorkerActive_ = true;
    lifecycleState_ = LifecycleState::kStarting;
  }

  running_.store(false);
  errorReported_.store(false);
  {
    std::lock_guard<std::mutex> lk(errMu_);
    lastError_.clear();
    onError_ = std::move(onError);
  }
  ResetSessionState();
  {
    std::lock_guard<std::mutex> lk(paramMu_);
    width_ = width;
    height_ = height;
    if (!presetVideoParamsValid_) videoIsH265_ = h265Hint;
  }

  auto finishFailedStart = [this](const std::string& detail, int fd) {
    const bool cancelled = cancelRequested_.load();
    if (fd >= 0) CloseSocketIf(fd);
    running_.store(false);
    ResetSessionState();
    {
      std::lock_guard<std::mutex> lk(lifecycleMu_);
      startWorkerActive_ = false;
      if (lifecycleState_ != LifecycleState::kStopping) {
        lifecycleState_ = LifecycleState::kIdle;
      }
    }
    lifecycleCv_.notify_all();
    if (!cancelled) FailOnce(detail, true);
    {
      std::lock_guard<std::mutex> lk(errMu_);
      if (cancelled && lastError_.empty()) lastError_ = "startup cancelled";
      onError_ = nullptr;
    }
    return false;
  };

  const bool secure = url.rfind("rtmps://", 0) == 0;
  if (secure && !authenticatedTlsToken) return finishFailedStart("RTMPS transport unavailable", -1);
  if ((!secure && (authenticatedTlsToken || url.rfind("rtmp://", 0) != 0)) || url.size() > 65535 ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) {
        return c <= 32 || c == 127 || c == '#' || c == '\\';
      })) return finishFailedStart("invalid RTMP URL", -1);
  const size_t schemeEnd = secure ? 8 : 7;
  const size_t slash = url.find('/', schemeEnd);
  const size_t appEnd = slash == std::string::npos ? slash : url.find('/', slash + 1);
  if (appEnd == std::string::npos || slash + 1 == appEnd || appEnd + 1 >= url.size() ||
      url[appEnd + 1] == '/' || url[appEnd + 1] == '?' || url.find('?') < appEnd) {
    return finishFailedStart("url missing app or stream key", -1);
  }
  const std::string authority = url.substr(schemeEnd, slash - schemeEnd);
  std::string host;
  size_t endHost = 0;
  if (!authority.empty() && authority.front() == '[') {
    endHost = authority.find(']');
    if (endHost == std::string::npos) return finishFailedStart("invalid IPv6 host", -1);
    host = authority.substr(1, endHost - 1);
    in6_addr numeric{};
    if (inet_pton(AF_INET6, host.c_str(), &numeric) != 1) return finishFailedStart("invalid IPv6 host", -1);
    ++endHost;
  } else {
    endHost = authority.find(':');
    if (endHost == std::string::npos) endHost = authority.size();
    host = authority.substr(0, endHost);
    if (host.empty() || host.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-") !=
        std::string::npos) return finishFailedStart("invalid RTMP host", -1);
  }
  std::string port = secure ? "443" : "1935";
  if (endHost != authority.size()) {
    port = authority.substr(endHost + 1);
    if (authority[endHost] != ':' || port.empty() || port.find_first_not_of("0123456789") != std::string::npos ||
        std::strtol(port.c_str(), nullptr, 10) <= 0 || std::strtol(port.c_str(), nullptr, 10) > 65535) {
      return finishFailedStart("invalid RTMP port", -1);
    }
    port = std::to_string(std::strtol(port.c_str(), nullptr, 10));
  }
  const std::string app = url.substr(slash + 1, appEnd - slash - 1);
  const std::string stream = url.substr(appEnd + 1);
  const int64_t deadlineUs = NowUs() + kStartTimeoutUs;
  std::string error;
  int fd = -1;
  if (secure) {
    fd = RtmpTlsPipe::Instance().Take(authenticatedTlsToken, authority);
    if (fd < 0) return finishFailedStart("RTMPS transport unavailable", -1);
    std::lock_guard<std::mutex> lk(sockMu_);
    sock_ = fd;
  } else {
    addrinfo* res = nullptr;
    if (!ResolveWithDeadline(host, port, deadlineUs, cancelRequested_, &res, &error)) {
      return finishFailedStart(error, -1);
    }
    for (addrinfo* ai = res; ai && !cancelRequested_.load() && NowUs() < deadlineUs; ai = ai->ai_next) {
      fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0) { error = ErrnoDetail("socket failed", errno); continue; }
      {
        std::lock_guard<std::mutex> lk(sockMu_);
        sock_ = fd;
      }
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      // Leave a share of the single startup budget for the next family/address.
      int64_t attemptEnd = ai->ai_next ? std::min<int64_t>(deadlineUs, NowUs() + 2000000) : deadlineUs;
      if (SetNonBlocking(fd, &error) && ConnectWithDeadline(fd, ai->ai_addr, ai->ai_addrlen,
                                                            attemptEnd, cancelRequested_, &error)) break;
      CloseSocketIf(fd);
      fd = -1;
    }
    freeaddrinfo(res);
  }
  if (fd < 0) return finishFailedStart(error.empty() ? "operation timed out" : error, -1);
  auto peer = std::make_unique<PeerSession>();
  if (!ConnectAndPublish(url.substr(0, appEnd), app, stream, fd, *peer, deadlineUs, &error)) {
    return finishFailedStart(error, fd);
  }
  if (cancelRequested_.load()) return finishFailedStart("cancelled", fd);

  bool cancelledBeforeSender = false;
  bool senderCreateFailed = false;
  {
    std::lock_guard<std::mutex> lk(lifecycleMu_);
    if (lifecycleState_ != LifecycleState::kStarting ||
        cancelRequested_.load()) {
      cancelledBeforeSender = true;
    } else {
      running_.store(true);
      try {
        senderThread_ = std::thread(&RtmpPublisher::SenderLoop, this, fd, std::move(peer));
      } catch (...) {
        running_.store(false);
        senderCreateFailed = true;
      }
      if (!senderCreateFailed) {
        startWorkerActive_ = false;
        lifecycleState_ = LifecycleState::kRunning;
      } else {
        startWorkerActive_ = false;
      }
    }
  }
  if (cancelledBeforeSender) return finishFailedStart("cancelled", fd);
  if (senderCreateFailed) {
    return finishFailedStart("failed to create sender thread", fd);
  }
  lifecycleCv_.notify_all();
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "RTMP publishing started");
  return true;
}

bool RtmpPublisher::ConnectAndPublish(const std::string& tcUrl,
                                      const std::string& app,
                                      const std::string& stream, int fd, PeerSession& peer,
                                      int64_t deadlineUs,
                                      std::string* error) {
  // ---- handshake: C0+C1, read S0+S1+S2, echo C2 ----
  std::vector<uint8_t> c1;
  c1.push_back(0x03);
  c1.resize(1 + 1536);
  memset(c1.data() + 1, 0, 8);
  for (int i = 8; i < 1536; ++i) {
    c1[1 + i] = static_cast<uint8_t>(rand() & 0xFF);
  }
  if (!SendAll(fd, c1.data(), c1.size(), deadlineUs, cancelRequested_, error)) {
    return false;
  }
  uint8_t s0s1s2[1 + 1536 + 1536];
  if (!ReadAll(fd, s0s1s2, 1 + 1536, deadlineUs, cancelRequested_,
               error)) {
    return false;
  }
  if (s0s1s2[0] != 0x03) {
    if (error != nullptr) *error = "unsupported RTMP handshake version";
    return false;
  }
  // C2 echoes S1 (time + random bytes), not S2.
  if (!SendAll(fd, s0s1s2 + 1, 1536, deadlineUs,
               cancelRequested_, error) ||
      !ReadAll(fd, s0s1s2 + 1537, 1536, deadlineUs, cancelRequested_, error)) {
    return false;
  }

  auto sendMsg = [&](uint8_t type, uint8_t csid, uint32_t streamId,
                     uint32_t tsMs,
                     const std::vector<uint8_t>& payload) -> bool {
    OutMsg msg;
    msg.type = type;
    msg.csid = csid;
    msg.streamId = streamId;
    msg.tsMs = tsMs;
    msg.payload = payload;
    const std::vector<uint8_t> wire = RtmpPublisher::BuildWireMessage(msg);
    return SendAll(fd, wire.data(), wire.size(), deadlineUs,
                   cancelRequested_, error);
  };

  std::vector<uint8_t> chunkSize;
  Put4(chunkSize, kOutChunkSize);
  if (!sendMsg(MSG_SET_CHUNK_SIZE, 2, 0, 0, chunkSize)) return false;
  auto expect = [&](PeerSession::Stage stage) {
    peer.stage = stage;
    peer.confirmed = false;
    while (!peer.confirmed) if (!ReceivePeer(fd, peer, true, deadlineUs, error)) return false;
    return true;
  };
  if (!sendMsg(MSG_AMF0_COMMAND, 3, 0, 0, ConnectCommand(app, tcUrl)) || !expect(PeerSession::Connect) ||
      !sendMsg(MSG_AMF0_COMMAND, 3, 0, 0, CreateStreamCommand()) || !expect(PeerSession::Create) ||
      !sendMsg(MSG_AMF0_COMMAND, 8, peer.streamId, 0, PublishCommand(stream)) || !expect(PeerSession::Publish)) {
    return false;
  }
  peer.stage = PeerSession::Live;

  int w = 0;
  int h = 0;
  int rate = 48000;
  int channels = 1;
  bool h265 = false;
  {
    std::lock_guard<std::mutex> lk(paramMu_);
    w = width_;
    h = height_;
    rate = audioRate_;
    channels = audioChannels_;
    h265 = videoIsH265_;
  }
  return sendMsg(MSG_AMF0_DATA, 5, peer.streamId, 0,
                 MetaDataCommand(w, h, rate, channels, h265));
}

// Hand the same decoder (partial chunks, acknowledgement counters and
// compressed header history) from the startup worker to the sender.
bool RtmpPublisher::ReceivePeer(int fd, PeerSession& peer, bool wait, int64_t deadlineUs, std::string* error) {
  if (cancelRequested_.load()) { *error = "cancelled"; return false; }
  if (NowUs() >= deadlineUs) { *error = "operation timed out"; return false; }
  if (wait && !WaitForFd(fd, POLLIN, deadlineUs, cancelRequested_, error)) return false;
  uint8_t tmp[16384];
  const ssize_t n = ::recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
  if (n == 0) { *error = "server closed connection"; return false; }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
    *error = ErrnoDetail("server recv failed", errno);
    return false;
  }
  peer.received += static_cast<uint64_t>(n);
  peer.buffer.insert(peer.buffer.end(), tmp, tmp + n);
  auto control = [&](uint8_t type, std::vector<uint8_t> payload) {
    OutMsg msg;
    msg.type = type;
    msg.csid = 2;
    msg.payload = std::move(payload);
    auto wire = BuildWireMessage(msg);
    return SendAll(fd, wire.data(), wire.size(), deadlineUs, cancelRequested_, error);
  };
  auto reject = [&](const std::string& code) {
    // Report only allowlisted codes, never free-form text (often includes the key).
    static const char* const known[] = {"NetConnection.Connect.Rejected", "NetConnection.Connect.InvalidApp",
        "NetConnection.Connect.Failed", "NetStream.Publish.BadName", "NetStream.Publish.Denied",
        "NetStream.Publish.Rejected", "NetStream.Publish.Failed", "NetStream.Unpublish.Success"};
    *error = "server rejected: RTMP request denied";
    for (const char* safe : known) if (code == safe) { *error = std::string("server rejected: ") + safe; break; }
    return false;
  };
  auto visit = [&](uint8_t type, uint32_t id, const std::vector<uint8_t>& body) {
    if (type == 5) peer.window = RtmpChunkParser::Be32(body.data());
    if (type == 4 && body.size() >= 2 && body[0] == 0 && body[1] == 6) {
      if (body.size() != 6) { *error = "invalid RTMP ping"; return false; }
      auto pong = body;
      pong[1] = 7;
      return control(4, std::move(pong));
    }
    if (type != 20 && type != 17) return true;
    RtmpCommand c;
    if (!AmfReplyReader(body).Read(c, type == 17)) { *error = "invalid RTMP command"; return false; }
    const bool connection = id == 0;
    const bool stream = peer.streamId != 0 && peer.streamId == id;
    const bool transaction = connection && ((peer.stage == PeerSession::Connect && c.transaction == 1) ||
        (peer.stage == PeerSession::Create && c.transaction == 2));
    if (c.name == "_error" && (transaction || ((connection || stream) &&
        peer.stage >= PeerSession::Publish && c.transaction == 0))) return reject(c.code);
    if (c.name == "_result" && transaction && !peer.confirmed) {
      if (c.level == "error") return reject(c.code);
      if (peer.stage == PeerSession::Create) {
        if (!c.numericResult || c.result < 1 || c.result > UINT32_MAX || std::floor(c.result) != c.result) {
          *error = "invalid createStream stream ID";
          return false;
        }
        peer.streamId = static_cast<uint32_t>(c.result);
      } else if (!c.code.empty() && c.code != "NetConnection.Connect.Success") return reject(c.code);
      peer.confirmed = true;
    }
    if (c.name == "onStatus") {
      if (connection && c.code.rfind("NetConnection.Connect.", 0) == 0 &&
          (c.level == "error" || c.code != "NetConnection.Connect.Success")) return reject(c.code);
      // Some servers report a rejected publish on the connection command channel.
      // A success on that channel must still never confirm a stream.
      if (connection && peer.stage >= PeerSession::Publish && c.level == "error" &&
          c.code.rfind("NetStream.Publish.", 0) == 0) return reject(c.code);
      if (!stream || peer.stage < PeerSession::Publish) return true;
      if (c.level == "error" || c.code == "NetStream.Unpublish.Success" ||
          (c.code.rfind("NetStream.Publish.", 0) == 0 && c.code != "NetStream.Publish.Start" &&
           c.code != "NetStream.Publish.Idle")) return reject(c.code);
      if (peer.stage == PeerSession::Publish && c.transaction == 0 && c.code == "NetStream.Publish.Start") {
        peer.confirmed = true;
      }
    }
    return true;
  };
  RtmpChunkParser::Status status;
  const size_t used = peer.parser.Consume(peer.buffer.data(), peer.buffer.size(), &status, visit);
  if (status == RtmpChunkParser::Status::kStopped) return false;
  if (status != RtmpChunkParser::Status::kOk || peer.buffer.size() - used > kMaxInboundBufferBytes + 18) {
    *error = "invalid RTMP chunk stream";
    return false;
  }
  peer.buffer.erase(peer.buffer.begin(), peer.buffer.begin() + used);
  if (peer.window && peer.received - peer.lastAck >= peer.window) {
    std::vector<uint8_t> ack;
    Put4(ack, static_cast<uint32_t>(peer.received));
    peer.lastAck = peer.received;
    return control(MSG_ACK, std::move(ack));
  }
  return true;
}

void RtmpPublisher::SenderLoop(int fd, std::unique_ptr<PeerSession> peer) {
  while (!cancelRequested_.load()) {
    std::deque<OutMsg> local;
    {
      std::unique_lock<std::mutex> lk(queueMu_);
      if (queue_.empty()) {
        queueCv_.wait_for(lk, std::chrono::milliseconds(250), [this] {
          return cancelRequested_.load() || !queue_.empty();
        });
      }
      if (cancelRequested_.load()) break;
      local.swap(queue_);
      inFlightBytes_ = queuedBytes_;
      queuedBytes_ = 0;
      queuedVideoConfigSet_ = false;
      queuedVideoConfigVersion_ = 0;
      queuedAudioConfigSet_ = false;
      queuedAudioConfig_ = 0;
    }

    for (OutMsg& msg : local) {
      msg.streamId = peer->streamId;
      if (cancelRequested_.load()) break;
      const std::vector<uint8_t> wire = RtmpPublisher::BuildWireMessage(msg);
      std::string error;
      const int64_t deadlineUs = NowUs() + kSendTimeoutUs;
      if (!SendAll(fd, wire.data(), wire.size(), deadlineUs,
                   cancelRequested_, &error)) {
        if (!cancelRequested_.load()) AbortRuntime(error);
        break;
      }
      {
        std::lock_guard<std::mutex> lk(queueMu_);
        const size_t bytes = QueuedBytes(msg);
        inFlightBytes_ = bytes <= inFlightBytes_ ? inFlightBytes_ - bytes : 0;
      }
    }
    {
      std::lock_guard<std::mutex> lk(queueMu_);
      inFlightBytes_ = 0;
    }
    if (cancelRequested_.load()) break;

    std::string detail;
    if (!ReceivePeer(fd, *peer, false, NowUs() + kSendTimeoutUs, &detail)) {
      if (!cancelRequested_.load()) AbortRuntime(detail);
      break;
    }
  }

  running_.store(false);
  CloseSocketIf(fd);
}

bool RtmpPublisher::WaitingForKeyframe() {
  std::lock_guard<std::mutex> lk(queueMu_);
  return waitingForKeyframe_;
}

bool RtmpPublisher::EnqueueVideo(OutMsg frame, OutMsg sequenceHeader,
                                 bool keyFrame, uint64_t configVersion) {
  if (!running_.load() || cancelRequested_.load()) return false;

  bool queued = false;
  bool impossible = false;
  {
    std::lock_guard<std::mutex> lk(queueMu_);
    if (!running_.load() || cancelRequested_.load()) return false;

    const size_t sequenceBytes = QueuedBytes(sequenceHeader);
    const size_t frameBytes = QueuedBytes(frame);
    if (sequenceBytes > kMaxQueuedBytes || frameBytes > kMaxQueuedBytes ||
        sequenceBytes + frameBytes > kMaxQueuedBytes) {
      // No useful recovery is possible if a required header/keyframe pair alone
      // exceeds the hard cap. Fail rather than silently corrupting the stream.
      impossible = true;
    } else {
      if (waitingForKeyframe_) {
        if (!keyFrame) return false;
        ResetQueueLocked();
      }

      const bool needSequence =
          !queuedVideoConfigSet_ || queuedVideoConfigVersion_ != configVersion;
      size_t required = frameBytes + (needSequence ? sequenceBytes : 0);
      if (inFlightBytes_ + queuedBytes_ + required > kMaxQueuedBytes) {
        queue_.clear();
        queuedBytes_ = 0;
        waitingForKeyframe_ = true;
        queuedVideoConfigSet_ = false;
        queuedVideoConfigVersion_ = 0;
        queuedAudioConfigSet_ = false;
        queuedAudioConfig_ = 0;
        if (!keyFrame) return false;
        required = sequenceBytes + frameBytes;
      }

      if (inFlightBytes_ + queuedBytes_ + required <= kMaxQueuedBytes) {
        if (!queuedVideoConfigSet_ ||
            queuedVideoConfigVersion_ != configVersion) {
          queuedBytes_ += sequenceBytes;
          queue_.push_back(std::move(sequenceHeader));
          queuedVideoConfigSet_ = true;
          queuedVideoConfigVersion_ = configVersion;
        }
        queuedBytes_ += frameBytes;
        queue_.push_back(std::move(frame));
        waitingForKeyframe_ = false;
        queued = true;
      } else {
        impossible = true;
      }
    }
  }

  if (impossible) {
    AbortRuntime("RTMP video header/keyframe exceeds 4 MiB queue limit");
    return false;
  }
  if (!queued) return false;
  queueCv_.notify_one();
  return true;
}

bool RtmpPublisher::EnqueueAudio(OutMsg frame, OutMsg sequenceHeader,
                                 uint32_t configKey) {
  if (!running_.load() || cancelRequested_.load()) return false;

  bool overflow = false;
  bool queued = false;
  {
    std::lock_guard<std::mutex> lk(queueMu_);
    if (!running_.load() || cancelRequested_.load()) return false;
    if (waitingForKeyframe_) return false;

    const bool needSequence =
        !queuedAudioConfigSet_ || queuedAudioConfig_ != configKey;
    const size_t sequenceBytes = QueuedBytes(sequenceHeader);
    const size_t frameBytes = QueuedBytes(frame);
    const size_t required = frameBytes + (needSequence ? sequenceBytes : 0);
    if (required > kMaxQueuedBytes ||
        inFlightBytes_ + queuedBytes_ + required > kMaxQueuedBytes) {
      queue_.clear();
      queuedBytes_ = 0;
      waitingForKeyframe_ = true;
      queuedVideoConfigSet_ = false;
      queuedVideoConfigVersion_ = 0;
      queuedAudioConfigSet_ = false;
      queuedAudioConfig_ = 0;
      overflow = true;
    } else {
      if (needSequence) {
        queuedBytes_ += sequenceBytes;
        queue_.push_back(std::move(sequenceHeader));
        queuedAudioConfigSet_ = true;
        queuedAudioConfig_ = configKey;
      }
      queuedBytes_ += frameBytes;
      queue_.push_back(std::move(frame));
      queued = true;
    }
  }

  if (queued) queueCv_.notify_one();
  if (overflow) {
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                 "RTMP queue reached 4 MiB; dropping stale media until next keyframe");
  }
  return queued;
}

bool RtmpPublisher::TimestampToMs(int64_t tsUs, bool video,
                                  uint32_t* tsMs) {
  if (tsMs == nullptr) return false;
  if (tsUs == -1) tsUs = NowUs();
  if (tsUs < 0) return false;

  std::lock_guard<std::mutex> lk(paramMu_);
  if (!tsBaseSet_) {
    tsBaseUs_ = tsUs;
    tsBaseSet_ = true;
  }
  int64_t* lastTs = video ? &lastVideoTsUs_ : &lastAudioTsUs_;
  bool* lastSet = video ? &lastVideoTsSet_ : &lastAudioTsSet_;
  if (*lastSet && tsUs < *lastTs) {
    tsUs = *lastTs;
  }
  if (tsUs < tsBaseUs_) tsUs = tsBaseUs_;
  *lastTs = tsUs;
  *lastSet = true;

  const int64_t relUs = tsUs - tsBaseUs_;
  const int64_t relMs = relUs / 1000LL;
  *tsMs = static_cast<uint32_t>(
      std::min<int64_t>(relMs, std::numeric_limits<uint32_t>::max()));
  return true;
}

void RtmpPublisher::SendH264Packet(const uint8_t* data, size_t size,
                                   int64_t tsUs) {
  if (!running_.load() || cancelRequested_.load() || data == nullptr ||
      size < 5) {
    return;
  }

  std::vector<NalView> nals = SplitAnnexB(data, size);
  if (nals.empty()) return;

  std::vector<uint8_t> avcc;
  bool hasSlice = false;
  bool hasKey = false;
  bool h265 = false;
  uint64_t configVersion = 0;
  std::vector<uint8_t> vps;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;

  {
    std::lock_guard<std::mutex> lk(paramMu_);
    bool detectedH265Param = false;
    bool detectedH264Sps = false;
    for (const NalView& nal : nals) {
      if (nal.size < 2) continue;
      const uint8_t h264Type = nal.data[0] & 0x1F;
      const uint8_t h265Type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      if ((h265Type == 32 || h265Type == 33 || h265Type == 34) &&
          nal.data[1] == 0x01) {
        detectedH265Param = true;
      }
      if (h264Type == 7) detectedH264Sps = true;
    }
    if (detectedH265Param && !videoIsH265_) {
      videoIsH265_ = true;
      vps_.clear();
      sps_.clear();
      pps_.clear();
      ++videoConfigVersion_;
    } else if (detectedH264Sps && videoIsH265_) {
      videoIsH265_ = false;
      vps_.clear();
      sps_.clear();
      pps_.clear();
      ++videoConfigVersion_;
    }

    for (NalView nal : nals) {
      while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) --nal.size;
      if (nal.size < 2) continue;

      if (videoIsH265_) {
        const uint8_t type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
        if (type == 32) {
          const std::vector<uint8_t> next(nal.data, nal.data + nal.size);
          if (next != vps_) {
            vps_ = next;
            ++videoConfigVersion_;
          }
          continue;
        }
        if (type == 33) {
          const std::vector<uint8_t> next(nal.data, nal.data + nal.size);
          if (next != sps_) {
            sps_ = next;
            ++videoConfigVersion_;
          }
          continue;
        }
        if (type == 34) {
          const std::vector<uint8_t> next(nal.data, nal.data + nal.size);
          if (next != pps_) {
            pps_ = next;
            ++videoConfigVersion_;
          }
          continue;
        }
        if (type == 19 || type == 20 || type == 21) hasKey = true;
        if (type > 31) continue;
      } else {
        const uint8_t type = static_cast<uint8_t>(nal.data[0] & 0x1F);
        if (type == 7) {
          const std::vector<uint8_t> next(nal.data, nal.data + nal.size);
          if (next != sps_) {
            sps_ = next;
            ++videoConfigVersion_;
          }
          continue;
        }
        if (type == 8) {
          const std::vector<uint8_t> next(nal.data, nal.data + nal.size);
          if (next != pps_) {
            pps_ = next;
            ++videoConfigVersion_;
          }
          continue;
        }
        if (type == 5) hasKey = true;
        if (type == 6 || type == 9 || type == 10 || type == 11 || type == 12) {
          continue;
        }
      }

      hasSlice = true;
      Put4(avcc, static_cast<uint32_t>(nal.size));
      avcc.insert(avcc.end(), nal.data, nal.data + nal.size);
    }

    h265 = videoIsH265_;
    configVersion = videoConfigVersion_;
    vps = vps_;
    sps = sps_;
    pps = pps_;
  }

  if (!hasSlice || avcc.empty()) return;
  if (h265 ? (vps.empty() || sps.empty() || pps.empty())
           : (sps.empty() || pps.empty())) {
    return;
  }
  if (WaitingForKeyframe() && !hasKey) return;

  uint32_t timestampMs = 0;
  if (!TimestampToMs(tsUs, true, &timestampMs)) return;

  OutMsg sequence;
  sequence.type = MSG_VIDEO;
  sequence.csid = 6;
  sequence.tsMs = 0;
  sequence.payload = h265 ? HevcSequenceTag(vps, sps, pps)
                          : AvcSequenceHeader(sps, pps);

  OutMsg frame;
  frame.type = MSG_VIDEO;
  frame.csid = 6;
  frame.tsMs = timestampMs;
  if (h265) {
    // Enhanced-RTMP ex-header: isExHeader | FrameType | PacketType=CodedFrames.
    // Inter frames must carry FrameType=2, and CodedFrames (unlike
    // CodedFramesX) is followed by a 24-bit composition time, same as AVC.
    frame.payload.push_back(
        static_cast<uint8_t>(0x80 | (hasKey ? 0x10 : 0x20) | 0x01));
    frame.payload.push_back('h');
    frame.payload.push_back('v');
    frame.payload.push_back('c');
    frame.payload.push_back('1');
    Put3(frame.payload, 0);
  } else {
    frame.payload.push_back(static_cast<uint8_t>(hasKey ? 0x17 : 0x27));
    frame.payload.push_back(0x01);
    Put3(frame.payload, 0);
  }
  frame.payload.insert(frame.payload.end(), avcc.begin(), avcc.end());
  EnqueueVideo(std::move(frame), std::move(sequence), hasKey, configVersion);
}

void RtmpPublisher::SendAdtsPacket(const uint8_t* data, size_t size,
                                   int64_t tsUs) {
  if (!running_.load() || cancelRequested_.load() || data == nullptr ||
      size < 8) {
    return;
  }
  if (data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) return;

  const int profileBits = (data[2] >> 6) & 0x03;
  const int sfIndex = (data[2] >> 2) & 0x0F;
  const int channels = ((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03);
  const int frameLen = ((data[3] & 0x03) << 11) | (data[4] << 3) |
                       ((data[5] >> 5) & 0x07);
  const int headerLen = (data[1] & 0x01) ? 7 : 9;
  if (sfIndex >= 13 || channels <= 0 || frameLen < headerLen + 1 ||
      frameLen > 8192 || static_cast<size_t>(frameLen) > size) {
    return;
  }
  const size_t aacLen =
      static_cast<size_t>(frameLen) - static_cast<size_t>(headerLen);

  uint32_t timestampMs = 0;
  if (!TimestampToMs(tsUs, false, &timestampMs)) return;

  OutMsg sequence;
  sequence.type = MSG_AUDIO;
  sequence.csid = 4;
  sequence.tsMs = 0;
  sequence.payload = AacSequenceHeader(profileBits, sfIndex, channels);

  OutMsg frame;
  frame.type = MSG_AUDIO;
  frame.csid = 4;
  frame.tsMs = timestampMs;
  frame.payload.push_back(0xAF);
  frame.payload.push_back(0x01);
  frame.payload.insert(frame.payload.end(), data + headerLen,
                       data + headerLen + aacLen);

  const uint32_t configKey =
      (static_cast<uint32_t>(profileBits) << 16) |
      (static_cast<uint32_t>(sfIndex) << 8) |
      static_cast<uint32_t>(channels);
  if (EnqueueAudio(std::move(frame), std::move(sequence), configKey)) {
    std::lock_guard<std::mutex> lk(paramMu_);
    static const int kSampleRates[13] = {96000, 88200, 64000, 48000, 44100,
                                         32000, 24000, 22050, 16000, 12000,
                                         11025, 8000, 7350};
    audioRate_ = kSampleRates[sfIndex];
    audioChannels_ = channels;
  }
}

void RtmpPublisher::Cancel() {
  cancelRequested_.store(true);
  running_.store(false);
  queueCv_.notify_all();
  InterruptSocket();
}

void RtmpPublisher::Stop() {
  // Serialize complete stop/join cleanup so repeated concurrent Stop calls never
  // wait on a lifecycle transition owned by a caller blocked behind this one.
  std::lock_guard<std::mutex> stopLock(stopMu_);
  std::thread sender;
  int fd = -1;
  {
    std::unique_lock<std::mutex> lk(lifecycleMu_);
    if (lifecycleState_ == LifecycleState::kStopping) return;
    if (lifecycleState_ == LifecycleState::kIdle) {
      running_.store(false);
      // Before the first Start this latches cancellation for pending async work.
      // After a completed/failed Start it is an ordinary idempotent Stop and
      // keeps the publisher reusable.
      if (!startAttempted_) cancelRequested_.store(true);
      ResetSessionState();
      std::lock_guard<std::mutex> errLock(errMu_);
      if (!startAttempted_ && lastError_.empty()) {
        lastError_ = "startup cancelled before Start";
      }
      onError_ = nullptr;
      return;
    }
    lifecycleState_ = LifecycleState::kStopping;
    cancelRequested_.store(true);
    running_.store(false);
    queueCv_.notify_all();
    InterruptSocket();
    while (startWorkerActive_) lifecycleCv_.wait(lk);
    if (senderThread_.joinable()) sender = std::move(senderThread_);
    {
      std::lock_guard<std::mutex> sockLock(sockMu_);
      fd = sock_;
    }
  }

  if (sender.joinable()) {
    if (sender.get_id() == std::this_thread::get_id()) {
      sender.detach();
    } else {
      sender.join();
    }
  }
  if (fd >= 0) CloseSocketIf(fd);
  ResetSessionState();
  {
    std::lock_guard<std::mutex> lk(errMu_);
    onError_ = nullptr;
  }
  // Active-session Stop is fully restartable. The pre-Start cancellation latch
  // is only retained by the idle branch above.
  cancelRequested_.store(false);
  errorReported_.store(false);
  {
    std::lock_guard<std::mutex> lk(lifecycleMu_);
    lifecycleState_ = LifecycleState::kIdle;
  }
  lifecycleCv_.notify_all();
}

}  // namespace ipcam
