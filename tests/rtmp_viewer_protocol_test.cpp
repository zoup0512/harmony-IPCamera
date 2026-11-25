// c++ -std=c++17 -pthread tests/rtmp_viewer_protocol_test.cpp -o /tmp/rtmp-protocol
#include "../entry/src/main/cpp/rtmp_viewer_protocol.h"
#include "../entry/src/main/cpp/codec_records.h"
#include <cassert>
#include <iostream>
using namespace rtmpview;

template <class F>
void fails(Status want, F f) {
  try {
    f();
    assert(false);
  } catch (Status got) {
    assert(got == want);
  }
}
static void endpoints() {
  auto a = endpoint("rtmp://[::1]:01935/live/cam/sub?token=A%2FB");
  assert(a.host == "::1" && a.port == "1935" && a.app == "live" && a.stream == "cam/sub?token=A%2FB");
  assert(a.tcUrl == "rtmp://[::1]:01935/live" && endpoint("rtmp://camera.lan/a/b").port == "1935");
  for (auto u :
       {"rtmp://u:p@localhost/a/b", "rtmp://host:65536/a/b", "rtmp://host:+3/a/b", "rtmp://[example]/a/b",
        "rtmp://host//b", "rtmp://host/a/", "rtmp://host/a?x/b", "rtmp://host/a/b#f", "rtmp://host/a/b\r"})
    fails(Protocol, [&] { endpoint(u); });
  fails(Transport, [] { endpoint("rtmps://localhost/a/b"); });
}
static void chunks() {
  // fmt0 absolute extended timestamp, interleaved csid 2 ping, fmt3 continuation,
  // fmt1 + fmt2 deltas, fmt3 new message. A single partial media assembly survives the ping.
  Bytes bytes = wire(9, 6, 31, Bytes(130, 1), 0xffffff);
  auto ping = wire(4, 2, 0, {0, 6, 1, 2, 3, 4});
  bytes.insert(bytes.begin() + 16 + 128, ping.begin(), ping.end());
  bytes.insert(bytes.end(), {0x46, 0, 0, 2, 0, 0, 1, 9, 3, 0x86, 0, 0, 5, 4, 0xc6, 5});
  auto parse = [](const Bytes& b, auto visit) {
    Chunks c;
    size_t i = 0;
    auto read = [&](uint8_t* p, size_t n) {
      if (n > b.size() - i) throw Network;
      std::copy_n(b.data() + i, n, p);
      i += n;
    };
    while (i < b.size()) c.next(read, visit);
  };
  std::vector<uint32_t> timestamps;
  parse(bytes, [&](int type, uint32_t id, uint32_t ts, const Bytes& b) {
    if (type == 4) {
      assert(timestamps.empty());
      return;
    }
    assert(type == 9 && id == 31);
    if (timestamps.empty()) assert(b == Bytes(130, 1));
    timestamps.push_back(ts);
  });
  assert((timestamps == std::vector<uint32_t>{0xffffff, 0x1000001, 0x1000006, 0x100000b}));
  auto ignore = [](auto...) { assert(false); };
  for (auto& b : std::vector<Bytes>{{0xc6},
                                    {6, 0, 0, 0, 0x20, 0, 1, 9, 1, 0, 0, 0},
                                    wire(1, 2, 0, {0, 1, 0, 1}),
                                    wire(5, 3, 0, {0, 0, 0, 1})})
    fails(Protocol, [&] { parse(b, ignore); });
  Bytes many;
  for (int i = 0; i < 3; ++i) {
    many.insert(many.end(), {uint8_t(6 + i), 0, 0, 0, 0x20, 0, 0, 9, 1, 0, 0, 0});
    many.resize(many.size() + 128);
  }
  fails(Protocol, [&] { parse(many, ignore); });  // shared budget, not 2 MiB per CSID
  auto truncated = wire(9, 6, 1, Bytes(140));
  truncated.pop_back();
  fails(Network, [&] { parse(truncated, ignore); });
}
static void media() {
  Bytes sps = liveavc::unbase64("Z0LACtkEJsBEAAADAAQAAAMACDxImSA="), pps = liveavc::unbase64("aMuDyyA=");
  auto config = [&](int width) {
    Bytes b{0x17, 0, 0, 0, 0};
    auto record = ipcam::AvcDecoderConfigRecord(sps, pps);
    record[4] = 0xfc | (width - 1);
    b.insert(b.end(), record.begin(), record.end());
    return b;
  };
  for (int width : {1, 2, 4}) {
    Avc avc;
    int calls = 0;
    auto push = [&](const Bytes& b, int64_t pts, bool cfg) {
      ++calls;
      if (cfg) {
        assert(calls == 1 && pts == 0);
        return;
      }
      assert((b == Bytes{0, 0, 0, 1, 0x65, 0x80}));
      assert(pts == (calls == 2 ? int64_t(UINT32_MAX) - 2 : int64_t(UINT32_MAX) + 1) * 1000);
    };
    avc.packet(config(width), 0, push);
    Bytes b{0x27, 1, 255, 255, 254};
    put(b, 2, width);
    b.insert(b.end(), {0x61, 0x80});
    avc.packet(b, 0, push);
    assert(calls == 1);
    b[0] = 0x17;
    b[b.size() - 2] = 0x65;
    avc.packet(b, UINT32_MAX, push);
    avc.packet(b, 2, push);
    assert(calls == 3);
    b.pop_back();
    fails(Protocol, [&] { avc.packet(b, 0, push); });
  }
  auto never = [](const Bytes&, int64_t, bool) { assert(false); };
  Avc avc;
  fails(VideoCodec, [&] { avc.packet({0x12}, 0, never); });
  fails(EnhancedHevc, [&] { avc.packet({0x90, 'h', 'v', 'c', '1'}, 0, never); });
  fails(EnhancedVideo, [&] { avc.packet({0x90, 'a', 'v', '0', '1'}, 0, never); });
  fails(Protocol, [&] { avc.packet(config(3), 0, never); });
  sps[3] = 50;
  fails(VideoCodec, [&] { avc.packet(config(4), 0, never); });
  sps[3] = 10;
  avc.packet(config(4), 0, [](auto...) {});
  fails(VideoCodec, [&] { avc.packet({0x17, 1, 0, 0, 0, 0, 0, 0, 4, 0x67, 66, 0, 51}, 0, never); });
  fails(Protocol, [&] { avc.packet({0x17, 1, 0, 0, 0, 0, 0, 0, 6, 0x65, 0, 0, 1, 0x67, 0}, 0, never); });
  Aac a;
  Bytes out;
  assert(a.packet({0xaf, 1, 0}, out) == Aac::Unsupported);
  assert(a.packet({0xaf, 0, 0x12, 0x10}, out) == Aac::Config &&
         a.packet({0xaf, 1, 1, 2, 3}, out) == Aac::Frame);
  ipcam::AdtsInfo adts;
  assert(ipcam::ParseAdts(out.data(), out.size(), &adts) && adts.frameLen == 10 && adts.channels == 2 &&
         adts.sampleRate == 44100);
  assert(a.packet(Bytes(8190, 1), out) == Aac::Unsupported &&
         a.packet({0xaf, 0, 0x2b, 0x92}, out) == Aac::Unsupported);
}
int main() {
  endpoints();
  chunks();
  media();
  std::cout << "PASS RTMP endpoint, chunk state and budgets, AVC admission/PTS and AAC framing\n";
}
