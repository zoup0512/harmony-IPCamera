#ifndef IPCAMERA_RTMP_CHUNK_PARSER_H
#define IPCAMERA_RTMP_CHUNK_PARSER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace ipcam {
// Incremental server chunk decoder. Never commit a header or body twice when
// TCP splits a chunk. Keep negotiated sizes and fmt 1/2/3 history across startup.
class RtmpChunkParser {
 public:
  enum class Status { kOk, kInvalidChunkSize, kMalformed, kStopped };
  explicit RtmpChunkParser(uint32_t maxChunkSize) : maxChunkSize_(maxChunkSize) {}
  uint32_t chunkSize() const { return chunkSize_; }
  static uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  // Visit returns false to stop at a complete message boundary.
  template<class Visit>
  size_t Consume(const uint8_t* data, size_t size, Status* status, Visit visit) {
    *status = Status::kOk;
    size_t pos = 0;
    while (pos < size) {
      const uint8_t fmt = data[pos] >> 6;
      uint32_t csid = data[pos] & 63;
      const size_t basic = csid == 0 ? 2 : csid == 1 ? 3 : 1;
      const size_t header = basic + (fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0);
      if (size - pos < header) break;
      if (csid < 2) csid = 64 + data[pos + 1] + (basic == 3 ? 256 * data[pos + 2] : 0);
      const auto it = peer_.find(csid);
      const bool known = it != peer_.end();
      if ((!known && (fmt != 0 || peer_.size() >= 32)) ||
          (known && it->second.left && fmt != 3)) {
        *status = Status::kMalformed;
        break;
      }
      const Peer& prev = known ? it->second : empty_;
      const uint8_t* h = data + pos + basic;
      const uint32_t length = fmt <= 1 ? Be24(h + 3) : prev.len;
      const uint8_t type = fmt <= 1 ? h[6] : prev.type;
      const bool extended = fmt <= 2 ? Be24(h) == 0xffffff : prev.extended;
      const bool continuation = prev.left != 0;
      const uint32_t left = continuation ? prev.left : length;
      const uint32_t piece = std::min(chunkSize_, left);
      const bool keep = type == 17 || type == 20 || (type >= 1 && type <= 6);
      if ((keep && length > 65536) || ((type == 1 || type == 2 || type == 3 || type == 5) && length != 4) ||
          (type == 6 && length != 5)) {
        *status = Status::kMalformed;
        break;
      }
      const size_t body = pos + header + (extended ? 4 : 0);
      if (body > size || size - body < piece) break;
      Peer& st = peer_[csid];
      if (!continuation) st.bytes.clear();
      st.len = length;
      st.left = left - piece;
      st.type = type;
      st.extended = extended;
      if (fmt == 0) st.id = h[7] | (uint32_t(h[8]) << 8) | (uint32_t(h[9]) << 16) | (uint32_t(h[10]) << 24);
      if (keep) st.bytes.insert(st.bytes.end(), data + body, data + body + piece);
      pos = body + piece;
      if (st.left || !keep) continue;
      if (type == 1) {
        uint32_t n = Be32(st.bytes.data());
        if (!n || n > maxChunkSize_) { *status = Status::kInvalidChunkSize; break; }
        chunkSize_ = n;
      } else if (type == 2) {
        auto aborted = peer_.find(Be32(st.bytes.data()));
        if (aborted != peer_.end()) aborted->second.left = 0;
      }
      if (!visit(type, st.id, st.bytes)) { *status = Status::kStopped; break; }
    }
    return pos;
  }
 private:
  struct Peer {
    uint32_t len = 0, left = 0, id = 0;
    uint8_t type = 0;
    bool extended = false;
    std::vector<uint8_t> bytes;
  };
  static uint32_t Be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
  const Peer empty_{};
  uint32_t maxChunkSize_, chunkSize_ = 128;
  std::map<uint32_t, Peer> peer_;
};
}  // namespace ipcam
#endif
