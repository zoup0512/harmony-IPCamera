#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace ipcam {
// One local publisher. No bind/listen, pathname, public fd or TCP relay. All JS
// side operations are nonblocking under the lock; Take transfers the other fd.
class RtmpTlsPipe {
 public:
  static constexpr size_t kRead = 32768, kMaxInput = 256 * 1024;
  static RtmpTlsPipe& Instance() { static RtmpTlsPipe p; return p; }
  uint32_t Open(const std::string& authority, void* owner) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!owner || authority.empty() || authority.size() > 512 || js_ >= 0 || serial_ == UINT32_MAX) return 0;
    int f[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, f) != 0) return 0;
    int cap = 64 * 1024;
    for (int fd : f) {
      if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cap, sizeof(cap)) ||
          setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cap, sizeof(cap))) {
        ::close(f[0]); ::close(f[1]); return 0;
      }
    }
    authority_ = authority; owner_ = owner; js_ = f[0]; protocol_ = f[1]; return ++serial_;
  }
  bool IsOwned(uint32_t token, void* owner) {
    std::lock_guard<std::mutex> lock(mu_);
    return Matches(token, owner) && protocol_ >= 0;
  }
  int Take(uint32_t token, const std::string& authority) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!token || token != serial_ || authority != authority_ || js_ < 0) return -1;
    const int fd = protocol_; protocol_ = -1; return fd;
  }
  // 0 = idle, -1 = stale/closed/EOF.
  ssize_t Read(uint32_t token, void* owner, void* data, size_t len) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!Matches(token, owner)) return -1;
    ssize_t n = ::recv(js_, data, std::min(len, kRead), MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return n == 0 ? -1 : n;
  }
  bool Write(uint32_t token, void* owner, const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!Matches(token, owner) || !len || len > kMaxInput) return false;
    // Fail the session on short write/backpressure rather than queue or
    // continue with a gap in the decrypted byte stream.
    return ::send(js_, data, len, MSG_DONTWAIT | MSG_NOSIGNAL) == static_cast<ssize_t>(len);
  }
  void Close(uint32_t token, void* owner) {
    std::lock_guard<std::mutex> lock(mu_);
    if (Matches(token, owner)) CloseLocked();
  }
  void Cleanup(void* owner) {
    std::lock_guard<std::mutex> lock(mu_);
    if (js_ >= 0 && owner_ == owner) CloseLocked();
  }
 private:
  bool Matches(uint32_t token, void* owner) const { return token && token == serial_ && owner == owner_ && js_ >= 0; }
  void CloseLocked() {
    ::shutdown(js_, SHUT_RDWR); ::close(js_); js_ = -1;
    if (protocol_ >= 0) ::close(protocol_);
    protocol_ = -1; authority_.clear(); owner_ = nullptr;
  }
  std::mutex mu_;
  int js_ = -1, protocol_ = -1;
  void* owner_ = nullptr;
  uint32_t serial_ = 0;
  std::string authority_;
};
} // namespace ipcam
