// Offline TCP loopback. Compile with C++17, OH native headers, -ffunction-sections
// -fdata-sections -Wl,--gc-sections -pthread. No camera, audio device, RTMP daemon or N-API runtime.
// Include the production adapter; only the SDK surface/decoder and audio device calls are doubled.
#include "../entry/src/main/cpp/rtmp_viewer.cpp"
#include "../entry/src/main/cpp/flv_stream.h"
#include <cassert>
#include <iostream>

namespace {
using namespace rtmpview;
const Bytes sps = liveavc::unbase64("Z0LACtkEJsBEAAADAAQAAAMACDxImSA="), pps = liveavc::unbase64("aMuDyyA="),
            idr = liveavc::unbase64("ZYiEBX///w9FAAFC3ycnJ1111111111114A=");  // 64x64 black picture
Bytes annex(std::initializer_list<Bytes> list) {
  Bytes b;
  for (auto& n : list) {
    b.insert(b.end(), {0, 0, 0, 1});
    b.insert(b.end(), n.begin(), n.end());
  }
  return b;
}
struct Fixture {
  std::atomic<int> samples{0}, configs{0}, renders{0}, feeds{0}, destroyed{0};
  std::atomic<bool> output{false};
  std::atomic<unsigned> flags{0}, freed{0};
  bool failRender = false;
  OH_AVCodecBufferAttr attr{};
  uint8_t input[1024]{};
};
Fixture* f;
}  // namespace
struct OH_AVCodec {};
struct OH_AVBuffer {};
struct OH_AVFormat {};
struct NativeWindow {};
extern "C" {
const char* OH_MD_KEY_ENABLE_SYNC_MODE = "sync";
const char* OH_MD_KEY_VIDEO_PIC_WIDTH = "width";
const char* OH_MD_KEY_VIDEO_PIC_HEIGHT = "height";
int32_t OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t id, OHNativeWindow** w) {
  assert(id == 123);
  *w = new NativeWindow;
  return 0;
}
void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow* w) { delete w; }
OH_AVCodec* OH_VideoDecoder_CreateByMime(const char* mime) {
  assert(!strcmp(mime, "video/avc"));
  return new OH_AVCodec;
}
OH_AVFormat* OH_AVFormat_CreateVideoFormat(const char*, int32_t w, int32_t h) {
  assert(liveavc::fitsPicture(w, h));
  return new OH_AVFormat;
}
bool OH_AVFormat_SetIntValue(OH_AVFormat*, const char*, int32_t) { return true; }
void OH_AVFormat_Destroy(OH_AVFormat* a) { delete a; }
OH_AVErrCode OH_VideoDecoder_Configure(OH_AVCodec*, OH_AVFormat*) { return AV_ERR_OK; }
OH_AVErrCode OH_VideoDecoder_SetSurface(OH_AVCodec*, OHNativeWindow*) { return AV_ERR_OK; }
OH_AVErrCode OH_VideoDecoder_Prepare(OH_AVCodec*) { return AV_ERR_OK; }
OH_AVErrCode OH_VideoDecoder_Start(OH_AVCodec*) { return AV_ERR_OK; }
OH_AVErrCode OH_VideoDecoder_Destroy(OH_AVCodec* c) {
  delete c;
  ++f->destroyed;
  return AV_ERR_OK;
}
OH_AVErrCode OH_VideoDecoder_QueryInputBuffer(OH_AVCodec*, uint32_t* n, int64_t) {
  *n = 1;
  return AV_ERR_OK;
}
OH_AVBuffer* OH_VideoDecoder_GetInputBuffer(OH_AVCodec*, uint32_t) {
  static OH_AVBuffer b;
  return &b;
}
int32_t OH_AVBuffer_GetCapacity(OH_AVBuffer*) { return sizeof(f->input); }
uint8_t* OH_AVBuffer_GetAddr(OH_AVBuffer*) { return f->input; }
OH_AVErrCode OH_AVBuffer_SetBufferAttr(OH_AVBuffer*, const OH_AVCodecBufferAttr* a) {
  f->attr = *a;
  return AV_ERR_OK;
}
OH_AVErrCode OH_VideoDecoder_PushInputBuffer(OH_AVCodec*, uint32_t) {
  Bytes b(f->input, f->input + f->attr.size);
  if (f->attr.flags == AVCODEC_BUFFER_FLAGS_CODEC_DATA) {
    assert(b == annex({sps, pps}));
    ++f->configs;
  } else {
    assert(f->configs && b == annex({idr}) && f->attr.pts == 37000);
    ++f->samples;
  }
  return AV_ERR_OK;
}
OH_AVErrCode OH_VideoDecoder_QueryOutputBuffer(OH_AVCodec*, uint32_t* i, int64_t) {
  if (!f->output.exchange(false)) return AV_ERR_TRY_AGAIN_LATER;
  *i = 5;
  return AV_ERR_OK;
}
OH_AVBuffer* OH_VideoDecoder_GetOutputBuffer(OH_AVCodec*, uint32_t) {
  static OH_AVBuffer b;
  return &b;
}
OH_AVErrCode OH_AVBuffer_GetBufferAttr(OH_AVBuffer*, OH_AVCodecBufferAttr* a) {
  *a = {};
  a->flags = f->flags.load();
  return AV_ERR_OK;
}
OH_AVErrCode OH_VideoDecoder_FreeOutputBuffer(OH_AVCodec*, uint32_t) {
  ++f->freed;
  return AV_ERR_OK;
}
OH_AVFormat* OH_VideoDecoder_GetOutputDescription(OH_AVCodec*) { return new OH_AVFormat; }
bool OH_AVFormat_GetIntValue(OH_AVFormat*, const char*, int32_t* v) {
  *v = 64;
  return true;
}
OH_AVErrCode OH_VideoDecoder_RenderOutputBuffer(OH_AVCodec*, uint32_t i) {
  assert(i == 5);
  ++f->renders;
  return f->failRender ? AV_ERR_UNKNOWN : AV_ERR_OK;
}
}
namespace ipcam {
struct VoicePlayer::Impl {};
VoicePlayer::VoicePlayer() = default;
VoicePlayer::~VoicePlayer() = default;
VoicePlayer::FeedResult VoicePlayer::Feed(const uint8_t* p, size_t n) {
  AdtsInfo a;
  assert(ParseAdts(p, n, &a) && n == 10 && a.sampleRate == 44100 && a.channels == 2);
  ++f->feeds;
  return FeedResult::Accepted;
}
}  // namespace ipcam
namespace {
void writeAll(int fd, const Bytes& b) {
  for (size_t i = 0; i < b.size();) {
    auto n = send(fd, b.data() + i, std::min<size_t>(29, b.size() - i), MSG_NOSIGNAL);
    assert(n > 0);
    i += n;
  }
}
void readAll(int fd, uint8_t* p, size_t size) {
  while (size) {
    auto n = recv(fd, p, size, 0);
    assert(n > 0);
    size -= n;
    p += n;
  }
}
Bytes statusCommand(const char* name, double tx, const char* code) {
  auto b = command(name, tx);
  b.insert(b.end(), {5, 3});
  key(b, "code");
  str(b, code);
  b.insert(b.end(), {0, 0, 9});
  return b;
}
template <class F>
void await(F ok) {
  auto end = Clock::now() + 3s;
  while (!ok()) {
    assert(Clock::now() < end);
    std::this_thread::sleep_for(2ms);
  }
}
enum Case { Success, RenderFailure, Missing, Cancelled, NoFrame, Enhanced };
void scenario(Case mode) {
  Fixture state;
  f = &state;
  state.failRender = mode == RenderFailure;
  int server = socket(AF_INET, SOCK_STREAM, 0);
  assert(server >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  assert(!bind(server, (sockaddr*)&a, len) && !listen(server, 1) &&
         !getsockname(server, (sockaddr*)&a, &len));
  auto ep = endpoint("rtmp://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/live/selected?token=test");
  Session s;
  Status ended = Connecting;
  std::thread worker([&] {
    try {
      SurfaceSink sink(s, 123);
      play(s, ep, sink);
    } catch (Status st) {
      ended = st;
    }
  });
  pollfd ready{server, POLLIN, 0};
  assert(poll(&ready, 1, 3000) == 1);
  int fd = accept(server, nullptr, nullptr);
  assert(fd >= 0);
  timeval tv{3, 0};
  assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));
  Bytes c(1537), s1(1537, 0x31), c2(1536);
  readAll(fd, c.data(), c.size());
  assert(c[0] == 3);
  if (mode == Cancelled) {
    s.stopped = true;
    worker.join();
    assert(ended == Stopped && !f->renders);
    close(fd);
    close(server);
    return;
  }
  s1[0] = 3;
  writeAll(fd, s1);
  readAll(fd, c2.data(), c2.size());
  assert(std::equal(c2.begin(), c2.end(), s1.begin() + 1));
  writeAll(fd, Bytes(c.begin() + 1, c.end()));
  // Independently parse client commands with the existing publisher-side reader.
  ipcam::RtmpChunkParser peer(65536);
  Bytes input;
  bool ack = false;
  auto expect = [&](uint8_t type, uint32_t id, auto body) {
    bool found = false;
    while (!found) {
      uint8_t ch;
      readAll(fd, &ch, 1);
      input.push_back(ch);
      ipcam::RtmpChunkParser::Status st;
      size_t used = peer.Consume(input.data(), input.size(), &st, [&](uint8_t t, uint32_t i, const Bytes& b) {
        if (t == 3) {
          assert(!i && be32(b.data()) > 0);
          ack = true;
          return true;
        }
        assert(t == type && i == id);
        body(b);
        found = true;
        return true;
      });
      assert(st == ipcam::RtmpChunkParser::Status::kOk);
      input.erase(input.begin(), input.begin() + used);
    }
  };
  expect(20, 0, [&](auto& b) { assert(b == connectCommand(ep)); });
  writeAll(fd, wire(5, 2, 0, {0, 0, 0, 64}));
  writeAll(fd, wire(20, 3, 0, statusCommand("_result", 1, "NetConnection.Connect.Success")));
  expect(20, 0, [](auto& b) {
    auto create = command("createStream", 2);
    create.push_back(5);
    assert(b == create);
  });
  auto created = command("_result", 2);
  created.push_back(5);
  num(created, 37);
  writeAll(fd, wire(20, 3, 0, created));
  expect(20, 37, [&](auto& b) {
    auto play = command("play", 0);
    play.push_back(5);
    str(play, ep.stream);
    num(play, -1);
    assert(b == play);
  });
  assert(ack && s.state == Connecting);
  auto msg = [&](uint8_t t, uint32_t id, const Bytes& b, uint32_t ts = 0) {
    writeAll(fd, wire(t, t == 4 ? 2 : 6, id, b, ts));
  };
  msg(20, 99,
      statusCommand("onStatus", 0, "NetStream.Play.StreamNotFound"));  // wrong stream must not end playback
  msg(20, 37,
      statusCommand("onStatus", 0,
                    mode == Missing ? "NetStream.Play.StreamNotFound" : "NetStream.Play.Start"));
  if (mode == Missing) {
    worker.join();
    assert(ended == NotFound && !f->configs);
  } else if (mode == NoFrame) {
    worker.join();
    assert(ended == Timeout && s.state == Connecting);
  } else if (mode == Enhanced) {
    msg(9, 37, {0x90, 'h', 'v', 'c', '1'});
    worker.join();
    assert(ended == EnhancedHevc && !f->configs);
  } else {
    msg(9, 99, {0x90, 'h', 'v', 'c', '1'});
    msg(4, 0, {0, 6, 1, 2, 3, 4});
    expect(4, 0, [](auto& b) { assert((b == Bytes{0, 7, 1, 2, 3, 4})); });
    // Feed output of the project's real FLV AVC helper, not a hand-written avcC.
    ipcam::FlvStream flv;
    auto cfg = annex({sps, pps}), au = annex({idr});
    flv.FeedVideo(cfg.data(), cfg.size(), 0);
    std::shared_ptr<ipcam::FlvStream::Reader> r;
    assert(flv.Open(r) == ipcam::FlvStream::OpenResult::Ok);
    flv.FeedVideo(au.data(), au.size(), 40000);
    bool audio = false;
    ipcam::FlvStream::Tag tag{};
    for (int i = 0; i < 2; ++i) {
      assert(flv.Read(r, tag, audio) == ipcam::FlvStream::ReadResult::TagReady);
      Bytes b = *tag.body;
      if (i) {
        b[2] = b[3] = 255;
        b[4] = 253;
      }
      msg(tag.type, 37, b, 40);
    }
    await([&] { return f->samples == 1; });
    assert(s.state == Connecting && !f->renders);
    msg(8, 37, {0xaf, 0, 0x12, 0x10});
    msg(8, 37, {0xaf, 1, 1, 2, 3});
    msg(4, 0, {0, 6, 4, 3, 2, 1});
    expect(4, 0, [](auto& b) { assert((b == Bytes{0, 7, 4, 3, 2, 1})); });
    assert(s.audio == 0 && !f->feeds);
    s.listen = true;
    await([&] { return s.audio == 1; });
    msg(8, 37, {0xaf, 1, 1, 2, 3});
    await([&] { return s.audio == 2; });
    assert(f->feeds == 1);
    s.listen = false;
    await([&] { return s.audio == 0; });
    f->flags = AVCODEC_BUFFER_FLAGS_DISCARD;
    f->output = true;
    await([&] { return f->freed == 1; });
    assert(!f->renders && s.state == Connecting);
    f->flags = 0;
    f->output = true;  // release delayed output with socket silent
    if (mode == RenderFailure) {
      worker.join();
      assert(ended == DecoderError && s.state == Connecting);
    } else {
      await([&] { return s.state == Playing; });
      msg(20, 37, statusCommand("onStatus", 0, "NetStream.Play.Stop"));
      worker.join();
      assert(ended == Ended);
    }
    assert(f->renders == 1 && f->destroyed == 1);
  }
  close(fd);
  close(server);
}
}  // namespace
int main() {
  for (auto c : {Cancelled, Missing, Enhanced, Success, RenderFailure, NoFrame}) scenario(c);
  std::cout << "PASS local RTMP play, selected stream, control, AVC Surface render gate, AAC mute, failure, "
               "stop and watchdog\n";
}
