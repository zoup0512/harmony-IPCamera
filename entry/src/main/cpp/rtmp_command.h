#ifndef IPCAMERA_RTMP_COMMAND_H
#define IPCAMERA_RTMP_COMMAND_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ipcam {

// Only interpret positional arguments and direct properties of the info object.
// A description, stream key, or nested object must never be treated as a status.
struct RtmpCommand {
  std::string name, code, level;
  double transaction = 0, result = 0;
  bool numericResult = false;
};

class AmfReplyReader {
 public:
  explicit AmfReplyReader(const std::vector<uint8_t>& data) : data_(data) {}
  bool Read(RtmpCommand& c, bool amf3Envelope = false) {
    if (amf3Envelope && !Marker(0)) return false;  // AMF0-compatible command envelope
    if (!Marker(2) || !String(c.name)) return false;
    if (c.name != "_result" && c.name != "_error" && c.name != "onStatus") return true;
    if (!Marker(0) || !Number(c.transaction) || !Value(0)) return false;
    if (i_ == data_.size()) return true;
    if (Marker(0)) {
      c.numericResult = true;
      return Number(c.result);
    }
    return Value(0, &c);
  }
 private:
  const std::vector<uint8_t>& data_;
  size_t i_ = 0;
  bool Skip(size_t n) {
    if (n > data_.size() - i_) return false;
    i_ += n;
    return true;
  }
  bool Marker(uint8_t m) {
    if (i_ == data_.size() || data_[i_] != m) return false;
    ++i_;
    return true;
  }
  bool UInt(unsigned bytes, uint32_t& n) {
    n = 0;
    if (!Skip(bytes)) return false;
    for (size_t j = i_ - bytes; j < i_; ++j) n = (n << 8) | data_[j];
    return true;
  }
  bool String(std::string& s) {
    uint32_t n;
    if (!UInt(2, n) || !Skip(n)) return false;
    s.assign(reinterpret_cast<const char*>(data_.data() + i_ - n), n);
    return true;
  }
  bool Number(double& n) {
    if (!Skip(8)) return false;
    uint64_t bits = 0;
    for (size_t j = i_ - 8; j < i_; ++j) bits = (bits << 8) | data_[j];
    std::memcpy(&n, &bits, sizeof(n));
    return std::isfinite(n);
  }
  bool Value(unsigned depth, RtmpCommand* info = nullptr) {
    if (depth > 16 || i_ == data_.size()) return false;
    uint32_t n;
    switch (data_[i_++]) {
      case 0: return Skip(8);
      case 1: return Skip(1);
      case 2: return UInt(2, n) && Skip(n);
      case 5: case 6: return true;
      case 7: return Skip(2);
      case 10:
        if (!UInt(4, n) || n > data_.size() - i_) return false;
        while (n--) if (!Value(depth + 1)) return false;
        return true;
      case 11: return Skip(10);
      case 12: case 15: return UInt(4, n) && Skip(n);
      case 8: if (!Skip(4)) return false; break; // ECMA array count is advisory
      case 3: break;
      default: return false;
    }
    std::string key;
    while (String(key)) {
      if (key.empty() && Marker(9)) return true;
      if (info && (key == "code" || key == "level")) {
        auto& s = key == "code" ? info->code : info->level;
        if (!s.empty() || !Marker(2) || !String(s) || s.empty()) return false;
      } else if (!Value(depth + 1)) return false;
    }
    return false;
  }
};
}  // namespace ipcam
#endif
