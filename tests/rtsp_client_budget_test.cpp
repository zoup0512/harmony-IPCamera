// Standalone C++17 test; no camera, decoder, or network required.
#include "../entry/src/main/cpp/rtsp_client_protocol.h"
#include <cassert>
#include <iostream>

using namespace liveavc;
namespace {
class Bits {
 public:
  void put(unsigned value, unsigned count) {
    while (count--) {
      if (!(written % 8)) bytes.push_back(0);
      bytes.back() |= ((value >> count) & 1) << (7 - written++ % 8);
    }
  }
  void ue(unsigned value) {
    unsigned width = 0;
    for (unsigned v = value + 1; v; v >>= 1) ++width;
    put(0, width - 1); put(value + 1, width);
  }
  Bytes sps() {
    put(1, 1); // rbsp_stop_one_bit; the last byte is already zero-padded
    Bytes nal{0, 0, 0, 1, 0x67};
    unsigned zeros = 0;
    for (uint8_t v : bytes) {
      if (zeros == 2 && v <= 3) { nal.push_back(3); zeros = 0; }
      nal.push_back(v); zeros = v == 0 ? zeros + 1 : 0;
    }
    return nal;
  }
 private:
  Bytes bytes;
  unsigned written = 0;
};
Bytes config(unsigned cols, unsigned rows, unsigned refs, unsigned buffering, bool high = false) {
  Bits b;
  b.put(high ? 100 : 66, 8); b.put(0, 8); b.put(41, 8); b.ue(0);
  if (high) { b.ue(1); b.ue(0); b.ue(0); b.put(0, 2); }
  b.ue(0); b.ue(2); b.ue(refs); b.put(0, 1);
  b.ue(cols - 1); b.ue(rows - 1); b.put(1, 1); b.put(1, 1); b.put(0, 1);
  b.put(1, 1); b.put(0, 8); // VUI present, no optional fields before restrictions
  b.put(1, 1); b.put(1, 1); b.ue(2); b.ue(1); b.ue(16); b.ue(16);
  b.ue(0); b.ue(buffering);
  return b.sps();
}
void testBudget() {
  const Bytes slice{0, 0, 1, 0x65, 0x80};
  AvcBudget budget;
  assert(budget.inspect(slice, false) == Admission::WaitForSps);
  assert(budget.inspect(config(120, 68, 4, 4), true) == Admission::Ready);
  assert(budget.inspect(slice, false) == Admission::Ready);
  assert(budget.inspect(config(80, 45, 4, 5, true), false) == Admission::Ready);
  for (bool sdp : {true, false}) {
    assert(budget.inspect(config(160, 90, 2, 2), sdp) == Admission::Unsupported);
    assert(budget.inspect(config(120, 68, 5, 5), sdp) == Admission::Unsupported);
    assert(budget.inspect(config(120, 68, 2, 5), sdp) == Admission::Unsupported);
  }
  Bytes mixed = config(80, 45, 2, 2);
  auto large = config(160, 90, 2, 2);
  mixed.insert(mixed.end(), large.begin()+1, large.end()); // three-byte Annex B prefix
  AvcBudget fresh;
  assert(fresh.inspect(mixed, false) == Admission::Unsupported);
  assert(fresh.inspect(slice, false) == Admission::WaitForSps); // admission is atomic
  assert(fitsPicture(1920, 1080) && !fitsPicture(1920, 1200));
}
Bytes rtp(uint8_t source, uint8_t seq, bool mark, uint8_t sample) {
  return {0x80, uint8_t(96 | (mark ? 128 : 0)), 0, seq, 0, 0, 0, 1,
          0, 0, 0, source, 0x61, sample};
}
void testTrackIsolation() {
  RtpAvc r; Bytes frame; uint32_t pts = 0;
  assert(!r.packet(rtp(1, 1, false, 0x11), 96, frame, pts));
  assert(!r.packet(rtp(2, 2, true, 0x22), 96, frame, pts));
  assert(r.packet(rtp(1, 2, true, 0x33), 96, frame, pts));
  assert((frame == Bytes{0,0,0,1,0x61,0x11,0,0,0,1,0x61,0x33}));
  Track track;
  assert(selectAvc("m=video 0 RTP/AVP 097 97 96 96\na=control:v\na=rtpmap:97 H264/90000\n"
                   "a=fmtp:97 packetization-mode=2\na=rtpmap:96 H264/90000\n", track));
  assert(track.pt == 96);
  assert(number("2147483647", 2147483647) == 2147483647);
}
} // namespace
int main() {
  testBudget(); testTrackIsolation();
  std::cout << "PASS: AVC admission, DPB/VUI, source isolation and PT selection\n";
}
