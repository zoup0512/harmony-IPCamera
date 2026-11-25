// Built by http_getsnapshot_test.py. Real WebStream, controlled cold decoder and JPEG markers.
#include "jpeg_encoder.h"
#include "web_stream.h"
#include <cassert>
#include <future>
#include <pthread.h>

using namespace std::chrono_literals;
using Bytes = std::vector<uint8_t>;
using ipcam::WebStream;
using Clock = std::chrono::steady_clock;

template<class F> static void Until(F ready) {
  const auto limit = Clock::now() + 3s;
  while (!ready()) {
    assert(Clock::now() < limit);
    std::this_thread::sleep_for(1ms);
  }
}

// Signal only after the request has recorded its fence, without production hooks
// or sleep-based ordering. libc++ and libstdc++ use different timed wait symbols.
static thread_local std::atomic<bool>* registered;
static void SignalWait() {
  if (registered) { registered->store(true); registered = nullptr; }
}
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
  SignalWait(); return __real_pthread_cond_timedwait(c, m, t);
}
#ifdef _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t*, pthread_mutex_t*, clockid_t, const timespec*);
extern "C" int __wrap_pthread_cond_clockwait(pthread_cond_t* c, pthread_mutex_t* m, clockid_t id, const timespec* t) {
  SignalWait(); return __real_pthread_cond_clockwait(c, m, id, t);
}
#endif
static std::future<Bytes> Request(WebStream& s, const std::atomic<bool>& running) {
  std::atomic<bool> waiting{false};
  auto call = std::async(std::launch::async, [&] {
    registered = &waiting;
    return s.WaitForFreshJpeg(running);
  });
  Until([&]{ return waiting.load(); });
  return call;
}
static std::atomic<int> hold{-1}, decoding{-1};
void BeforeDecodedOutput(uint8_t level) {
  decoding = level;
  Until([&]{ return hold != level; });
}
Bytes ipcam::JpegEncoder::EncodeNV12(const uint8_t* nv12, int, int) {
  return nv12[0] == 99 ? Bytes{} : Bytes{nv12[0]};
}
static void Feed(WebStream& s, uint8_t level, uint8_t type = 0x65) {
  Bytes b{0,0,1,type,1,level};
  s.FeedVideo(b.data(), b.size());
}
static void Cancelled(std::future<Bytes>& r) {
  assert(r.wait_for(500ms) == std::future_status::ready && r.get().empty());
}
int main() {
  const Bytes avc{0,0,1,0x67,0x64,0,0x1f,0,0,1,0x68,0xce};
  WebStream s;
  std::atomic<bool> running{true};
  assert(s.WaitForFreshJpeg(running).empty()); // no worker
  s.SetParams(false, avc, 32, 18);
  s.IncClients();
  Feed(s, 1);
  Until([&]{ return s.LatestJpeg() == Bytes{1}; });
  const auto start = Clock::now();
  assert(s.WaitForFreshJpeg(running, 75ms).empty());
  assert(Clock::now() - start >= 75ms && Clock::now() - start < 1s);
  hold = 2;
  Feed(s, 2);
  Until([&]{ return decoding == 2; });
  auto a = Request(s, running), b = Request(s, running);
  hold = -1;
  Until([&]{ return s.LatestJpeg() == Bytes{2}; }); // old input completed AFTER both requests
  hold = 3;
  Feed(s, 3);
  Until([&]{ return decoding == 3; });
  auto later = Request(s, running); // may not share frame 3 with a/b
  hold = -1;
  assert(a.get() == Bytes{3} && b.get() == Bytes{3});
  Feed(s, 4);
  assert(later.get() == Bytes{4});

  auto reset = Request(s, running);
  hold = 5;
  Feed(s, 5);
  Until([&]{ return decoding == 5; });
  s.ResetVideoInput();
  Cancelled(reset); // abort without waiting for codec exit
  s.SetParams(false, avc, 32, 18);
  auto next = Request(s, running);
  Feed(s, 6);
  hold = -1;
  assert(next.get() == Bytes{6});
  auto resize = Request(s, running);
  s.SetParams(false, avc, 64, 32);
  Cancelled(resize);
  auto change = Request(s, running);
  Bytes hevc{0,0,1,0x40,1,2,0,0,1,0x42,1,3,0,0,1,0x44,1,4};
  s.FeedVideo(hevc.data(), hevc.size());
  Cancelled(change);
  auto sameFormat = Request(s, running);
  s.FeedVideo(hevc.data(), hevc.size()); // repeated sets are not a switch
  Feed(s, 7, 0x28); // HEVC IDR_N_LP
  assert(sameFormat.get() == Bytes{7});
  auto cancelled = Request(s, running);
  running = false;
  Cancelled(cancelled);
  running = true;
  s.SetMotionEnabled(true);
  auto noClients = Request(s, running);
  s.DecClients(); s.IncClients();
  Cancelled(noClients); // worker stayed alive; a rapid re-add cannot revive this wait
  s.SetMotionEnabled(false);
  auto stopped = Request(s, running);
  s.DecClients();
  Cancelled(stopped);
  s.IncClients();
  auto resumed = Request(s, running);
  Feed(s, 99, 0x26); // failed JPEG must not relabel frame 7
  Until([&]{ return decoding == 99; });
  Feed(s, 8, 0x26);
  assert(resumed.get() == Bytes{8});
  s.DecClients();
}
