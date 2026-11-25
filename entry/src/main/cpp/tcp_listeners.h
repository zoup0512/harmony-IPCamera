#ifndef IPCAMERA_TCP_LISTENERS_H
#define IPCAMERA_TCP_LISTENERS_H

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ipcam {

// Local inspection only: an IPv6-only or loopback listener is not eligible for IPv4 forwarding.
inline int PublicV4ListenerPort(const int (&fds)[2]) {
  for (int fd : fds) {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (fd < 0 || ::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) || ss.ss_family != AF_INET) continue;
    const auto* addr = reinterpret_cast<const sockaddr_in*>(&ss);
    if (addr->sin_addr.s_addr == htonl(INADDR_ANY)) return ntohs(addr->sin_port);
  }
  return 0;
}

// Returns 0 on failure, otherwise the number of nonblocking listener FDs the
// caller owns in fds[0..n). On Stop wake/join the poller before closing them.
// No resolver/AI_ADDRCONFIG or libc-specific IPv4-mapped IPv6 defaults. A
// missing family (including an unavailable ::1) permits single-stack service;
// a port conflict or resource error does not silently create a partial service.
inline int OpenTcpListeners(int port, int backlog, int (&fds)[2], bool loopbackOnly = false) {
  fds[0] = fds[1] = -1;
  if (port < 0 || port > 65535 || backlog <= 0) { errno = EINVAL; return 0; }
  int count = 0;
  const int families[] = {AF_INET6, AF_INET};
  for (const int family : families) {
    int fd = ::socket(family, SOCK_STREAM, 0);
    auto open = [&]() -> bool {
      if (fd < 0) return false;
      const int one = 1;
      if (family == AF_INET6 && ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) < 0) return false;
      int flags = ::fcntl(fd, F_GETFL);
      if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
          ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) return false;
      if (family == AF_INET6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = loopbackOnly ? in6addr_loopback : in6addr_any;
        a.sin6_port = htons(port);
        socklen_t len = sizeof(a);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), len) < 0 ||
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) < 0) return false;
        port = ntohs(a.sin6_port);  // same ephemeral port for both families
      } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        a.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) return false;
      }
      return ::listen(fd, backlog) == 0;
    };
    if (open()) { fds[count++] = fd; continue; }
    const int e = errno;
    if (fd >= 0) ::close(fd);
    if (e == EAFNOSUPPORT || e == EPROTONOSUPPORT || e == EADDRNOTAVAIL || e == ENOPROTOOPT) continue;
    if (count) { ::close(fds[0]); fds[0] = -1; }
    errno = e;
    return 0;
  }
  return count;
}
}  // namespace ipcam

#endif  // IPCAMERA_TCP_LISTENERS_H
