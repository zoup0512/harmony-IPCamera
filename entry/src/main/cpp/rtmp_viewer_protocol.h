#pragma once

#include "rtmp_chunk_parser.h"
#include "rtmp_command.h"
#include "rtsp_client_protocol.h"  // Admission on the exact bytes sent to AVDecoder.
#include <arpa/inet.h>
#include <charconv>

namespace rtmpview {
using Bytes = std::vector<uint8_t>;
enum Status {
  Connecting = 0,
  Playing = 1,
  Network = -1,
  Rejected = -2,
  VideoCodec = -3,
  Transport = -4,
  Protocol = -5,
  DecoderError = -6,
  Timeout = -7,
  EnhancedVideo = -8,
  Ended = -9,
  Stopped = -10,
  Busy = -11,
  EnhancedHevc = -12,
  NotFound = -13,
  Aggregate = -14
};
constexpr size_t MaxVideo = 2u << 20, MaxAssembly = 4u << 20;
struct Endpoint {
  std::string host, port, app, stream, tcUrl;
};
inline Endpoint endpoint(const std::string& url) {
  if (url.size() > 4096 || url.find_first_of("#\\") != url.npos ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
    throw Protocol;
  if (url.rfind("rtmp://", 0) != 0) throw Transport;
  auto slash = url.find('/', 7), split = slash == url.npos ? url.npos : url.find('/', slash + 1);
  if (split == url.npos || split == slash + 1 || split + 1 == url.size() || url[split + 1] == '/' ||
      url[split + 1] == '?' || url.find('?') < split)
    throw Protocol;
  auto hp = url.substr(7, slash - 7);
  Endpoint e{"", "1935", url.substr(slash + 1, split - slash - 1), url.substr(split + 1),
             url.substr(0, split)};
  if (hp.empty()) throw Protocol;
  size_t end;
  if (hp[0] == '[') {
    end = hp.find(']');
    if (end == hp.npos) throw Protocol;
    e.host = hp.substr(1, end - 1);
    ++end;
    in6_addr addr{};
    if (inet_pton(AF_INET6, e.host.c_str(), &addr) != 1) throw Protocol;
  } else {
    end = hp.find(':');
    if (end == hp.npos) end = hp.size();
    e.host = hp.substr(0, end);
    if (e.host.empty() || e.host.size() > 253 ||
        e.host.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-") !=
            e.host.npos)
      throw Protocol;
  }
  if (end < hp.size()) {
    if (hp[end] != ':') throw Protocol;
    e.port = hp.substr(end + 1);
    unsigned port = 0;
    auto r = std::from_chars(e.port.data(), e.port.data() + e.port.size(), port);
    if (r.ec != std::errc() || r.ptr != e.port.data() + e.port.size() || !port || port > 65535)
      throw Protocol;
    e.port = std::to_string(port);
  }
  return e;
}
inline uint32_t be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (p[1] << 8) | p[2]; }
inline uint32_t be32(const uint8_t* p) { return ipcam::RtmpChunkParser::Be32(p); }
inline void put(Bytes& b, uint32_t n, int width) {
  while (width--) b.push_back(uint8_t(n >> (width * 8)));
}
inline void key(Bytes& b, const std::string& s) {
  put(b, s.size(), 2);
  b.insert(b.end(), s.begin(), s.end());
}
inline void str(Bytes& b, const std::string& s) {
  b.push_back(2);
  key(b, s);
}
inline void num(Bytes& b, double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, 8);
  b.push_back(0);
  for (int i = 7; i >= 0; --i) b.push_back(uint8_t(bits >> (i * 8)));
}
inline Bytes command(const std::string& name, double tx) {
  Bytes b;
  str(b, name);
  num(b, tx);
  return b;
}
inline Bytes connectCommand(const Endpoint& e) {
  auto b = command("connect", 1);
  b.push_back(3);
  key(b, "app");
  str(b, e.app);
  key(b, "tcUrl");
  str(b, e.tcUrl);
  key(b, "videoCodecs");
  num(b, 128);
  key(b, "audioCodecs");
  num(b, 1024);
  key(b, "objectEncoding");
  num(b, 0);
  b.insert(b.end(), {0, 0, 9});
  return b;
}
// The client keeps its outbound chunk size at the protocol default (128).
inline Bytes wire(uint8_t type, uint8_t csid, uint32_t id, const Bytes& body, uint32_t ts = 0) {
  Bytes b{csid};
  put(b, std::min(ts, 0xffffffu), 3);
  put(b, body.size(), 3);
  b.push_back(type);
  for (unsigned i = 0; i < 4; ++i) b.push_back(uint8_t(id >> (8 * i)));
  for (size_t off = 0;; off += 128) {
    if (off) b.push_back(0xc0 | csid);
    if (ts >= 0xffffff) put(b, ts, 4);
    size_t n = std::min<size_t>(128, body.size() - off);
    b.insert(b.end(), body.begin() + off, body.begin() + off + n);
    if (off + n == body.size()) return b;
  }
}

// Publisher's incremental parser discards media and timestamps. This reader keeps its
// fmt0-3/abort rules, but assembles media too, with a shared budget across all CSIDs.
// Read must fill exactly n bytes or throw; header state cannot be committed twice.
class Chunks {
  struct Peer {
    uint32_t id = 0, ts = 0, delta = 0, extValue = 0, len = 0, used = 0;
    uint8_t type = 0;
    bool ext = false;
    Bytes data;
  };
  std::map<uint32_t, Peer> peers;
  uint32_t chunk = 128;
  size_t reserved = 0;
  void release(Peer& p) {
    reserved -= p.data.size();
    Bytes{}.swap(p.data);
    p.used = 0;
  }

 public:
  template <class Read, class Visit>
  void next(Read read, Visit visit) {
    uint8_t h[11];
    read(h, 1);
    unsigned fmt = h[0] >> 6, cs = h[0] & 63;
    if (cs < 2) {
      unsigned n = cs + 1;
      read(h, n);
      cs = 64 + h[0] + (n == 2 ? 256 * h[1] : 0);
    }
    auto it = peers.find(cs);
    if (it == peers.end() && (fmt || peers.size() >= 32)) throw Protocol;
    auto& p = peers[cs];
    bool cont = p.used != 0;
    if (cont && fmt != 3) throw Protocol;
    if (fmt < 3) {
      read(h, fmt == 0 ? 11 : fmt == 1 ? 7 : 3);
      uint32_t stamp = be24(h);
      if (fmt <= 1) {
        p.len = be24(h + 3);
        p.type = h[6];
      }
      if (!fmt) p.id = h[7] | (uint32_t(h[8]) << 8) | (uint32_t(h[9]) << 16) | (uint32_t(h[10]) << 24);
      p.ext = stamp == 0xffffff;
      if (p.ext) {
        read(h, 4);
        stamp = be32(h);
        p.extValue = stamp;
      }
      p.delta = stamp;
      p.ts = fmt ? p.ts + stamp : stamp;
    } else {
      if (p.ext) {
        read(h, 4);
        if (be32(h) != p.extValue) throw Protocol;
      }
      if (!cont) p.ts += p.delta;
    }
    if (!cont) {
      if (p.type >= 1 && p.type <= 6 && (cs != 2 || p.id != 0)) throw Protocol;
      if (((p.type == 1 || p.type == 2 || p.type == 3 || p.type == 5) && p.len != 4) ||
          (p.type == 6 && p.len != 5) || (p.type == 4 && (p.len < 2 || p.len > 64)))
        throw Protocol;
      if (p.type == 22) throw Aggregate;
      size_t cap = p.type == 9 ? MaxVideo : p.type == 8 ? 65536 : 65536;
      if (p.len > cap || p.len > MaxAssembly - reserved) throw Protocol;
      p.data.resize(p.len);
      reserved += p.len;
    }
    size_t n = std::min(chunk, p.len - p.used);
    if (n) read(p.data.data() + p.used, n);
    p.used += n;
    if (p.used != p.len) return;
    if (p.type == 1) {
      chunk = be32(p.data.data());
      if (!chunk || chunk > 65536) throw Protocol;
    }
    if (p.type == 2) {
      auto a = peers.find(be32(p.data.data()));
      if (a != peers.end() && &a->second != &p) release(a->second);
    }
    visit(p.type, p.id, p.ts, p.data);
    release(p);
  }
};

struct Cursor {
  const Bytes& b;
  size_t i;
  uint32_t take(unsigned n) {
    if (n > b.size() - i) throw Protocol;
    uint32_t v = 0;
    while (n--) v = (v << 8) | b[i++];
    return v;
  }
  unsigned nal(Bytes& au, size_t n) {
    if (!n || n > b.size() - i || au.size() + 4 + n > MaxVideo || (b[i] & 128)) throw Protocol;
    unsigned type = b[i] & 31;
    if (n < 2 && (type == 1 || type == 5 || type == 7 || type == 8)) throw Protocol;
    if ((type == 7 || type == 8) && n > 4096) throw VideoCodec;
    // A length-prefixed NAL must not smuggle a second Annex-B NAL past type/IDR checks.
    for (size_t j = i + 2; j < i + n; ++j)
      if (b[j] == 1 && !b[j - 1] && !b[j - 2]) throw Protocol;
    au.insert(au.end(), {0, 0, 0, 1});
    au.insert(au.end(), b.begin() + i, b.begin() + i + n);
    i += n;
    return type;
  }
};
class Avc {
  liveavc::AvcBudget budget;
  unsigned width = 0;
  bool needIdr = true;
  // RTMP uses 32-bit millisecond DTS; do not sign-extend the absolute timestamp.
  bool timed = false;
  uint32_t previous = 0;
  int64_t ms = 0;

 public:
  template <class Push>
  void packet(const Bytes& b, uint32_t ts, Push push) {
    if (b.empty()) throw Protocol;
    if (b[0] & 128) {
      if (b.size() < 5) throw Protocol;
      if (!memcmp(b.data() + 1, "hvc1", 4) || !memcmp(b.data() + 1, "hev1", 4)) throw EnhancedHevc;
      throw EnhancedVideo;
    }
    if ((b[0] & 15) != 7) throw VideoCodec;
    if (b.size() < 5 || (b[0] >> 4 != 1 && b[0] >> 4 != 2)) throw Protocol;
    Bytes au;
    Cursor c{b, 5};
    if (b[1] == 0) {
      if (b[0] != 0x17 || be24(b.data() + 2) || c.take(1) != 1) throw Protocol;
      unsigned profile = c.take(1);
      c.take(2);
      unsigned field = c.take(1);
      if ((field & 252) != 252 || (field & 3) == 2) throw Protocol;
      width = (field & 3) + 1;
      unsigned sps = c.take(1);
      if ((sps >> 5) != 7 || !(sps & 31)) throw Protocol;
      for (unsigned j = 0; j < (sps & 31); ++j)
        if (c.nal(au, c.take(2)) != 7) throw Protocol;
      unsigned pps = c.take(1);
      if (!pps || pps > 32) throw Protocol;
      for (unsigned j = 0; j < pps; ++j)
        if (c.nal(au, c.take(2)) != 8) throw Protocol;
      if (c.i != b.size() &&
          (profile != 100 || c.take(1) != 0xfd || c.take(1) != 0xf8 || c.take(1) != 0xf8 || c.take(1) != 0))
        throw VideoCodec;
      if (c.i != b.size()) throw Protocol;
      if (budget.inspect(au, true) != liveavc::Admission::Ready) throw VideoCodec;
      needIdr = true;
      push(au, 0, true);
      return;
    }
    if (b[1] == 2) {
      if (b.size() != 5) throw Protocol;
      throw Ended;
    }
    if (b[1] != 1 || !width || b.size() == 5) throw Protocol;
    bool idr = false, slice = false;
    unsigned count = 0;
    while (c.i < b.size()) {
      if (++count > 256) throw Protocol;
      unsigned type = c.nal(au, c.take(width));
      idr |= type == 5;
      slice |= type == 1 || type == 5;
    }
    if (budget.inspect(au, false) != liveavc::Admission::Ready) throw VideoCodec;
    if (!slice) throw Protocol;
    if (needIdr && !idr) return;
    needIdr = false;
    if (timed) {
      uint32_t delta = ts - previous;  // forward modulo wrap, not an arbitrary signed jump
      if (delta > INT32_MAX || ms > INT64_MAX / 1000 - 0x800000 - delta) throw Protocol;
      ms += delta;
    } else {
      ms = ts;
      timed = true;
    }
    previous = ts;
    int32_t cts = int32_t(be24(b.data() + 2));
    if (cts & 0x800000) cts -= 0x1000000;
    push(au, (ms + cts) * 1000, false);
  }
};

// AAC is optional. Invalid/unsupported audio never enters VoicePlayer or breaks video.
// Only an exact two-byte AAC-LC ASC, mono/stereo and one raw AAC AU per tag.
class Aac {
  unsigned sf = 0, channels = 0;

 public:
  enum Result { Unsupported, Config, Frame };
  Result packet(const Bytes& b, Bytes& adts) {
    if (b.size() < 3 || (b[0] >> 4) != 10) {
      channels = 0;
      return Unsupported;
    }
    if (b[1] == 0) {
      channels = 0;
      if (b.size() != 4 || b[2] >> 3 != 2 || (b[3] & 7)) return Unsupported;
      sf = ((b[2] & 7) << 1) | (b[3] >> 7);
      unsigned ch = (b[3] >> 3) & 15;
      if (sf >= 13 || (ch != 1 && ch != 2)) return Unsupported;
      channels = ch;
      return Config;
    }
    size_t n = b.size() - 2 + 7;
    if (b[1] != 1 || !channels || n > 8191) {
      channels = 0;
      return Unsupported;
    }
    adts = {0xff,
            0xf1,
            uint8_t(0x40 | (sf << 2)),
            uint8_t((channels << 6) | (n >> 11)),
            uint8_t(n >> 3),
            uint8_t((n << 5) | 31),
            0xfc};
    adts.insert(adts.end(), b.begin() + 2, b.end());
    return Frame;
  }
};
}  // namespace rtmpview
