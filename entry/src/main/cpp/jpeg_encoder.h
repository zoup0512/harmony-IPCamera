#ifndef IPCAMERA_JPEG_ENCODER_H
#define IPCAMERA_JPEG_ENCODER_H

#include <cstdint>
#include <vector>

namespace ipcam {

// Baseline JPEG encoder for NV12 input (4:2:0), fixed quality ~75.
// Huffman tables are self-built but fully valid canonical tables (DC: 16
// symbols @ 4 bits, AC: 256 symbols @ 9 bits) so any decoder accepts the
// file; compression is slightly worse than tuned tables.
class JpegEncoder {
 public:
  // Returns a complete JPEG file stream for the NV12 frame.
  static std::vector<uint8_t> EncodeNV12(const uint8_t* nv12, int width, int height);
};

}  // namespace ipcam

#endif  // IPCAMERA_JPEG_ENCODER_H
