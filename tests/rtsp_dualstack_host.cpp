// Used by rtsp_dualstack_test.py and rtsp_interop_test.py --server. The bind
// guard applies to production code too: reject ANY address except ::1/127.0.0.1.
#include "rtsp_server.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>

static int listenerPort = 0;
extern "C" int __real_bind(int, const sockaddr*, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr* addr, socklen_t len) {
  const bool v6 = addr->sa_family == AF_INET6;
  if (v6 ? !IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr) :
      (addr->sa_family != AF_INET || reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr != htonl(INADDR_LOOPBACK)))
    std::abort();
  const char* skip = std::getenv("RTSP_TEST_FAMILY_ERROR");
  if (skip && ((v6 && !strcmp(skip,"no6")) || (!v6 && !strcmp(skip,"no4")))) {
    errno = EADDRNOTAVAIL;
    return -1;
  }
  if (!v6 && skip && !strcmp(skip, "busy4")) { errno = EADDRINUSE; return -1; }
  if (__real_bind(fd, addr, len) < 0) return -1;
  int type = 0;
  socklen_t size = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_STREAM) return 0;
  sockaddr_storage a{};
  size = sizeof(a);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size)) std::abort();
  int port = v6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&a)->sin6_port) : ntohs(reinterpret_cast<sockaddr_in*>(&a)->sin_port);
  if (listenerPort && listenerPort != port) std::abort();
  listenerPort = port;
  return 0;
}

static std::atomic<bool> dropNextAudio{false};
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* buf, size_t size, int flags) {
  const auto* p = static_cast<const uint8_t*>(buf);
  if (size > 12 && p[0] == 0x80 && (p[1] & 127) == 97 && dropNextAudio.exchange(false)) {
    errno = EAGAIN;
    return -1;
  }
  return __real_send(fd, buf, size, flags);
}

int main(int argc, char** argv) {
  ipcam::RtspConfig cfg;
  cfg.port = 0;
  cfg.loopbackOnly = true;
  const bool legacy = argc < 2 || !strcmp(argv[1], "auth");
  if (argc > 1 && !strcmp(argv[1], "auth")) { cfg.user = "viewer"; cfg.password = "test"; }
  ipcam::RtspServer server;
  if (!server.Start(cfg, nullptr)) return 1;
  const uint8_t video[] = {0,0,1,0x65,0x88,0,0,1,0x61,0x88};
  const uint8_t audio[] = {0xff,0xf1,0x50,0x80,1,0x1f,0xfc,1};
  if (legacy) server.PushAdts(audio, sizeof(audio), 0);
  std::cout << listenerPort << std::endl;
  std::string command;
  while (std::getline(std::cin, command)) {
    if (command == "frame") {
      auto ts = ipcam::NowMonotonicUs();
      server.PushH264(video, sizeof(video), ts);
      server.PushAdts(audio, sizeof(audio), ts);
    } else if (command == "reset-audio") {
      server.ResetAudioParams();
      std::cout << "reset" << std::endl;
    } else if (command == "drop-next-audio") {
      dropNextAudio = true;
    } else if (command == "new-audio") {
      const uint8_t mono48k[] = {0xff,0xf1,0x4c,0x40,1,0x1f,0xfc,1};
      server.PushAdts(mono48k, sizeof(mono48k), ipcam::NowMonotonicUs());
      std::cout << "new" << std::endl;
    } else if (command == "restart") {
      server.Stop();
      cfg.port = listenerPort;
      if (!server.Start(cfg, nullptr)) return 3;
      std::cout << listenerPort << std::endl;
    } else return 2;
  }
  server.Stop();
}
