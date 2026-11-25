// Fixture for rtsp_interop_test.py. Link with --wrap=bind so that even the
// unauthenticated test server can only listen on the host's loopback address.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cerrno>
#include <iostream>
#include "rtsp_server.h"

static int port;
extern "C" int __real_bind(int, const sockaddr*, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr* addr, socklen_t len) {
  if (addr->sa_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
  auto local = *reinterpret_cast<const sockaddr_in*>(addr);
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (__real_bind(fd, reinterpret_cast<sockaddr*>(&local), len) != 0 ||
      getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) != 0) return -1;
  port = ntohs(local.sin_port);
  return 0;
}

int main(int argc, char**) {
  ipcam::RtspConfig cfg;
  cfg.port = 0;
  if (argc > 1) {
    cfg.user = "viewer";
    cfg.password = "test";
  }
  ipcam::RtspServer server;
  if (!server.Start(cfg, nullptr)) return 1;
  const uint8_t video[] = {0, 0, 1, 0x65, 0x88, 0, 0, 1, 0x61, 0x88};
  const uint8_t audio[] = {0xff, 0xf1, 0x50, 0x80, 1, 0x1f, 0xfc, 1};
  server.PushAdts(audio, sizeof(audio), 0);  // announce audio in SDP
  std::cout << port << std::endl;
  std::string command;
  while (std::getline(std::cin, command)) {
    if (command != "frame") return 2;
    server.PushH264(video, sizeof(video), 1000000);
    server.PushAdts(audio, sizeof(audio), 1000000);
  }
}
