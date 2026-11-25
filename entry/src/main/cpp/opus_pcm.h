#ifndef IPCAMERA_OPUS_PCM_H
#define IPCAMERA_OPUS_PCM_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ipcam {

inline bool IsOpusRate(int rate) {
  return rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000;
}

// S16LE, interleaved. Keep rational phase and one frame across capture callbacks;
// 44.1 kHz and other non-Opus rates must be converted, not relabeled as 48 kHz.
class OpusPcmConverter {
 public:
  void Reset(int sourceRate, int sourceChannels, int codecRate, int codecChannels) {
    sourceRate_ = sourceRate; sourceChannels_ = sourceChannels;
    codecRate_ = codecRate; codecChannels_ = codecChannels;
    havePrev_ = false;
    phase_ = 0;
  }

  void Append(const uint8_t* src, size_t frames, std::vector<uint8_t>& out) {
    if (sourceRate_ == codecRate_ && sourceChannels_ == codecChannels_) {
      out.insert(out.end(), src, src + frames * sourceChannels_ * 2);
      return;
    }
    for (size_t i = 0; i < frames; ++i, src += 2 * sourceChannels_) {
      const int left = Read(src);
      const int right = sourceChannels_ == 2 ? Read(src + 2) : left;
      const int current[] = {codecChannels_ == 1 ? (left + right) / 2 : left, right};
      if (!havePrev_) {
        for (int c = 0; c < codecChannels_; ++c) Write(current[c], out);
        phase_ = sourceRate_;
        havePrev_ = true;
      } else {
        while (phase_ <= codecRate_) {
          for (int c = 0; c < codecChannels_; ++c) {
            const int delta = static_cast<int>((static_cast<int64_t>(current[c] - prev_[c]) * phase_) / codecRate_);
            Write(prev_[c] + delta, out);
          }
          phase_ += sourceRate_;
        }
        phase_ -= codecRate_;
      }
      prev_[0] = current[0]; prev_[1] = current[1];
    }
  }

  static int Read(const uint8_t* p) {
    const int value = p[0] | (p[1] << 8);
    return value >= 32768 ? value - 65536 : value;
  }

 private:
  static void Write(int v, std::vector<uint8_t>& out) {
    const uint16_t bits = static_cast<uint16_t>(v);
    out.push_back(static_cast<uint8_t>(bits)); out.push_back(static_cast<uint8_t>(bits >> 8));
  }
  int sourceRate_ = 48000, sourceChannels_ = 1, codecRate_ = 48000, codecChannels_ = 1;
  int phase_ = 0, prev_[2]{};
  bool havePrev_ = false;
};

// RFC 6716 section 3.1: duration is per channel; Ogg uses 48 kHz even for
// narrowband inputs. Granules refer to the END (not the PTS) of each packet.
inline unsigned OpusPacketTicks(const uint8_t* p, size_t n) {
  if (!n) return 0;
  const unsigned config = p[0] >> 3;
  const unsigned code = p[0] & 3;
  const unsigned frames = code == 0 ? 1 : code < 3 ? 2 : n >= 2 ? p[1] & 63 : 0;
  const unsigned silk[] = {480, 960, 1920, 2880};
  const unsigned samples = config < 12 ? silk[config % 4] : config < 16 ? 480U << (config % 2) : 120U << (config % 4);
  return frames * samples <= 5760 ? frames * samples : 0;
}

}  // namespace ipcam
#endif
