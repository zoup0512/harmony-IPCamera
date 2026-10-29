#ifndef IPCAMERA_RTMP_CHUNK_PARSER_H
#define IPCAMERA_RTMP_CHUNK_PARSER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ipcam {

// Incremental parser for the server->client half of an RTMP chunk stream.
// Pure (no sockets/logging) so it can be exercised by host tests. It tracks
// per-chunk-stream header state, applies SetChunkSize, and reassembles AMF0
// command messages to surface connect/publish rejections.
class RtmpChunkParser {
 public:
  enum class Status { kOk, kInvalidChunkSize, kRejected };

  explicit RtmpChunkParser(uint32_t maxChunkSize) : maxChunkSize_(maxChunkSize) {}

  // Consumes every complete chunk in [data, data+size) and returns the number
  // of bytes consumed; a trailing partial chunk is left for the next call.
  // Stops early (consumed bytes still reported) on a non-kOk status.
  size_t Consume(const uint8_t* data, size_t size, Status* status,
                 std::string* detail) {
    *status = Status::kOk;
    size_t pos = 0;
    while (pos < size) {
      const size_t chunkStart = pos;
      const uint8_t fmt = data[pos] >> 6;
      uint32_t csid = data[pos] & 0x3F;
      size_t basicHeader = 1;
      if (csid == 0) {
        if (pos + 2 > size) break;
        csid = 64 + data[pos + 1];
        basicHeader = 2;
      } else if (csid == 1) {
        if (pos + 3 > size) break;
        csid = 64 + data[pos + 1] + data[pos + 2] * 256;
        basicHeader = 3;
      }

      PeerState& st = peer_[csid];
      const size_t messageHeader =
          (fmt == 0) ? 11 : (fmt == 1 ? 7 : (fmt == 2 ? 3 : 0));
      if (pos + basicHeader + messageHeader > size) break;

      const uint8_t* h = data + pos + basicHeader;
      // fmt 3 either continues the in-progress message or starts a new one
      // that reuses every header field (including the declared length) of
      // the previous message on this chunk stream; fmt 2 starts a new
      // message with only a new timestamp delta.
      const bool newMessage = fmt != 3 || !st.inMsg;
      uint32_t newLength = newMessage ? st.msgLen : st.remaining;
      uint8_t newType = st.type;
      bool extTs = st.extTs;
      if (fmt <= 2) {
        const uint32_t ts = (static_cast<uint32_t>(h[0]) << 16) |
                            (static_cast<uint32_t>(h[1]) << 8) | h[2];
        extTs = ts == 0xFFFFFF;
      }
      if (fmt <= 1) {
        newLength = (static_cast<uint32_t>(h[3]) << 16) |
                    (static_cast<uint32_t>(h[4]) << 8) | h[5];
        newType = h[6];
      }
      // A 4-byte extended timestamp follows the header whenever the last
      // full timestamp field on this chunk stream was 0xFFFFFF — including
      // on fmt-3 chunks, which carry no timestamp field of their own.
      const size_t extBytes = extTs ? 4 : 0;
      const size_t payloadPos = pos + basicHeader + messageHeader + extBytes;
      if (payloadPos > size) break;
      const size_t piece = std::min<size_t>(chunkSize_, newLength);
      if (payloadPos + piece > size) {
        pos = chunkStart;
        break;
      }

      if (newMessage) {
        st.msgLen = newLength;
        st.msgBuf.clear();
      }
      st.remaining = newLength;
      st.type = newType;
      st.extTs = extTs;
      st.inMsg = st.remaining > 0;
      if (st.type == kCommandType && st.msgBuf.size() < kMaxCommandBytes) {
        st.msgBuf.insert(st.msgBuf.end(), data + payloadPos,
                         data + payloadPos + piece);
      }
      if (st.type == kSetChunkSizeType && newMessage && newLength == 4 &&
          piece == 4) {
        const uint32_t value =
            ((static_cast<uint32_t>(data[payloadPos]) << 24) |
             (static_cast<uint32_t>(data[payloadPos + 1]) << 16) |
             (static_cast<uint32_t>(data[payloadPos + 2]) << 8) |
             static_cast<uint32_t>(data[payloadPos + 3])) & 0x7FFFFFFF;
        if (value == 0 || value > maxChunkSize_) {
          *status = Status::kInvalidChunkSize;
          return payloadPos + piece;
        }
        chunkSize_ = value;
      }
      pos = payloadPos + piece;
      st.remaining -= static_cast<uint32_t>(piece);
      if (st.remaining == 0) {
        st.inMsg = false;
        const char* reject = CommandRejection(st);
        if (reject != nullptr) {
          *status = Status::kRejected;
          if (detail != nullptr) *detail = reject;
          return pos;
        }
      }
    }
    return pos;
  }

  uint32_t chunkSize() const { return chunkSize_; }

 private:
  static constexpr uint8_t kSetChunkSizeType = 1;
  static constexpr uint8_t kCommandType = 20;  // AMF0 command
  static constexpr size_t kMaxCommandBytes = 65536;

  struct PeerState {
    uint32_t remaining = 0;  // bytes left in the in-progress message
    uint32_t msgLen = 0;     // declared length of the current/last message
    uint8_t type = 0;
    bool extTs = false;      // last full timestamp field was 0xFFFFFF
    bool inMsg = false;
    std::vector<uint8_t> msgBuf;  // reassembled AMF0 command payload
  };

  // The publisher only sends connect/createStream/publish, all mandatory, so
  // any _error reply or rejection status is fatal for the session.
  static const char* CommandRejection(const PeerState& st) {
    if (st.type != kCommandType || st.msgBuf.empty()) return nullptr;
    const std::string s(reinterpret_cast<const char*>(st.msgBuf.data()),
                        st.msgBuf.size());
    static const char* const kReject[] = {
        "NetStream.Publish.Rejected", "NetStream.Publish.Failed",
        "NetStream.Publish.Denied", "NetStream.Publish.BadName",
        "NetConnection.Connect.Rejected", "_error"};
    for (const char* marker : kReject) {
      if (s.find(marker) != std::string::npos) return marker;
    }
    return nullptr;
  }

  uint32_t maxChunkSize_;
  uint32_t chunkSize_ = 128;
  std::map<uint32_t, PeerState> peer_;
};

}  // namespace ipcam

#endif  // IPCAMERA_RTMP_CHUNK_PARSER_H
