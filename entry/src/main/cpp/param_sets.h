#ifndef IPCAMERA_PARAM_SETS_H
#define IPCAMERA_PARAM_SETS_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace ipcam {

// Merges freshly seen H.264 SPS/PPS or H.265 VPS/SPS/PPS NALs (Annex-B) into
// a cached Annex-B parameter-set blob. Replacement is per NAL type: a buffer
// carrying only a PPS keeps the cached SPS (and VPS), while a repeated set
// replaces its predecessor instead of growing the cache. Output is ordered
// VPS, SPS, PPS with 4-byte start codes; non-parameter NALs are dropped.
inline std::vector<uint8_t> MergeParamSets(const std::vector<uint8_t>& cached,
                                           const std::vector<uint8_t>& fresh,
                                           bool h265) {
  using Groups = std::map<uint8_t, std::vector<std::vector<uint8_t>>>;
  auto split = [h265](const std::vector<uint8_t>& in) {
    Groups groups;
    const size_t size = in.size();
    size_t i = 0;
    size_t start = SIZE_MAX;
    auto emit = [&](size_t begin, size_t end) {
      while (end > begin + 1 && in[end - 1] == 0x00) --end;
      if (end <= begin + 1) return;
      const uint8_t type = h265 ? static_cast<uint8_t>((in[begin] >> 1) & 0x3F)
                                : static_cast<uint8_t>(in[begin] & 0x1F);
      const bool param = h265 ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
      if (param) groups[type].emplace_back(in.begin() + begin, in.begin() + end);
    };
    while (i + 3 <= size) {
      if (in[i] == 0x00 && in[i + 1] == 0x00 && in[i + 2] == 0x01) {
        if (start != SIZE_MAX && i > start) emit(start, i);
        i += 3;
        start = i;
      } else {
        ++i;
      }
    }
    if (start != SIZE_MAX && size > start) emit(start, size);
    return groups;
  };

  Groups merged = split(cached);
  for (auto& entry : split(fresh)) merged[entry.first] = std::move(entry.second);

  std::vector<uint8_t> out;
  for (const auto& entry : merged) {  // std::map orders VPS < SPS < PPS
    for (const auto& nal : entry.second) {
      static const uint8_t kStart[] = {0x00, 0x00, 0x00, 0x01};
      out.insert(out.end(), kStart, kStart + 4);
      out.insert(out.end(), nal.begin(), nal.end());
    }
  }
  return out;
}

}  // namespace ipcam

#endif  // IPCAMERA_PARAM_SETS_H
