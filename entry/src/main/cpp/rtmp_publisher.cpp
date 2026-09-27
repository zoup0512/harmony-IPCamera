#include "rtmp_publisher.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include <hilog/log.h>

#define LOG_DOMAIN 0xC010
#define LOG_TAG "RtmpPublisher"

namespace ipcam {
namespace {

constexpr uint8_t MSG_SET_CHUNK_SIZE = 1;
constexpr uint8_t MSG_ACK = 3;
constexpr uint8_t MSG_WINDOW_ACK = 5;
constexpr uint8_t MSG_AUDIO = 8;
constexpr uint8_t MSG_VIDEO = 9;
constexpr uint8_t MSG_AMF0_DATA = 18;
constexpr uint8_t MSG_AMF0_COMMAND = 20;

constexpr uint32_t kOutChunkSize = 60000;

void LogErr(const char* what, int err) {
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "%{public}s err=%{public}d", what, err);
}

uint64_t NowUs() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
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

bool ReadAll(int fd, uint8_t* dst, size_t len) {
  size_t off = 0;
  while (off < len) {
    pollfd p{fd, POLLIN, 0};
    int pr = ::poll(&p, 1, 5000);
    if (pr <= 0) return false;
    ssize_t n = ::recv(fd, dst + off, len - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
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
  for (int i = 7; i >= 0; --i) v.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
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

std::vector<uint8_t> ConnectCommand(const std::string& app, const std::string& tcUrl) {
  std::vector<uint8_t> v;
  AmfString(v, "connect");
  AmfNumber(v, 1);
  v.push_back(0x03);  // object
  AmfKey(v, "app"); AmfString(v, app);
  AmfKey(v, "type"); AmfString(v, "private");
  AmfKey(v, "flashVer"); AmfString(v, "FMLE/3.0 (compatible; harmony-ipcamera)");
  AmfKey(v, "tcUrl"); AmfString(v, tcUrl);
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
  AmfNumber(v, 3);
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
  AmfKey(v, "width"); AmfNumber(v, width);
  AmfKey(v, "height"); AmfNumber(v, height);
  AmfKey(v, "framerate"); AmfNumber(v, 30);
  if (h265) {
    AmfKey(v, "videocodecid"); AmfString(v, "hvc1");
  } else {
    AmfKey(v, "videocodecid"); AmfNumber(v, 7);
  }
  AmfKey(v, "audiocodecid"); AmfNumber(v, 10);
  AmfKey(v, "audiosamplerate"); AmfNumber(v, audioRate);
  AmfKey(v, "audiochannels"); AmfNumber(v, audioChannels);
  AmfKey(v, "encoder"); AmfString(v, "harmony-ipcamera");
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

std::vector<uint8_t> AacSequenceHeader(int profileBits, int sfIndex, int channels) {
  int aot = profileBits + 1;
  std::vector<uint8_t> body;
  body.push_back(0xAF);  // AAC | 44kHz-rate-flag | 16bit | stereo-flag
  body.push_back(0x00);  // sequence header
  body.push_back(static_cast<uint8_t>((aot << 3) | (sfIndex >> 1)));
  body.push_back(static_cast<uint8_t>(((sfIndex & 1) << 7) | (channels << 3)));
  return body;
}


// HEVCDecoderConfigurationRecord for enhanced-RTMP sequence start. Built from
// the VPS/SPS/PPS NALUs; profile fields come straight from the SPS
// profile_tier_level bytes.
std::vector<uint8_t> HevcConfigurationRecord(const std::vector<uint8_t>& vps,
                                             const std::vector<uint8_t>& sps,
                                             const std::vector<uint8_t>& pps) {
  std::vector<uint8_t> r;
  r.push_back(0x01);  // configurationVersion
  if (sps.size() > 13) {
    r.push_back(sps[2]);                                   // space/tier/profile_idc
    r.insert(r.end(), sps.begin() + 3, sps.begin() + 7);   // compat flags
    r.insert(r.end(), sps.begin() + 7, sps.begin() + 13);  // constraint flags
    r.push_back(sps[13]);                                  // level_idc
  } else {
    r.insert(r.end(), 12, 0x01);
  }
  r.push_back(0xF0);  // reserved + min_spatial_segmentation_idc hi
  r.push_back(0x00);  // lo
  r.push_back(0xFC);  // reserved + parallelismType(2)=0
  r.push_back(0xFD);  // reserved(6) + chromaFormat(2)=1 (4:2:0)
  r.push_back(0xE0);  // reserved + bitDepthLumaMinus8=0
  r.push_back(0xE0);  // reserved + bitDepthChromaMinus8
  r.push_back(0x00);  // avgFrameRate hi
  r.push_back(0x00);  // lo
  r.push_back(0x1F);  // constantFrameRate(2)=0 numTemporalLayers(3)=1 temporalIdNested(1)=1 lengthSizeMinusOne(2)=3
  r.push_back(0x03);  // numOfArrays
  auto pushArray = [&r](uint8_t nalType, const std::vector<uint8_t>& nal) {
    // array_completeness(1)=1 reserved(1)=0 NAL_unit_type(6)
    r.push_back(static_cast<uint8_t>(0x80 | nalType));
    r.push_back(0x00);  // numNalus hi
    r.push_back(0x01);  // numNalus lo
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
  body.push_back(0x90);  // enhanced: keyframe, packetType sequence start
  body.push_back('h');
  body.push_back('v');
  body.push_back('c');
  body.push_back('1');
  auto rec = HevcConfigurationRecord(vps, sps, pps);
  body.insert(body.end(), rec.begin(), rec.end());
  return body;
}

}  // namespace

RtmpPublisher::~RtmpPublisher() { Stop(); }

void RtmpPublisher::Enqueue(OutMsg msg) {
  std::lock_guard<std::mutex> lk(queueMu_);
  if (queue_.size() > 600) {
    queue_.erase(queue_.begin(), queue_.begin() + 100);
  }
  queue_.push_back(std::move(msg));
}

bool RtmpPublisher::Start(const std::string& url, int width, int height, bool h265Hint,
                          ErrorSink onError) {
  if (running_.load()) return false;
  {
    std::lock_guard<std::mutex> lk(paramMu_);
    width_ = width;
    height_ = height;
    videoSeqSent_ = false;
    audioSeqSent_ = false;
    videoIsH265_ = h265Hint;
    vps_.clear();
    sps_.clear();
    pps_.clear();
    tsBaseUs_ = 0;
    tsBaseSet_ = false;
  }
  {
    std::lock_guard<std::mutex> lk(errMu_);
    onError_ = std::move(onError);
  }

  // rtmp://host[:port]/app/stream[?query]
  std::string rest = url;
  const std::string scheme = "rtmp://";
  if (rest.rfind(scheme, 0) != 0) {
    LogErr("url must start with rtmp://", 0);
    return false;
  }
  rest = rest.substr(scheme.size());
  size_t slash = rest.find('/');
  if (slash == std::string::npos) {
    LogErr("url missing /app/stream", 0);
    return false;
  }
  std::string hostPort = rest.substr(0, slash);
  std::string path = rest.substr(slash + 1);
  int port = 1935;
  std::string host = hostPort;
  size_t colon = hostPort.find(':');
  if (colon != std::string::npos) {
    host = hostPort.substr(0, colon);
    port = atoi(hostPort.c_str() + colon + 1);
  }
  if (path.empty()) {
    LogErr("url missing app/stream", 0);
    return false;
  }
  std::string app = path;
  std::string stream;
  size_t appEnd = path.find('/');
  if (appEnd != std::string::npos) {
    app = path.substr(0, appEnd);
    stream = path.substr(appEnd + 1);
  }

  char portStr[8];
  snprintf(portStr, sizeof(portStr), "%d", port);
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || res == nullptr) {
    LogErr("getaddrinfo failed", 0);
    return false;
  }
  sock_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock_ < 0) {
    freeaddrinfo(res);
    LogErr("socket failed", errno);
    return false;
  }
  int one = 1;
  setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (::connect(sock_, res->ai_addr, res->ai_addrlen) < 0) {
    freeaddrinfo(res);
    LogErr("connect failed", errno);
    ::close(sock_);
    sock_ = -1;
    return false;
  }
  freeaddrinfo(res);

  running_ = true;
  if (!ConnectAndPublish(host, port, app, stream)) {
    Stop();
    return false;
  }
  senderThread_ = std::thread(&RtmpPublisher::SenderLoop, this, sock_);
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "publishing to rtmp://%{public}s:%{public}d/%{public}s", host.c_str(), port,
               app.c_str());
  return true;
}

bool RtmpPublisher::ConnectAndPublish(const std::string& host, int port, const std::string& app,
                                      const std::string& stream) {
  // ---- handshake: C0+C1, read S0+S1+S2, echo C2 ----
  // C0 is 1 byte, C1 is exactly 1536 bytes (4 time + 4 zero + 1528 random).
  std::vector<uint8_t> c1;
  c1.push_back(0x03);
  c1.resize(1 + 1536);
  memset(c1.data() + 1, 0, 8);
  for (int i = 8; i < 1536; ++i) c1[1 + i] = static_cast<uint8_t>(rand() & 0xFF);
  if (!SendAll(sock_, c1.data(), c1.size())) return false;
  uint8_t s0s1s2[1 + 1536 + 1536];
  if (!ReadAll(sock_, s0s1s2, sizeof(s0s1s2))) return false;
  if (s0s1s2[0] != 0x03) return false;
  if (!SendAll(sock_, s0s1s2 + 1 + 1536, 1536)) return false;  // C2 = S2

  auto sendMsg = [&](uint8_t type, uint8_t csid, uint32_t streamId, uint32_t tsMs,
                     const std::vector<uint8_t>& payload) -> bool {
    std::vector<uint8_t> wire;
    bool ext = tsMs >= 0xFFFFFF;
    wire.push_back(csid & 0x3F);  // fmt=0
    Put3(wire, ext ? 0xFFFFFF : tsMs);
    Put3(wire, static_cast<uint32_t>(payload.size()));
    wire.push_back(type);
    Put4LE(wire, streamId);
    if (ext) Put4(wire, tsMs);
    wire.insert(wire.end(), payload.begin(), payload.end());
    return SendAll(sock_, wire.data(), wire.size());
  };

  // set outbound chunk size
  {
    std::vector<uint8_t> m;
    Put4(m, kOutChunkSize);
    if (!sendMsg(MSG_SET_CHUNK_SIZE, 2, 0, 0, m)) return false;
  }
  // connect
  if (!sendMsg(MSG_AMF0_COMMAND, 3, 0, 0,
               ConnectCommand(app, "rtmp://" + host + ":" + std::to_string(port) + "/" + app))) {
    return false;
  }
  // createStream
  if (!sendMsg(MSG_AMF0_COMMAND, 3, 0, 0, CreateStreamCommand())) return false;
  // publish on assumed stream id 1 (universal for RTMP servers)
  if (!sendMsg(MSG_AMF0_COMMAND, 8, 1, 0, PublishCommand(stream))) return false;
  // onMetaData
  int w = 0, h = 0, rate = 48000, ch = 1;
  {
    std::lock_guard<std::mutex> lk(paramMu_);
    w = width_;
    h = height_;
    rate = audioRate_;
    ch = audioChannels_;
  }
  bool h265 = false;
  {
    std::lock_guard<std::mutex> lk(paramMu_);
    h265 = videoIsH265_;
  }
  if (!sendMsg(MSG_AMF0_DATA, 5, 1, 0, MetaDataCommand(w, h, rate, ch, h265))) return false;
  return true;
}

void RtmpPublisher::SenderLoop(int fd) {
  uint64_t received = 0;
  uint64_t lastAck = 0;
  uint32_t windowAckSize = 2500000;
  uint32_t peerChunkSize = 128;

  struct PeerState {
    uint32_t tsDelta = 0;
    uint32_t length = 0;   // payload bytes still expected
    uint8_t type = 0;
    bool inMsg = false;
  };
  std::map<uint32_t, PeerState> peer;
  std::vector<uint8_t> rbuf;

  while (running_.load()) {
    // drain outbound queue
    std::vector<OutMsg> local;
    {
      std::lock_guard<std::mutex> lk(queueMu_);
      local.swap(queue_);
    }
    for (const OutMsg& m : local) {
      std::vector<uint8_t> wire;
      bool ext = m.tsMs >= 0xFFFFFF;
      wire.push_back(m.csid & 0x3F);
      Put3(wire, ext ? 0xFFFFFF : m.tsMs);
      Put3(wire, static_cast<uint32_t>(m.payload.size()));
      wire.push_back(m.type);
      Put4LE(wire, m.streamId);
      if (ext) Put4(wire, m.tsMs);
      size_t off = 0;
      bool first = true;
      while (first || off < m.payload.size()) {
        size_t n = std::min<size_t>(kOutChunkSize, m.payload.size() - off);
        if (!first) wire.push_back(0xC0 | (m.csid & 0x3F));
        wire.insert(wire.end(), m.payload.begin() + off, m.payload.begin() + off + n);
        off += n;
        first = false;
      }
      if (!SendAll(fd, wire.data(), wire.size())) {
        LogErr("send failed, stopping", errno);
        running_ = false;
        break;
      }
    }
    if (!running_.load()) break;

    // read server traffic: skip messages, react to SetChunkSize/WindowAck
    uint8_t tmp[16384];
    pollfd p{fd, POLLIN, 0};
    int pr = ::poll(&p, 1, 50);
    if (pr > 0) {
      ssize_t n = ::recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
      if (n == 0) {
        LogErr("server closed connection", 0);
        running_ = false;
        break;
      }
      if (n > 0) {
        rbuf.insert(rbuf.end(), tmp, tmp + n);
        received += static_cast<uint64_t>(n);
        size_t pos = 0;
        while (true) {
          if (pos >= rbuf.size()) break;
          uint8_t b0 = rbuf[pos];
          uint8_t fmt = b0 >> 6;
          uint32_t csid = b0 & 0x3F;
          size_t hdr = 1;
          if (csid == 0) {
            if (pos + 2 > rbuf.size()) break;
            csid = 64 + rbuf[pos + 1];
            hdr = 2;
          } else if (csid == 1) {
            if (pos + 3 > rbuf.size()) break;
            csid = 64 + rbuf[pos + 1] + rbuf[pos + 2] * 256;
            hdr = 3;
          }
          PeerState& st = peer[csid];
          size_t fixed = (fmt == 0) ? 11 : (fmt == 1 ? 7 : (fmt == 2 ? 3 : 0));
          if (fmt == 3 && !st.inMsg) {
            // continuation chunk with no state: nothing sensible to do; drop byte
            pos += hdr;
            continue;
          }
          bool extTs = false;
          if (fmt == 0 || fmt == 1 || fmt == 2) {
            if (pos + hdr + fixed > rbuf.size()) break;
            const uint8_t* h = rbuf.data() + pos + hdr;
            if (fmt == 0) {
              st.tsDelta = (h[0] << 16) | (h[1] << 8) | h[2];
              st.length = (h[3] << 16) | (h[4] << 8) | h[5];
              st.type = h[6];
            } else if (fmt == 1) {
              uint32_t oldLen = st.length;
              st.tsDelta = (h[0] << 16) | (h[1] << 8) | h[2];
              st.length = (h[3] << 16) | (h[4] << 8) | h[5];
              st.type = h[6];
              (void)oldLen;
            } else {
              st.tsDelta = (h[0] << 16) | (h[1] << 8) | h[2];
            }
            extTs = (st.tsDelta == 0xFFFFFF);
            if (extTs) fixed += 4;  // extended timestamp follows
            if (fmt == 0 || fmt == 1) {
              if (extTs && pos + hdr + 11 + 4 > rbuf.size()) break;
            }
            if (extTs && pos + hdr + fixed > rbuf.size()) break;
            if (extTs) st.tsDelta = (rbuf[pos + hdr + 11] << 24) | (rbuf[pos + hdr + 12] << 16) |
                                    (rbuf[pos + hdr + 13] << 8) | rbuf[pos + hdr + 14];
            st.inMsg = true;
          }
          pos += hdr + fixed;
          // consume payload in peer-chunk-size pieces
          size_t piece = std::min<size_t>(peerChunkSize, st.length);
          if (pos + piece > rbuf.size()) break;
          if (st.type == MSG_SET_CHUNK_SIZE && st.length == 4 && piece == 4) {
            peerChunkSize = (rbuf[pos] << 24) | (rbuf[pos + 1] << 16) | (rbuf[pos + 2] << 8) |
                            rbuf[pos + 3];
          }
          pos += piece;
          st.length -= static_cast<uint32_t>(piece);
          if (st.length == 0) st.inMsg = false;
        }
        if (pos > 0) rbuf.erase(rbuf.begin(), rbuf.begin() + static_cast<long>(pos));
        if (received - lastAck > windowAckSize) {
          lastAck = received;
          std::vector<uint8_t> ack;
          ack.push_back(2);  // csid 2, fmt0
          Put3(ack, 0);
          Put3(ack, 4);
          ack.push_back(MSG_ACK);
          Put4(ack, 0);
          Put4(ack, static_cast<uint32_t>(received & 0xFFFFFFFF));
          if (!SendAll(fd, ack.data(), ack.size())) {
            LogErr("ack send failed", errno);
            running_ = false;
          }
        }
      }
    }
  }
  ::shutdown(fd, SHUT_RDWR);
}

void RtmpPublisher::SendH264Packet(const uint8_t* data, size_t size, uint64_t tsUs) {
  if (!running_.load() || data == nullptr || size < 5) return;
  std::vector<NalView> nals = SplitAnnexB(data, size);
  std::vector<uint8_t> avcc;  // 4-byte length-prefixed slice NALUs
  bool hasSlice = false;
  bool hasKey = false;
  {
    std::lock_guard<std::mutex> lk(paramMu_);
    // Pre-scan for parameter-set anchors so the codec follows the actual
    // stream even when the Start hint was wrong.
    for (auto& nal : nals) {
      if (nal.size < 2) continue;
      uint8_t h264Type = nal.data[0] & 0x1F;
      uint8_t h265Type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
      bool h265Param = (h265Type == 32 || h265Type == 33 || h265Type == 34) &&
                       nal.data[1] == 0x01;
      if (h265Param && !videoIsH265_) {
        videoIsH265_ = true;
        videoSeqSent_ = false;
      } else if (h264Type == 7 && videoIsH265_) {
        videoIsH265_ = false;
        videoSeqSent_ = false;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lk(paramMu_);
    for (auto& nal : nals) {
      while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
      if (nal.size < 2) continue;
      if (videoIsH265_) {
        uint8_t h265Type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
        if (h265Type == 32) {
          vps_.assign(nal.data, nal.data + nal.size);
          videoSeqSent_ = false;
          continue;
        }
        if (h265Type == 33) {
          sps_.assign(nal.data, nal.data + nal.size);
          videoSeqSent_ = false;
          continue;
        }
        if (h265Type == 34) {
          pps_.assign(nal.data, nal.data + nal.size);
          videoSeqSent_ = false;
          continue;
        }
        if (h265Type == 19 || h265Type == 20) hasKey = true;
      } else {
        uint8_t h264Type = static_cast<uint8_t>(nal.data[0] & 0x1F);
        if (h264Type == 7) {
          sps_.assign(nal.data, nal.data + nal.size);
          videoSeqSent_ = false;
          continue;
        }
        if (h264Type == 8) {
          pps_.assign(nal.data, nal.data + nal.size);
          videoSeqSent_ = false;
          continue;
        }
        if (h264Type == 5) hasKey = true;
      }
      hasSlice = true;
      Put4(avcc, static_cast<uint32_t>(nal.size));
      avcc.insert(avcc.end(), nal.data, nal.data + nal.size);
    }
    if (!hasSlice || avcc.empty()) return;
    if (!videoSeqSent_) {
      if (videoIsH265_) {
        if (vps_.empty() || sps_.empty() || pps_.empty()) return;
        OutMsg m;
        m.type = MSG_VIDEO;
        m.csid = 6;
        m.streamId = 1;
        m.tsMs = 0;
        m.payload = HevcSequenceTag(vps_, sps_, pps_);
        Enqueue(std::move(m));
        videoSeqSent_ = true;
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                     "video sequence header sent (enhanced-rtmp hvc1)");
      } else {
        if (sps_.empty() || pps_.empty()) return;
        OutMsg m;
        m.type = MSG_VIDEO;
        m.csid = 6;
        m.streamId = 1;
        m.tsMs = 0;
        m.payload = AvcSequenceHeader(sps_, pps_);
        Enqueue(std::move(m));
        videoSeqSent_ = true;
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "video sequence header sent");
      }
    }
    if (!tsBaseSet_) {
      tsBaseUs_ = tsUs != 0 ? tsUs : NowUs();
      tsBaseSet_ = true;
    }
    uint64_t rel = (tsUs != 0 ? tsUs : NowUs()) - tsBaseUs_;
    OutMsg m;
    m.type = MSG_VIDEO;
    m.csid = 6;
    m.streamId = 1;
    m.tsMs = static_cast<uint32_t>(rel / 1000ULL);
    if (videoIsH265_) {
      // enhanced-RTMP: flags byte (bit7 enhanced, frame type, packetType 1) +
      // fourCC hvc1 + length-prefixed NALUs
      std::vector<uint8_t> body;
      body.push_back(static_cast<uint8_t>(0x80 | (hasKey ? 0x10 : 0x00) | 0x01));
      body.push_back('h');
      body.push_back('v');
      body.push_back('c');
      body.push_back('1');
      body.insert(body.end(), avcc.begin(), avcc.end());
      m.payload = std::move(body);
    } else {
      // FLV VideoTagHeader: frame type | codec 7 (AVC), AVCPacketType 1, CTS 0
      std::vector<uint8_t> body;
      body.push_back(static_cast<uint8_t>(hasKey ? 0x17 : 0x27));
      body.push_back(0x01);
      Put3(body, 0);
      body.insert(body.end(), avcc.begin(), avcc.end());
      m.payload = std::move(body);
    }
    Enqueue(std::move(m));
  }
}

void RtmpPublisher::SendAdtsPacket(const uint8_t* data, size_t size, uint64_t tsUs) {
  if (!running_.load() || data == nullptr || size < 8) return;
  if (data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) return;
  int profileBits = (data[2] >> 6) & 0x03;
  int sfIndex = (data[2] >> 2) & 0x0F;
  int channels = ((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03);
  int frameLen = ((data[3] & 0x03) << 11) | (data[4] << 3) | ((data[5] >> 5) & 0x07);
  int headerLen = (data[1] & 0x01) ? 7 : 9;
  if (frameLen < headerLen + 1 || frameLen > 8192) return;
  size_t aacLen = static_cast<size_t>(frameLen) - static_cast<size_t>(headerLen);
  if (data + frameLen > data + size) return;

  std::lock_guard<std::mutex> lk(paramMu_);
  if (!audioSeqSent_) {
    OutMsg m;
    m.type = MSG_AUDIO;
    m.csid = 4;
    m.streamId = 1;
    m.tsMs = 0;
    m.payload = AacSequenceHeader(profileBits, sfIndex, channels);
    Enqueue(std::move(m));
    audioSeqSent_ = true;
    audioRate_ = sfIndex < 13 ? (sfIndex == 3 ? 48000 : (sfIndex == 4 ? 44100 : 48000)) : 48000;
    audioChannels_ = channels;
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "audio sequence header sent");
  }
  if (!tsBaseSet_) {
    tsBaseUs_ = tsUs != 0 ? tsUs : NowUs();
    tsBaseSet_ = true;
  }
  uint64_t rel = (tsUs != 0 ? tsUs : NowUs()) - tsBaseUs_;
  OutMsg m;
  m.type = MSG_AUDIO;
  m.csid = 4;
  m.streamId = 1;
  m.tsMs = static_cast<uint32_t>(rel / 1000ULL);
  m.payload.push_back(0xAF);
  m.payload.push_back(0x01);  // raw AAC
  m.payload.insert(m.payload.end(), data + headerLen, data + headerLen + aacLen);
  Enqueue(std::move(m));
}

void RtmpPublisher::Stop() {
  // Must be safe to call from any state and repeatedly: the sender thread can
  // self-terminate (server closed) leaving running_ false while the std::thread
  // is still joinable — skipping the join here would make ~RtmpPublisher
  // terminate the process on a joinable thread.
  running_ = false;
  {
    std::lock_guard<std::mutex> lk(errMu_);
    onError_ = nullptr;
  }
  if (sock_ >= 0) {
    ::shutdown(sock_, SHUT_RDWR);
  }
  if (senderThread_.joinable()) {
    senderThread_.join();
  }
  if (sock_ >= 0) {
    ::close(sock_);
    sock_ = -1;
  }
  {
    std::lock_guard<std::mutex> lk(queueMu_);
    queue_.clear();
  }
}

}  // namespace ipcam
