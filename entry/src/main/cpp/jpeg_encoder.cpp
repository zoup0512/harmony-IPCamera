#include "jpeg_encoder.h"

#include <cmath>
#include <cstring>

namespace ipcam {
namespace {

// Annex K quant tables at quality 50.
const int kQuantLuma50[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const int kQuantChroma50[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

const int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18,
                         11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20,
                         13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43,
                         36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45,
                         38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

void ScaleQuant(const int* base, uint8_t out[64]) {
  const int S = 75 < 50 ? 5000 / 75 : 200 - 2 * 75;  // quality 75 -> S=50
  for (int i = 0; i < 64; ++i) {
    int q = (base[i] * S + 50) / 100;
    if (q < 1) q = 1;
    if (q > 255) q = 255;
    out[i] = static_cast<uint8_t>(q);
  }
}

// forward DCT, float, naive separable (8x8).
void Fdct8x8(const float* in, float* out) {
  float tmp[64];
  for (int y = 0; y < 8; ++y) {
    for (int u = 0; u < 8; ++u) {
      float sum = 0;
      for (int x = 0; x < 8; ++x) {
        sum += in[y * 8 + x] *
               static_cast<float>(std::cos((2 * x + 1) * u * 3.14159265358979 / 16.0));
      }
      tmp[y * 8 + u] = (u == 0 ? 0.70710678f : 1.0f) / 2.0f * sum;
    }
  }
  for (int u = 0; u < 8; ++u) {
    for (int v = 0; v < 8; ++v) {
      float sum = 0;
      for (int y = 0; y < 8; ++y) {
        sum += tmp[y * 8 + u] *
               static_cast<float>(std::cos((2 * y + 1) * v * 3.14159265358979 / 16.0));
      }
      out[v * 8 + u] = (v == 0 ? 0.70710678f : 1.0f) / 2.0f * sum;
    }
  }
}

// MSB-first bit writer with 0xFF stuffing.
struct BitWriter {
  std::vector<uint8_t>* out;
  uint32_t acc = 0;
  int nbits = 0;
  void Put(uint32_t value, int bits) {
    acc = (acc << bits) | (value & ((1u << bits) - 1));
    nbits += bits;
    while (nbits >= 8) {
      uint8_t b = static_cast<uint8_t>((acc >> (nbits - 8)) & 0xFF);
      out->push_back(b);
      if (b == 0xFF) out->push_back(0x00);
      nbits -= 8;
    }
  }
  void Flush() {
    if (nbits > 0) Put((1 << (8 - nbits)) - 1, 8 - nbits);  // pad with 1s
    acc = 0;
    nbits = 0;
  }
};

void PushMarker(std::vector<uint8_t>& v, uint8_t marker, const std::vector<uint8_t>& payload) {
  v.push_back(0xFF);
  v.push_back(marker);
  v.push_back(static_cast<uint8_t>((payload.size() + 2) >> 8));
  v.push_back(static_cast<uint8_t>((payload.size() + 2) & 0xFF));
  v.insert(v.end(), payload.begin(), payload.end());
}

void PushDQT(std::vector<uint8_t>& v, uint8_t id, const uint8_t q[64]) {
  std::vector<uint8_t> p;
  p.push_back(id);
  for (int i = 0; i < 64; ++i) p.push_back(q[kZigzag[i]]);
  PushMarker(v, 0xDB, p);
}

// Huffman BITS/HUFFVAL -> DHT segment (canonical, our own tables).
void PushDHT(std::vector<uint8_t>& v, uint8_t tcTh, const uint8_t bits[16],
             const std::vector<uint8_t>& vals) {
  std::vector<uint8_t> p;
  p.push_back(tcTh);
  for (int i = 0; i < 16; ++i) p.push_back(bits[i]);
  p.insert(p.end(), vals.begin(), vals.end());
  PushMarker(v, 0xC4, p);
}

void PushMarker16(std::vector<uint8_t>& v, uint8_t marker, const std::vector<uint8_t>& payload) {
  PushMarker(v, marker, payload);
}

}  // namespace

std::vector<uint8_t> JpegEncoder::EncodeNV12(const uint8_t* nv12, int width, int height) {
  std::vector<uint8_t> jpg;
  uint8_t qluma[64];
  uint8_t qchroma[64];
  ScaleQuant(kQuantLuma50, qluma);
  ScaleQuant(kQuantChroma50, qchroma);

  // ---- headers ----
  jpg.push_back(0xFF);
  jpg.push_back(0xD8);  // SOI
  {
    std::vector<uint8_t> app0 = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    PushMarker(jpg, 0xE0, app0);
  }
  PushDQT(jpg, 0x00, qluma);
  PushDQT(jpg, 0x01, qchroma);
  {
    std::vector<uint8_t> sof;
    sof.push_back(8);
    sof.push_back(static_cast<uint8_t>((height >> 8) & 0xFF));
    sof.push_back(static_cast<uint8_t>(height & 0xFF));
    sof.push_back(static_cast<uint8_t>((width >> 8) & 0xFF));
    sof.push_back(static_cast<uint8_t>(width & 0xFF));
    sof.push_back(3);
    sof.push_back(1);
    sof.push_back(0x22);
    sof.push_back(0);
    sof.push_back(2);
    sof.push_back(0x11);
    sof.push_back(1);
    sof.push_back(3);
    sof.push_back(0x11);
    sof.push_back(1);
    PushMarker(jpg, 0xC0, sof);
  }
  const uint8_t dcBits[16] = {0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::vector<uint8_t> dcVals;
  for (int i = 0; i < 16; ++i) dcVals.push_back(static_cast<uint8_t>(i));
  PushDHT(jpg, 0x00, dcBits, dcVals);
  PushDHT(jpg, 0x01, dcBits, dcVals);
  // AC: 128 symbols @ 8 bits (0..127) + 128 @ 9 bits (128..255) — valid
  // canonical Huffman (BITS byte max is 255, so 256 codes cannot share one
  // length). Symbol s: s<128 -> 8-bit code s; else 9-bit code 256+(s-128).
  const uint8_t acBits[16] = {0, 0, 0, 0, 0, 0, 0, 128, 128, 0, 0, 0, 0, 0, 0, 0};
  std::vector<uint8_t> acVals;
  for (int i = 0; i < 256; ++i) acVals.push_back(static_cast<uint8_t>(i));
  PushDHT(jpg, 0x10, acBits, acVals);
  PushDHT(jpg, 0x11, acBits, acVals);
  {
    std::vector<uint8_t> sos;
    sos.push_back(3);
    sos.push_back(1);
    sos.push_back(0x00);
    sos.push_back(2);
    sos.push_back(0x11);
    sos.push_back(3);
    sos.push_back(0x11);
    sos.push_back(0);
    sos.push_back(63);
    sos.push_back(0);
    PushMarker(jpg, 0xDA, sos);
  }

  // ---- entropy-coded data ----
  BitWriter bw{&jpg};
  int dcPred[3] = {0, 0, 0};
  const uint8_t* yPlane = nv12;
  const uint8_t* uvPlane = nv12 + static_cast<size_t>(width) * height;
  auto sampleY = [&](int x, int y) {
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;
    return yPlane[static_cast<size_t>(y) * width + x];
  };
  auto sampleU = [&](int x, int y) {
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;
    return uvPlane[static_cast<size_t>(y / 2) * width + (x & ~1)];
  };
  auto sampleV = [&](int x, int y) {
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;
    return uvPlane[static_cast<size_t>(y / 2) * width + (x & ~1) + 1];
  };

  auto encodeBlock = [&](const float* block, const uint8_t q[64], int compIdx) {
    float freq[64];
    Fdct8x8(block, freq);
    int coef[64];
    for (int i = 0; i < 64; ++i) {
      int zi = kZigzag[i];
      int v = static_cast<int>(std::lround(freq[zi] / q[zi]));
      coef[i] = v;
    }
    // DC
    int diff = coef[0] - dcPred[compIdx];
    dcPred[compIdx] = coef[0];
    int cat = 0;
    int absv = diff < 0 ? -diff : diff;
    while (absv) {
      cat++;
      absv >>= 1;
    }
    bw.Put(cat, 4);  // DC Huffman: 4-bit codes, symbol = cat (0..15)
    if (cat) {
      int v = diff > 0 ? diff : diff + (1 << cat) - 1;
      bw.Put(static_cast<uint32_t>(v), cat);
    }
    // AC: symbol s -> s<128 ? 8-bit code s : 9-bit code 256+(s-128)
    auto putSym = [&bw](int sym) {
      if (sym < 128) {
        bw.Put(static_cast<uint32_t>(sym), 8);
      } else {
        bw.Put(static_cast<uint32_t>(256 + (sym - 128)), 9);
      }
    };
    int run = 0;
    for (int i = 1; i < 64; ++i) {
      int v = coef[i];
      if (v == 0) {
        run++;
        continue;
      }
      while (run > 15) {
        putSym(0xF0);  // ZRL
        run -= 16;
      }
      int mag = v < 0 ? -v : v;
      int cat2 = 0;
      int a2 = mag;
      while (a2) {
        cat2++;
        a2 >>= 1;
      }
      putSym((run << 4) | cat2);
      int ex = v > 0 ? v : v + (1 << cat2) - 1;
      if (cat2) bw.Put(static_cast<uint32_t>(ex), cat2);
      run = 0;
    }
    if (run > 0) putSym(0x00);  // EOB
  };

  const int mcuW = (width + 15) / 16;
  const int mcuH = (height + 15) / 16;
  for (int my = 0; my < mcuH; ++my) {
    for (int mx = 0; mx < mcuW; ++mx) {
      int px = mx * 16;
      int py = my * 16;
      for (int by = 0; by < 2; ++by) {
        for (int bx = 0; bx < 2; ++bx) {
          float blk[64];
          for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
              blk[y * 8 + x] = static_cast<float>(sampleY(px + bx * 8 + x, py + by * 8 + y)) - 128.0f;
            }
          }
          encodeBlock(blk, qluma, 0);
        }
      }
      float cb[64];
      float cr[64];
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
          int sx = px + x * 2;
          int sy = py + y * 2;
          cb[y * 8 + x] = static_cast<float>(sampleU(sx, sy)) - 128.0f;
          cr[y * 8 + x] = static_cast<float>(sampleV(sx, sy)) - 128.0f;
        }
      }
      encodeBlock(cb, qchroma, 1);
      encodeBlock(cr, qchroma, 2);
    }
  }
  bw.Flush();
  jpg.push_back(0xFF);
  jpg.push_back(0xD9);  // EOI
  return jpg;
}

}  // namespace ipcam
