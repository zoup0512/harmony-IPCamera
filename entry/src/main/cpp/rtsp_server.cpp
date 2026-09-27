#include "rtsp_server.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "RtspServer" 
#include <cstring>
#include <ctime>
#include <map>
#include <random>
#include <sstream>

namespace ipcam {
namespace {

constexpr int kVideoPt = 96;
constexpr int kAudioPt = 97;
constexpr size_t kRtpPayloadMax = 1400;

const int kAacSampleRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000,  7350,  0, 0, 0};

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
  if (out->frameLength < out->headerLen + 1 || out->frameLength > 8192) return false;
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
  static int8_t table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) table[i] = -1;
    for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(kBase64Chars[i])] = static_cast<int8_t>(i);
    init = true;
  }
  out->clear();
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\r' || c == '\n') continue;
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

uint32_t RandomU32() {
  static std::mt19937 rng(static_cast<uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count() ^ getpid()));
  return rng();
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

}  // namespace

struct RtspServer::Session : std::enable_shared_from_this<Session> {
  RtspServer* server = nullptr;
  int fd = -1;
  std::string peerIp;
  std::string id;
  std::atomic<bool> active{true};
  std::atomic<bool> playing{false};

  bool videoSetup = false;
  bool videoTcp = false;
  int videoChan = 0;
  int videoClientPort = 0;
  int videoRtpSock = -1;
  sockaddr_in videoAddr{};

  bool audioSetup = false;
  bool audioTcp = false;
  int audioChan = 2;
  int audioClientPort = 0;
  int audioRtpSock = -1;
  sockaddr_in audioAddr{};

  uint16_t videoSeq = 0;
  uint32_t videoSsrc = 0;
  uint16_t audioSeq = 0;
  uint32_t audioSsrc = 0;

  std::mutex ioMu;

  void Run();
  void CloseUdp();
  bool HandleRequestText(const std::string& text);
  bool CheckAuth(const std::map<std::string, std::string>& headers);
  bool SendResponse(const std::string& cseq, int code, const std::string& reason,
                    const std::string& extraHeaders, const std::string& body);
  void SendVideoFrame(const std::vector<NalView>& nals, uint32_t ts90k);
  void SendAudioFrame(const uint8_t* payload, size_t size, uint32_t tsSamples);
  bool DispatchPacket(bool tcp, int chan, int udpSock, const sockaddr_in& addr,
                      const uint8_t* pkt, size_t len);
};

RtspServer::~RtspServer() { Stop(); }

uint64_t RtspServer::NowUs() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

bool RtspServer::Start(const RtspConfig& config, RtspStatusCallback callback) {
  if (running_.load()) return false;
  config_ = config;
  {
    std::lock_guard<std::mutex> lk(cbMu_);
    callback_ = std::move(callback);
  }
  listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    Notify(RtspEvent::kServerError, 0, "socket() failed");
    return false;
  }
  int one = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(config_.port));
  if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(listenFd_, 8) < 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    Notify(RtspEvent::kServerError, 0, "bind/listen failed (port busy?)");
    return false;
  }
  running_ = true;
  acceptThread_ = std::thread(&RtspServer::AcceptLoop, this);
  return true;
}

void RtspServer::Stop() {
  if (!running_.exchange(false)) return;
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
  if (acceptThread_.joinable()) acceptThread_.join();
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    for (auto& s : sessions_) {
      s->active = false;
      if (s->fd >= 0) ::shutdown(s->fd, SHUT_RDWR);
    }
  }
  int waited = 0;
  while (ClientCount() > 0 && waited < 3000) {
    usleep(50 * 1000);
    waited += 50;
  }
  std::lock_guard<std::mutex> lk(cbMu_);
  callback_ = nullptr;
}

int RtspServer::ClientCount() {
  std::lock_guard<std::mutex> lk(sessionsMu_);
  return static_cast<int>(sessions_.size());
}

void RtspServer::AcceptLoop() {
  while (running_.load()) {
    pollfd p{listenFd_, POLLIN, 0};
    int pr = ::poll(&p, 1, 500);
    if (pr <= 0) continue;
    if (!running_.load()) break;
    sockaddr_in cli{};
    socklen_t clen = sizeof(cli);
    int cfd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&cli), &clen);
    if (cfd < 0) continue;
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    auto s = std::make_shared<Session>();
    s->server = this;
    s->fd = cfd;
    s->peerIp = ::inet_ntoa(cli.sin_addr);
    s->videoSsrc = RandomU32() | 1;
    s->audioSsrc = RandomU32() | 1;
    char sid[16];
    snprintf(sid, sizeof(sid), "%08x", RandomU32());
    s->id = sid;
    AddSession(s);
    Notify(RtspEvent::kClientConnected, ClientCount(), s->peerIp);
    std::thread([s]() { s->Run(); }).detach();
  }
}

void RtspServer::AddSession(const std::shared_ptr<Session>& session) {
  std::lock_guard<std::mutex> lk(sessionsMu_);
  sessions_.push_back(session);
}

void RtspServer::RemoveSession(Session* session) {
  std::lock_guard<std::mutex> lk(sessionsMu_);
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    if (it->get() == session) {
      sessions_.erase(it);
      break;
    }
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

void RtspServer::UpdateParamSets(const uint8_t* nal, size_t size) {
  if (nal == nullptr || size < 2) return;
  std::lock_guard<std::mutex> lk(paramMu_);
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

RtspServer::ParamSets RtspServer::GetParamSets() const {
  std::lock_guard<std::mutex> lk(paramMu_);
  return params_;
}

RtspServer::VideoParams RtspServer::GetVideoParams() const {
  std::lock_guard<std::mutex> lk(paramMu_);
  VideoParams vp;
  vp.h265 = (codec_ == VideoCodec::H265);
  vp.vps = params_.vps;
  vp.sps = params_.sps;
  vp.pps = params_.pps;
  vp.ready = codecDetected_ && !params_.sps.empty() &&
             (vp.h265 ? !params_.vps.empty() : !params_.pps.empty());
  return vp;
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

void RtspServer::PushH264(const uint8_t* data, size_t size, uint64_t tsUs) {
  if (!running_.load() || data == nullptr || size < 5) return;
  if (tsUs == 0) tsUs = NowUs();
  std::vector<NalView> nals = SplitAnnexB(data, size);
  if (nals.empty()) return;
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
  }
  for (auto& nal : nals) {
    while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
    if (nal.size >= 1) UpdateParamSets(nal.data, nal.size);
  }
  uint32_t ts90k = static_cast<uint32_t>((tsUs * 9ULL) / 100ULL);
  std::vector<std::shared_ptr<Session>> snapshot;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    snapshot = sessions_;
  }
  for (auto& s : snapshot) {
    if (s->active.load() && s->playing.load()) s->SendVideoFrame(nals, ts90k);
  }
}

void RtspServer::PushAdts(const uint8_t* data, size_t size, uint64_t tsUs) {
  if (!running_.load() || data == nullptr || size < 8) return;
  if (tsUs == 0) tsUs = NowUs();
  AdtsHeader h;
  if (!ParseAdts(data, size, &h)) return;
  UpdateAacInfo(h.profile, h.sfIndex, h.channels);
  size_t payloadLen = static_cast<size_t>(h.frameLength) - static_cast<size_t>(h.headerLen);
  if (payloadLen == 0 || payloadLen > kRtpPayloadMax) return;
  uint32_t tsSamples = static_cast<uint32_t>(
      (static_cast<uint64_t>(h.sampleRate) * tsUs) / 1000000ULL);
  std::vector<std::shared_ptr<Session>> snapshot;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    snapshot = sessions_;
  }
  for (auto& s : snapshot) {
    if (s->active.load() && s->playing.load() && s->audioSetup) {
      s->SendAudioFrame(data + h.headerLen, payloadLen, tsSamples);
    }
  }
}

std::string RtspServer::BuildSdp() const {
  ParamSets spsPps = GetParamSets();
  AacInfo aac = GetAacInfo();
  bool h265 = false;
  {
    std::lock_guard<std::mutex> lk(paramMu_);
    h265 = (codec_ == VideoCodec::H265);
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "BuildSdp codec=%{public}s vps=%{public}zu sps=%{public}zu pps=%{public}zu",
               h265 ? "H265" : "H264", spsPps.vps.size(), spsPps.sps.size(),
               spsPps.pps.size());
  std::ostringstream sdp;
  sdp << "v=0\r\n";
  sdp << "o=- " << NowUs() << " 0 IN IP4 0.0.0.0\r\n";
  sdp << "s=IPCamera Live\r\n";
  sdp << "i=harmony-ipcamera streaming\r\n";
  sdp << "t=0 0\r\n";
  sdp << "a=tool:harmony-ipcamera-poc\r\n";
  sdp << "a=type:broadcast\r\n";
  sdp << "a=control:*\r\n";
  sdp << "m=video 0 RTP/AVP " << kVideoPt << "\r\n";
  sdp << "c=IN IP4 0.0.0.0\r\n";
  if (h265) {
    sdp << "a=rtpmap:" << kVideoPt << " H265/90000\r\n";
    sdp << "a=fmtp:" << kVideoPt << " ";
    bool first = true;
    auto sprop = [&sdp, &first](const char* name, const std::vector<uint8_t>& v) {
      if (v.empty()) return;
      sdp << (first ? "" : ";") << name << "=" << Base64Encode(v.data(), v.size());
      first = false;
    };
    sprop("sprop-vps", spsPps.vps);
    sprop("sprop-sps", spsPps.sps);
    sprop("sprop-pps", spsPps.pps);
    sdp << "\r\n";
  } else {
    sdp << "a=rtpmap:" << kVideoPt << " H264/90000\r\n";
    sdp << "a=fmtp:" << kVideoPt << " packetization-mode=1";
    if (spsPps.sps.size() >= 4) {
      char plid[8];
      snprintf(plid, sizeof(plid), "%02x%02x%02x", spsPps.sps[1], spsPps.sps[2], spsPps.sps[3]);
      sdp << ";profile-level-id=" << plid;
      sdp << ";sprop-parameter-sets=" << Base64Encode(spsPps.sps.data(), spsPps.sps.size());
      if (!spsPps.pps.empty()) {
        sdp << "," << Base64Encode(spsPps.pps.data(), spsPps.pps.size());
      }
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
      sdp << "c=IN IP4 0.0.0.0\r\n";
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

void RtspServer::Session::Run() {
  std::string buf;
  char tmp[4096];
  while (active.load()) {
    pollfd p{fd, POLLIN, 0};
    int pr = ::poll(&p, 1, 500);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    if (!(p.revents & POLLIN)) continue;
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
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
      std::string req = buf.substr(0, hdrEnd);
      buf.erase(0, hdrEnd + 4);
      if (!HandleRequestText(req)) {
        fatal = true;
        break;
      }
    }
    if (fatal) break;
  }
  active = false;
  playing = false;
  CloseUdp();
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
  server->RemoveSession(this);
  server->Notify(RtspEvent::kClientDisconnected, server->ClientCount(), peerIp);
}

void RtspServer::Session::CloseUdp() {
  if (videoRtpSock >= 0) {
    ::close(videoRtpSock);
    videoRtpSock = -1;
  }
  if (audioRtpSock >= 0) {
    ::close(audioRtpSock);
    audioRtpSock = -1;
  }
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
    size_t vstart = colon + 1;
    while (vstart < line.size() && line[vstart] == ' ') vstart++;
    headers[key] = line.substr(vstart);
  }
  std::string cseq = headers.count("cseq") ? headers["cseq"] : "0";

  if (method == "OPTIONS") {
    return SendResponse(cseq, 200, "OK",
                        "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, "
                        "GET_PARAMETER, SET_PARAMETER\r\n",
                        "");
  }
  if (method == "DESCRIBE") {
    if (!CheckAuth(headers)) {
      return SendResponse(cseq, 401, "Unauthorized",
                          "WWW-Authenticate: Basic realm=\"IPCamera\"\r\n", "");
    }
    std::string sdp = server->BuildSdp();
    return SendResponse(cseq, 200, "OK",
                        "Content-Type: application/sdp\r\n"
                        "Content-Length: " + std::to_string(sdp.size()) + "\r\n",
                        sdp);
  }
  if (method == "SETUP") {
    if (!CheckAuth(headers)) {
      return SendResponse(cseq, 401, "Unauthorized",
                          "WWW-Authenticate: Basic realm=\"IPCamera\"\r\n", "");
    }
    bool audioTrack = url.find("trackID=1") != std::string::npos;
    std::string transport = headers.count("transport") ? headers["transport"] : "";
    bool tcp = transport.find("TCP") != std::string::npos;

    if (tcp) {
      int chan = audioTrack ? audioChan : videoChan;
      size_t pos = transport.find("interleaved=");
      if (pos != std::string::npos) chan = atoi(transport.c_str() + pos + 12);
      if (audioTrack) {
        audioSetup = true;
        audioTcp = true;
        audioChan = chan;
      } else {
        videoSetup = true;
        videoTcp = true;
        videoChan = chan;
      }
      char extra[160];
      snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;interleaved=%d-%d\r\n",
               chan, chan + 1);
      std::string extraHdr = std::string(extra) + "Session: " + id + "\r\n";
      return SendResponse(cseq, 200, "OK", extraHdr, "");
    }

    size_t pos = transport.find("client_port=");
    int p0 = 0;
    if (pos != std::string::npos) p0 = atoi(transport.c_str() + pos + 12);
    if (p0 <= 0) {
      return SendResponse(cseq, 461, "Unsupported transport", "", "");
    }
    sockaddr_in cliAddr{};
    cliAddr.sin_family = AF_INET;
    cliAddr.sin_port = htons(static_cast<uint16_t>(p0));
    if (::inet_pton(AF_INET, peerIp.c_str(), &cliAddr.sin_addr) != 1) {
      return SendResponse(cseq, 461, "Unsupported transport", "", "");
    }
    int rtpSock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (rtpSock < 0) return SendResponse(cseq, 500, "Internal server error", "", "");
    int one = 1;
    ::setsockopt(rtpSock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(rtpSock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        ::connect(rtpSock, reinterpret_cast<sockaddr*>(&cliAddr), sizeof(cliAddr)) < 0) {
      ::close(rtpSock);
      return SendResponse(cseq, 500, "Internal server error", "", "");
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    ::getsockname(rtpSock, reinterpret_cast<sockaddr*>(&bound), &blen);
    int serverPort = ntohs(bound.sin_port);
    if (audioTrack) {
      audioSetup = true;
      audioTcp = false;
      audioClientPort = p0;
      audioRtpSock = rtpSock;
      audioAddr = cliAddr;
    } else {
      videoSetup = true;
      videoTcp = false;
      videoClientPort = p0;
      videoRtpSock = rtpSock;
      videoAddr = cliAddr;
    }
    char extra[160];
    snprintf(extra, sizeof(extra),
             "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n",
             p0, p0 + 1, serverPort, serverPort + 1);
    std::string extraHdr = std::string(extra) + "Session: " + id + "\r\n";
    return SendResponse(cseq, 200, "OK", extraHdr, "");
  }
  if (method == "PLAY") {
    if (!CheckAuth(headers)) {
      return SendResponse(cseq, 401, "Unauthorized",
                          "WWW-Authenticate: Basic realm=\"IPCamera\"\r\n", "");
    }
    playing = true;
    server->Notify(RtspEvent::kClientPlaying, server->ClientCount(), peerIp);
    std::string extraHdr = "Session: " + id + "\r\n" +
        "RTP-Info: url=trackID=0;seq=" + std::to_string(videoSeq) + "\r\n";
    return SendResponse(cseq, 200, "OK", extraHdr, "");
  }
  if (method == "PAUSE") {
    playing = false;
    return SendResponse(cseq, 200, "OK", "Session: " + id + "\r\n", "");
  }
  if (method == "TEARDOWN") {
    SendResponse(cseq, 200, "OK", "Session: " + id + "\r\n", "");
    return false;
  }
  if (method == "GET_PARAMETER" || method == "SET_PARAMETER") {
    return SendResponse(cseq, 200, "OK", "", "");
  }
  return SendResponse(cseq, 501, "Not implemented", "", "");
}

bool RtspServer::Session::CheckAuth(const std::map<std::string, std::string>& headers) {
  RtspConfig cfg = server->config_;
  if (cfg.user.empty()) return true;
  auto it = headers.find("authorization");
  if (it == headers.end()) return false;
  const std::string& auth = it->second;
  const std::string prefix = "Basic ";
  if (auth.rfind(prefix, 0) != 0) return false;
  std::string decoded;
  if (!Base64Decode(auth.substr(prefix.size()), &decoded)) return false;
  std::string expect = cfg.user + ":" + cfg.password;
  return decoded == expect;
}

bool RtspServer::Session::SendResponse(const std::string& cseq, int code,
                                       const std::string& reason,
                                       const std::string& extraHeaders,
                                       const std::string& body) {
  std::ostringstream rsp;
  rsp << "RTSP/1.0 " << code << " " << reason << "\r\n";
  rsp << "CSeq: " << cseq << "\r\n";
  rsp << "Server: harmony-ipcamera\r\n";
  rsp << extraHeaders;
  if (!body.empty()) {
    rsp << "Content-Length: " << body.size() << "\r\n\r\n";
    rsp << body;
  } else {
    rsp << "\r\n";
  }
  const std::string s = rsp.str();
  return SendAll(fd, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void RtspServer::Session::SendVideoFrame(const std::vector<NalView>& nals, uint32_t ts90k) {
  bool h265 = false;
  {
    std::lock_guard<std::mutex> lk(server->paramMu_);
    h265 = (server->codec_ == RtspServer::VideoCodec::H265);
  }
  std::lock_guard<std::mutex> lk(ioMu);
  if (!active.load() || fd < 0 || !videoSetup) return;
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
    return DispatchPacket(videoTcp, videoChan, videoRtpSock, videoAddr, pkt, 12 + bodyLen);
  };
  for (size_t ni = 0; ni < nals.size(); ++ni) {
    const NalView& nal = nals[ni];
    if (nal.data == nullptr || nal.size == 0) continue;
    bool lastNal = (ni + 1 == nals.size());
    size_t nalHdrLen = h265 ? 2 : 1;
    if (nal.size <= nalHdrLen) continue;
    if (nal.size - nalHdrLen <= kRtpPayloadMax) {
      // single NAL unit packet
      if (!writeRtp(lastNal, nal.data, nal.size)) return;
    } else if (!h265) {
      // H.264 FU-A (type 28)
      uint8_t indicator = static_cast<uint8_t>((nal.data[0] & 0xE0) | 28);
      uint8_t nalType = static_cast<uint8_t>(nal.data[0] & 0x1F);
      size_t offset = 1;
      bool first = true;
      while (offset < nal.size) {
        size_t chunk = std::min(kRtpPayloadMax - 2, nal.size - offset);
        bool lastFrag = (offset + chunk == nal.size);
        uint8_t fuh = static_cast<uint8_t>((first ? 0x80 : 0x00) | (lastFrag ? 0x40 : 0x00) | nalType);
        uint8_t body[2 + kRtpPayloadMax];
        body[0] = indicator;
        body[1] = fuh;
        memcpy(body + 2, nal.data + offset, chunk);
        if (!writeRtp(lastFrag && lastNal, body, 2 + chunk)) return;
        offset += chunk;
        first = false;
      }
    } else {
      // H.265 FU (RFC 7798, type 49): 2-byte payload header + 1-byte FU header
      uint8_t ph[2];
      ph[0] = static_cast<uint8_t>((nal.data[0] & 0x81) | (49 << 1));
      ph[1] = nal.data[1];
      uint8_t nalType = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      size_t offset = 2;
      bool first = true;
      while (offset < nal.size) {
        size_t chunk = std::min(kRtpPayloadMax - 3, nal.size - offset);
        bool lastFrag = (offset + chunk == nal.size);
        uint8_t fuh = static_cast<uint8_t>((first ? 0x80 : 0x00) | (lastFrag ? 0x40 : 0x00) | nalType);
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

void RtspServer::Session::SendAudioFrame(const uint8_t* payload, size_t size,
                                         uint32_t tsSamples) {
  std::lock_guard<std::mutex> lk(ioMu);
  if (!active.load() || fd < 0 || !audioSetup || payload == nullptr || size == 0) return;
  uint8_t pkt[4 + 12 + 4 + kRtpPayloadMax];
  size_t off = 0;
  if (audioTcp) {
    uint16_t ilen = static_cast<uint16_t>(12 + 4 + size);
    pkt[0] = '$';
    pkt[1] = static_cast<uint8_t>(audioChan);
    pkt[2] = static_cast<uint8_t>(ilen >> 8);
    pkt[3] = static_cast<uint8_t>(ilen & 0xFF);
    off = 4;
  }
  uint8_t* rtp = pkt + off;
  rtp[0] = 0x80;
  rtp[1] = static_cast<uint8_t>(kAudioPt | 0x80);  // one AU per packet -> marker 1
  rtp[2] = static_cast<uint8_t>(audioSeq >> 8);
  rtp[3] = static_cast<uint8_t>(audioSeq & 0xFF);
  rtp[4] = static_cast<uint8_t>(tsSamples >> 24);
  rtp[5] = static_cast<uint8_t>((tsSamples >> 16) & 0xFF);
  rtp[6] = static_cast<uint8_t>((tsSamples >> 8) & 0xFF);
  rtp[7] = static_cast<uint8_t>(tsSamples & 0xFF);
  rtp[8] = static_cast<uint8_t>(audioSsrc >> 24);
  rtp[9] = static_cast<uint8_t>((audioSsrc >> 16) & 0xFF);
  rtp[10] = static_cast<uint8_t>((audioSsrc >> 8) & 0xFF);
  rtp[11] = static_cast<uint8_t>(audioSsrc & 0xFF);
  audioSeq++;
  // RFC 3640: AU-headers-length (16 bits) + AU header (13-bit size, 3-bit index)
  rtp[12] = 0x00;
  rtp[13] = 0x10;
  rtp[14] = static_cast<uint8_t>(size >> 5);
  rtp[15] = static_cast<uint8_t>((size & 0x1F) << 3);
  memcpy(rtp + 16, payload, size);
  size_t total = off + 12 + 4 + size;
  if (audioTcp) {
    if (!SendAll(fd, pkt, total)) active = false;
  } else {
    ssize_t n = ::send(audioRtpSock, pkt, total, MSG_NOSIGNAL);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) active = false;
  }
}

bool RtspServer::Session::DispatchPacket(bool tcp, int chan, int udpSock,
                                         const sockaddr_in& addr,
                                         const uint8_t* pkt, size_t len) {
  if (tcp) {
    uint8_t il[4] = {'$', static_cast<uint8_t>(chan),
                     static_cast<uint8_t>((len >> 8) & 0xFF),
                     static_cast<uint8_t>(len & 0xFF)};
    if (!SendAll(fd, il, 4)) {
      active = false;
      return false;
    }
    if (!SendAll(fd, pkt, len)) {
      active = false;
      return false;
    }
    return true;
  }
  if (udpSock < 0) return false;
  ssize_t n = ::send(udpSock, pkt, len, MSG_NOSIGNAL);
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    active = false;
    return false;
  }
  return true;
}

}  // namespace ipcam
