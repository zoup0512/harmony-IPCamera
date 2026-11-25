// Linux/SDK POSIX host: c++ -std=c++17 -pthread -Ientry/src/main/cpp tests/rtmp_tls_pipe_test.cpp
#include "rtmp_tls_pipe.h"
#include <cassert>
#include <chrono>
#include <future>
#include <poll.h>
#include <vector>

int main() {
  auto& p = ipcam::RtmpTlsPipe::Instance(); char a, b, data[32]{};
  const auto first = p.Open("localhost:443", &a); assert(first);
  assert(p.IsOwned(first, &a) && !p.IsOwned(first, &b));
  assert(!p.Open("other", &b) && p.Take(first, "other") < 0);
  const int fd = p.Take(first, "localhost:443"); assert(fd >= 0 && p.Take(first, "localhost:443") < 0);
  assert(p.Read(first, &b, data, sizeof(data)) < 0 && p.Read(first, &a, data, sizeof(data)) == 0);
  assert(send(fd, "ok", 2, MSG_NOSIGNAL) == 2 && p.Read(first, &a, data, sizeof(data)) == 2);
  assert(p.Write(first, &a, "OK", 2) && recv(fd, data, 2, 0) == 2 && data[0] == 'O');
  std::promise<void> ready; auto blocked = std::async(std::launch::async, [&] {
    pollfd x{fd, POLLIN, 0}; ready.set_value(); return poll(&x, 1, 5000);
  });
  ready.get_future().wait(); p.Close(first, &a);
  assert(blocked.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready && blocked.get() > 0);
  assert(recv(fd, data, 1, 0) == 0); close(fd);
  auto second = p.Open("localhost", &a); assert(second && second != first);
  p.Close(first, &a); p.Close(second, &b); p.Cleanup(&b);
  assert(p.Read(first, &a, data, 1) == -1 && !p.Write(first, &a, "x", 1));
  int peer = p.Take(second, "localhost"); assert(peer >= 0);
  std::vector<char> flood(32768); size_t writes = 0;
  assert(!p.Write(second, &a, flood.data(), p.kMaxInput + 1));
  while (p.Write(second, &a, flood.data(), flood.size())) assert(++writes < 16);
  p.Cleanup(&a); close(peer);
}
