#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include "mic_streamer.h"
#include "opus_stream.h"
#include "codec_records.h"
// Inspect the real generated SDP without listening on an interface.
#define private public
#include "rtsp_server.h"
#undef private
using namespace ipcam;
using Bytes = std::vector<uint8_t>;

static Bytes ReadBytes(int fd, size_t n) {
  Bytes b(n); size_t pos = 0;
  while (pos < n) { auto r = recv(fd, b.data()+pos, n-pos, 0); assert(r > 0); pos += r; }
  return b;
}
struct Page { unsigned seq, ticks; Bytes body; };
static Page ReadPage(int fd) {
  auto hdr = ReadBytes(fd, 27); assert(memcmp(hdr.data(), "OggS", 4) == 0);
  auto lacing = ReadBytes(fd, hdr[26]); size_t n = 0; for (int b : lacing) n += b;
  return {hdr[18], static_cast<unsigned>(hdr[6] | hdr[7] << 8), ReadBytes(fd, n)};
}
static void Pipeline(bool stereo) {
  mock::attempts.clear(); mock::opusPts.clear();
  mock::acceptRate = stereo ? 24000 : 44100; mock::acceptCh = stereo ? 2 : 1;
  mock::actualRate = stereo ? 44100 : 16000; mock::actualCh = stereo ? 2 : 1;
  mock::opusRate = stereo ? 48000 : 16000; mock::opusCh = 2;
  mock::opusFmt = stereo ? SAMPLE_S16LE : SAMPLE_F32LE;
  MicStreamer mic; OpusStream opus; RtspServer rtsp;
  rtsp.running_ = true; // No socket creation; only PushAdts and SDP are exercised.
  Bytes adts; int tapRate = 0, tapCh = 0;
  mic.SetPcmTap([&](auto p, auto n, int rate, int ch) {
    tapRate = rate; tapCh = ch; opus.FeedPcm(p, n, rate, ch);
  });
  assert(mic.Start(stereo ? 0 : 44100, stereo ? 1 : 2, 64000,
    [&](auto p, auto n, auto pts) { adts.assign(p,p+n); rtsp.PushAdts(p,n,pts); },
    [](const auto& error) { std::cerr << error << '\n'; assert(false); }));
  using Attempts = std::vector<std::pair<int,int>>;
  const Attempts expected = stereo ? Attempts{{48000,1},{48000,2},{24000,1},{24000,2}} : Attempts{{44100,2},{44100,1}};
  assert(mock::attempts == expected);
  assert(mic.SampleRate() == mock::actualRate && mic.Channels() == mock::actualCh);
  assert(mock::aacConfig.v[0] == mic.SampleRate() && mock::aacConfig.v[1] == mic.Channels());
  opus.SetSourceActive(true, mic.SampleRate(), mic.Channels()); assert(opus.EnsureReady());
  int fds[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  std::atomic<bool> running{true}; std::thread http([&] { opus.Serve(fds[1],running); });
  Page head = ReadPage(fds[0]), tags = ReadPage(fds[0]);
  assert(head.seq == 0 && tags.seq == 1 && head.body.size() == 19);
  assert(memcmp(head.body.data(),"OpusHead",8) == 0 && head.body[9] == mock::opusCh);
  assert((head.body[12] | head.body[13]<<8) == mock::actualRate);
  Bytes pcm; for (int i = 0; i < 2048; ++i) {
    pcm.insert(pcm.end(), {0,0x20}); if (stereo) pcm.insert(pcm.end(), {0,0xe0});
  }
  // Fragment on non-codec boundaries. Only complete interleaved frames enter the tap.
  auto* cap = mock::cap;
  size_t cut = 637 * 2 * mic.Channels();
  cap->cb.OH_AudioCapturer_OnReadData(cap,cap->ctx,pcm.data(),cut);
  cap->cb.OH_AudioCapturer_OnReadData(cap,cap->ctx,pcm.data()+cut,pcm.size()-cut);
  auto first = ReadPage(fds[0]), second = ReadPage(fds[0]);
  assert(first.seq == 2 && first.ticks == 960 && first.body == (Bytes{0x98,0x55}));
  assert(second.seq == 3 && second.ticks == 1920);
  assert(mock::opusPts.at(0) == 0 && mock::opusPts.at(1) == 20000);
  if (stereo) {
    assert(mock::lastOpusInput.size() == 960*4);
    for (size_t i = 0; i < mock::lastOpusInput.size(); ++i) assert(mock::lastOpusInput[i] == pcm[i%4]);
  } else {
    assert(mock::lastOpusInput.size() == 320*2*4);
    for (size_t i = 0; i < mock::lastOpusInput.size(); i+=4) {
      float v; memcpy(&v, mock::lastOpusInput.data()+i,4); assert(v == 0.25f);
    }
  }
  assert(tapRate == mic.SampleRate() && tapCh == mic.Channels());
  AdtsInfo a; assert(ParseAdts(adts.data(),adts.size(),&a));
  assert(a.sampleRate == tapRate && a.channels == tapCh && a.frameLen == int(adts.size()));
  assert(rtsp.BuildSdp(false,"127.0.0.1").find("MPEG4-generic/"+std::to_string(tapRate)+"/"+std::to_string(tapCh)+"\r\n") != std::string::npos);
  std::ofstream(stereo ? "stereo.adts" : "mono.adts",std::ios::binary).write(reinterpret_cast<char*>(adts.data()),adts.size());
  rtsp.running_ = false;
  opus.SetSourceActive(false); http.join(); assert(!opus.EnsureReady()); close(fds[0]); close(fds[1]);
  mic.Stop(); assert(!mic.IsRunning() && mic.SampleRate() == 0 && mic.Channels() == 0);
  assert(!mock::capturers && !mock::builders && !mock::codecs);
}
int main() {
  Pipeline(true); Pipeline(false);
  {
    OpusStream stream;
    stream.SetSourceActive(true, 16000, 1); opusRegistered = false;
    assert(!stream.EnsureReady()); // Unavailable encoder, HTTP route can answer 501.
    opusRegistered = true; stream.SetSourceActive(true, 44100, 2);
    assert(stream.EnsureReady()); // No stale failure cache across format transitions.
  }
  for (int mode = 0; mode < 3; ++mode) {
    mock::badRead = mode == 0; mock::rejectAac = mode == 1;
    if (mode == 2) mock::actualCh = 6;
    mock::attempts.clear(); MicStreamer mic; int errors = 0;
    assert(!mic.Start(0,2,64000,{},[&](auto&) { ++errors; }));
    assert(errors == 1 && !mic.IsRunning() && mock::attempts.size() == 16);
    assert(!mock::capturers && !mock::builders && !mock::codecs);
  }
  // Non-integer resampling must be independent of callback chunking, preserve
  // stereo and downmix arithmetically when only a mono encoder is available.
  OpusPcmConverter cv; Bytes src(44101*4); for (size_t i=0; i<src.size(); ++i) src[i] = i%179;
  Bytes one, many; cv.Reset(44100,2,48000,1); cv.Append(src.data(),44101,one);
  cv.Reset(44100,2,48000,1); for (size_t i=0; i<44101; ++i) cv.Append(src.data()+4*i,1,many);
  assert(one == many && one.size() == 48001*2);
  assert(OpusPcmConverter::Read(one.data()) == (OpusPcmConverter::Read(src.data()) + OpusPcmConverter::Read(src.data()+2))/2);
  std::cout << "PASS: mic fallback/read-back, AAC, SDP, OpusHead/PCM/Ogg, synchronous teardown\n";
}
