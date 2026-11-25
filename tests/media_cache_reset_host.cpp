// Built by media_cache_reset_test.py against production RTSP/Web/Snapshot code.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <thread>

#include "jpeg_encoder.h"
#include "rtsp_server.h"
#include "snapshot.h"
#include "web_stream.h"

using namespace std::chrono_literals;
using Bytes = std::vector<uint8_t>;
const Bytes avc{0,0,1,0x67,0x64,0,0x1f, 0,0,1,0x68,0xce};
static int port;
extern "C" int __real_bind(int, const sockaddr*, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr* addr, socklen_t len) {
  if (addr->sa_family == AF_INET6) { errno = EAFNOSUPPORT; return -1; }
  assert(addr->sa_family == AF_INET);
  auto local = *reinterpret_cast<const sockaddr_in*>(addr);
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (__real_bind(fd, reinterpret_cast<sockaddr*>(&local), len) < 0 ||
      getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) < 0) return -1;
  port = ntohs(local.sin_port);
  return 0;
}
template<class F> void Wait(F ready) {
  const auto until = std::chrono::steady_clock::now() + 5s;
  while (!ready()) {
    assert(std::chrono::steady_clock::now() < until);
    std::this_thread::sleep_for(2ms);
  }
}
std::atomic<bool> pauseAdmission{false}, accepted{false};
extern "C" int __real_accept(int fd, sockaddr* a, socklen_t* len);
extern "C" int __wrap_accept(int fd, sockaddr* a, socklen_t* len) {
  int client = __real_accept(fd, a, len);
  if (client >= 0 && pauseAdmission) {
    accepted = true;
    Wait([&]{ return !pauseAdmission; });
  }
  return client;
}
int Connect() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
  return fd;
}
std::string Describe(int fd) {
  const std::string msg = "DESCRIBE rtsp://127.0.0.1:" + std::to_string(port) +
                          "/live RTSP/1.0\r\nCSeq: 1\r\n\r\n";
  assert(send(fd, msg.data(), msg.size(), 0) == static_cast<ssize_t>(msg.size()));
  std::string result;
  char buf[2048];
  for (;;) {
    auto n = recv(fd, buf, sizeof(buf), 0);
    assert(n > 0);
    result.append(buf, n);
    const auto p = result.find("\r\n\r\n");
    if (p == std::string::npos) continue;
    assert(result.find("RTSP/1.0 200 OK") == 0);
    const auto cl = result.find("Content-Length: ");
    assert(cl != std::string::npos);
    if (result.size() >= p + 4 + std::stoul(result.substr(cl + 16))) return result.substr(p + 4);
  }
}
void TestRtsp() {
  ipcam::RtspServer s;
  ipcam::RtspConfig cfg;
  cfg.port = 0;
  std::atomic<int> connected{0}, disconnected{0};
  assert(s.TryBeginVideoChange());  // Can also acquire before Start.
  assert(s.Start(cfg, [&](ipcam::RtspEvent e, int, const std::string&) {
    if (e == ipcam::RtspEvent::kClientConnected) ++connected;
    if (e == ipcam::RtspEvent::kClientDisconnected) ++disconnected;
  }));
  assert(!s.TryBeginVideoChange());
  int fd = Connect();
  char c;
  assert(recv(fd, &c, 1, 0) == 0);
  close(fd);
  assert(connected == 0 && disconnected == 0 && s.ClientCount() == 0);
  s.EndVideoChange();
  pauseAdmission = true;
  fd = Connect();
  Wait([&]{ return accepted.load(); });  // TCP accepted, session not yet registered.
  assert(s.TryBeginVideoChange());
  pauseAdmission = false;
  assert(recv(fd, &c, 1, 0) == 0);
  close(fd);
  assert(connected == 0 && disconnected == 0);
  s.EndVideoChange();
  fd = Connect();
  Wait([&]{ return connected == 1; });
  assert(!s.TryBeginVideoChange()); // Even before the first RTSP request.
  auto push = [&](Bytes b) { s.PushH264(b.data(), b.size(), 0); };
  push({0,0,1,0x40,1,2, 0,0,1,0x42,1,3});
  const Bytes adts{0xff,0xf1,0x50,0x80,1,0x1f,0xfc,1};
  s.PushAdts(adts.data(), adts.size(), 0);
  assert(!s.GetVideoParams().ready && Describe(fd).find("sprop-vps") == std::string::npos);
  push({0,0,1,0x44,1,4});
  assert(s.GetVideoParams().ready && s.GetVideoParams().h265);
  auto before = Describe(fd);
  assert(before.find("sprop-vps=") != std::string::npos && before.find(";sprop-pps=") != std::string::npos);
  close(fd);
  Wait([&]{ return s.TryBeginVideoChange(); });
  s.ResetVideoParams();
  auto p = s.GetVideoParams();
  assert(!p.ready && !p.h265 && p.vps.empty() && p.sps.empty() && p.pps.empty());
  assert(s.IsRunning());
  s.ResetVideoParams(true); // Hint codec for early DESCRIBE, not parameter readiness.
  assert(s.GetVideoParams().h265 && !s.GetVideoParams().ready);
  s.EndVideoChange();
  fd = Connect();
  auto after = Describe(fd);
  const auto audio = before.find("m=audio ");
  assert(audio != std::string::npos && after.find(before.substr(audio)) != std::string::npos);
  assert(after.find("H265/90000") != std::string::npos && after.find("sprop-") == std::string::npos);
  push({0,0,1,0x67,0x64,0,0x1f}); // Actual AVC anchor overrides the HEVC hint.
  auto partial = Describe(fd);
  assert(!s.GetVideoParams().ready && partial.find("H264/90000") != std::string::npos);
  const auto partialVideo = partial.substr(0, partial.find("m=audio "));
  assert(partialVideo.find("sprop-") == std::string::npos && partialVideo.find("profile-level-id") == std::string::npos);
  push({0,0,1,0x68,0xce});
  assert(s.GetVideoParams().ready && !s.GetVideoParams().h265);
  assert(Describe(fd).find("sprop-parameter-sets=") != std::string::npos);
  close(fd);
  s.Stop();
}

// A controlled publication boundary: freeze old work after decode but before
// it can publish its JPEG or sample motion. No production test hooks needed.
std::atomic<int> ticket{0}, allow{0};
Bytes ipcam::JpegEncoder::EncodeNV12(const uint8_t* nv12, int, int) {
  int mine = ++ticket;
  Wait([&]{ return allow >= mine; });
  return {nv12[0]};
}
template<class T> void FeedIdr(T& obj, uint8_t level) {
  const uint8_t idr[]{0,0,1,0x65,level};
  obj.FeedVideo(idr, sizeof(idr));
}
void TestWeb() {
  ipcam::WebStream s;
  s.SetMotionTimeout(60);
  s.SetMotionEnabled(true);
  assert(s.MotionEnabled() && !s.MotionActive());
  s.IncClients();
  s.SetParams(false, avc, 32, 18);
  FeedIdr(s, 12);
  allow = 1;
  Wait([&]{ return s.LatestJpeg() == Bytes{12}; });
  std::this_thread::sleep_for(10s); // Production detector's arm delay.
  FeedIdr(s, 240);
  allow = 2;
  Wait([&]{ return s.MotionActive(); });
  FeedIdr(s, 14);
  Wait([&]{ return ticket == 3; });
  s.ResetVideoInput();
  assert(s.LatestJpeg().empty() && !s.MotionActive() && s.MotionEnabled());
  s.SetParams(false, avc, 32, 18);
  FeedIdr(s, 245);
  allow = 3;
  Wait([&]{ return ticket == 4; });
  assert(s.LatestJpeg().empty()); // Old worker completed; new JPEG still blocked.
  allow = 4;
  Wait([&]{ return s.LatestJpeg() == Bytes{245}; });
  FeedIdr(s, 8);
  Wait([&]{ return ticket == 5; });
  assert(!s.MotionActive()); // First new frame seeded a fresh baseline.
  allow = 5;
  Wait([&]{ return s.MotionActive(); }); // Still armed, no client reconnect/re-enable.
  s.DecClients();
  s.SetMotionEnabled(false);
}
std::atomic<bool> decoding{false}, finishDecode{false};
void BeforeDecodedOutput(uint8_t level) {
  if (level == 41) {
    decoding = true;
    Wait([&]{ return finishDecode.load(); });
  }
}
void TestSnapshot() {
  ipcam::Snapshotter s;
  s.SetParams(false, avc);
  FeedIdr(s, 41);
  std::atomic<bool> entered{false}, release{false};
  s.CaptureAsync("old.bmp", 32, 18, [&](bool ok, const std::string& path) {
    assert(ok && path == "old.bmp" && s.IsBusy());
    entered = true;
    Wait([&]{ return release.load(); });
    s.ResetVideoInput();  // Reentrant: another thread may be joining via Stop.
  });
  Wait([&]{ return decoding.load(); });
  assert(s.IsBusy());
  s.ResetVideoInput();   // Old BMP may still be written for this accepted request.
  finishDecode = true;
  Wait([&]{ return entered.load(); });
  s.ResetVideoInput();   // Busy callback does not block cache reset or get cancelled.
  assert(s.IsBusy());
  std::thread joiner([&]{ s.Stop(); });
  release = true;
  joiner.join();
  assert(!s.IsBusy());
  auto noFrame = [&]{
    bool rejected = false;
    s.CaptureAsync("bad.bmp", 32, 18, [&](bool ok, const std::string& msg) {
      rejected = !ok && msg == "no keyframe cached yet";
    });
    assert(rejected);
  };
  FeedIdr(s, 33);
  noFrame(); // Parameters did not survive reset.
  s.ResetVideoInput();
  s.SetParams(false, avc);
  noFrame(); // IDR did not survive reset either.
  FeedIdr(s, 210);
  bool saved = false;
  s.CaptureAsync("new.bmp", 32, 18, [&](bool ok, const std::string&){ saved = ok; });
  s.Stop();
  assert(saved && !s.IsBusy());
}
int main() {
  TestRtsp();
  TestSnapshot();
  TestWeb();
  std::cout << "PASS: RTSP admission/SDP/AAC, in-flight snapshot, stale JPEG and MD baseline\n";
}
