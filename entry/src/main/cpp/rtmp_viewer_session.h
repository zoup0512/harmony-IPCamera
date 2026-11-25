#pragma once
#include "rtmp_viewer_protocol.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace rtmpview {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Session {
  std::atomic<bool> stopped{false}, listen{false};
  std::atomic<int> state{Connecting}, audio{-1};
};
using Addresses = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;
inline Addresses lookup(const Endpoint& ep, int flags) {
  addrinfo hints{}, *a = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV | flags;
  if (getaddrinfo(ep.host.c_str(), ep.port.c_str(), &hints, &a) != 0) a = nullptr;
  return Addresses(a, freeaddrinfo);
}
inline void check(const Session& s, Clock::time_point end) {
  if (s.stopped) throw Stopped;
  if (Clock::now() >= end) throw Timeout;
}
inline std::atomic<unsigned> dnsJobs{0};
struct ReleaseSlot {
  std::atomic<unsigned>& count;
  ~ReleaseSlot() { --count; }
};
inline Addresses resolve(const Session& s, const Endpoint& ep, Clock::time_point end) {
  if (auto a = lookup(ep, AI_NUMERICHOST)) return a;
  // libc DNS cannot be cancelled. A late job owns only host/port, no key, socket or handle.
  if (dnsJobs.fetch_add(1) >= 2) {
    --dnsJobs;
    throw Busy;
  }
  std::future<Addresses> result;
  try {
    std::packaged_task<Addresses()> task([host = ep.host, port = ep.port] {
      ReleaseSlot slot{dnsJobs};
      return lookup({host, port, {}, {}, {}}, 0);
    });
    result = task.get_future();
    std::thread(std::move(task)).detach();
  } catch (...) {
    --dnsJobs;
    throw;
  }
  while (result.wait_for(50ms) != std::future_status::ready) check(s, end);
  check(s, end);
  auto a = result.get();
  if (!a) throw Network;
  return a;
}
class Socket {
  int fd = -1;
  const Session& s;
  std::array<uint8_t, 16384> buffer{};
  size_t pos = 0, size = 0;

 public:
  explicit Socket(const Session& session) : s(session) {}
  ~Socket() {
    if (fd >= 0) close(fd);
  }
  size_t buffered() const { return size - pos; }
  template <class Tick>
  void wait(short events, Tick tick) {
    for (;;) {
      tick();
      pollfd p{fd, events, 0};
      int rc = poll(&p, 1, 50);
      if (rc > 0) {
        if (p.revents & events) return;
        throw Network;
      }
      if (rc < 0 && errno != EINTR) throw Network;
    }
  }
  void open(const Endpoint& ep, Clock::time_point end) {
    auto a = resolve(s, ep, end);
    check(s, end);
    // Pin one resolved address for this selection: no second connect, redirects, or fallback URLs.
    fd = socket(a->ai_family, SOCK_STREAM, 0);
    if (fd < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) throw Network;
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) return;
    if (errno != EINPROGRESS) throw Network;
    wait(POLLOUT, [&] { check(s, end); });
    int err = 0;
    socklen_t n = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) != 0 || err) throw Network;
  }
  void sendAll(const Bytes& b, Clock::time_point end) {
    size_t off = 0;
    end = std::min(end, Clock::now() + 2s);
    while (off < b.size()) {
      wait(POLLOUT, [&] { check(s, end); });
      auto n = send(fd, b.data() + off, b.size() - off, MSG_NOSIGNAL);
      if (n > 0)
        off += size_t(n);
      else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
        throw Network;
    }
  }
  template <class Tick, class Count>
  void read(uint8_t* out, size_t n, Tick tick, Count count) {
    while (n) {
      if (pos == size) {
        wait(POLLIN, tick);
        auto got = recv(fd, buffer.data(), buffer.size(), 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (got <= 0) throw Network;
        pos = 0;
        size = size_t(got);
        count(size);
      }
      size_t take = std::min(n, size - pos);
      std::memcpy(out, buffer.data() + pos, take);
      pos += take;
      out += take;
      n -= take;
    }
  }
};

// Sink is worker-owned. Only a successful output render may refresh lastFrame.
// The same socket/session loop is exercised by the host loopback with a recording sink.
template <class Sink>
void play(Session& s, const Endpoint& ep, Sink& sink) {
  auto deadline = Clock::now() + 10s;
  Socket sock(s);
  sock.open(ep, deadline);
  auto checkStart = [&] { check(s, deadline); };
  auto ignore = [](size_t) {};
  Bytes c(1537);
  c[0] = 3;
  std::mt19937 random(std::random_device{}());
  for (size_t i = 9; i < c.size(); ++i) c[i] = uint8_t(random());
  sock.sendAll(c, deadline);
  uint8_t version;
  sock.read(&version, 1, checkStart, ignore);
  if (version != 3) throw Transport;
  Bytes block(1536);
  sock.read(block.data(), block.size(), checkStart, ignore);
  sock.sendAll(block, deadline);
  sock.read(block.data(), block.size(), checkStart, ignore);
  if (!std::equal(block.begin() + 8, block.end(), c.begin() + 9)) throw Transport;  // simple handshake only
  auto send = [&](uint8_t type, uint8_t cs, uint32_t id, const Bytes& b) {
    sock.sendAll(wire(type, cs, id, b), deadline);
  };
  auto control = [&](uint8_t type, uint32_t value) {
    Bytes b;
    put(b, value, 4);
    send(type, 2, 0, b);
  };
  send(20, 3, 0, connectCommand(ep));
  enum { Connect, Create, Play, Live } stage = Connect;
  uint32_t stream = 0, window = 0;
  uint64_t received = sock.buffered(), acked = 0;  // bytes read alongside S2

  Chunks chunks;
  Avc avc;
  Aac aac;
  auto lastFrame = Clock::now(), nextTick = lastFrame;
  auto tick = [&] {
    check(s, deadline);
    auto now = Clock::now();
    if (stage == Live && now >= nextTick) {
      sink.poll(lastFrame);
      deadline = lastFrame + 12s;
      nextTick = now + 20ms;
    }
  };
  auto ack = [&] {
    if (window && received - acked >= window) {
      control(3, uint32_t(received));
      acked = received;
    }
  };
  auto read = [&](uint8_t* p, size_t n) {
    sock.read(p, n, tick, [&](size_t got) {
      received += got;
      ack();
    });
  };
  auto visit = [&](uint8_t type, uint32_t id, uint32_t ts, const Bytes& b) {
    if (type == 5) {
      window = be32(b.data());
      if (!window) throw Protocol;
      ack();
      return;
    }
    if (type == 6) {
      if (b[4] > 2) throw Protocol;
      control(5, be32(b.data()));
      return;
    }
    if (type == 4) {
      unsigned event = (b[0] << 8) | b[1];
      if (event == 6) {
        if (b.size() != 6) throw Protocol;
        auto pong = b;
        pong[1] = 7;
        send(4, 2, 0, pong);
      }
      if (event == 1 && b.size() == 6 && stream && be32(b.data() + 2) == stream) throw Ended;
      return;
    }
    if (type == 17 || type == 20) {
      ipcam::RtmpCommand cmd;
      if (!ipcam::AmfReplyReader(b).Read(cmd, type == 17)) throw Protocol;
      bool tx = id == 0 &&
                ((stage == Connect && cmd.transaction == 1) || (stage == Create && cmd.transaction == 2));
      if (cmd.name == "_error" &&
          (tx || (stage >= Play && cmd.transaction == 0 && (id == 0 || id == stream))))
        throw Rejected;
      if (cmd.name == "_result" && tx) {
        if (cmd.level == "error") throw Rejected;
        if (stage == Connect) {
          if (cmd.code != "NetConnection.Connect.Success") throw Rejected;
          auto create = command("createStream", 2);
          create.push_back(5);
          send(20, 3, 0, create);
          stage = Create;
        } else {
          if (!cmd.numericResult || cmd.result < 1 || cmd.result > UINT32_MAX ||
              std::floor(cmd.result) != cmd.result)
            throw Protocol;
          stream = uint32_t(cmd.result);
          auto req = command("play", 0);
          req.push_back(5);
          str(req, ep.stream);
          num(req, -1);
          send(20, 8, stream, req);
          stage = Play;
        }
      }
      if (cmd.name == "onStatus" && !id && cmd.code.rfind("NetConnection.Connect.", 0) == 0 &&
          cmd.code != "NetConnection.Connect.Success")
        throw Rejected;  // includes redirection
      if (cmd.name == "onStatus" && stage >= Play && id == stream && cmd.transaction == 0) {
        if (cmd.code == "NetStream.Play.StreamNotFound") throw NotFound;
        if (cmd.code == "NetStream.Play.Stop" || cmd.code == "NetStream.Play.UnpublishNotify" ||
            cmd.code == "NetStream.Play.Complete")
          throw Ended;
        if (cmd.level == "error" || cmd.code == "NetStream.Play.Failed" ||
            cmd.code == "NetStream.Play.FileStructureInvalid" ||
            cmd.code == "NetStream.Play.NoSupportedTrackFound")
          throw Rejected;
        if (stage == Play && cmd.code == "NetStream.Play.Start") {
          stage = Live;
          lastFrame = Clock::now();
          deadline = lastFrame + 12s;
        }
      }
      return;
    }
    if (stage != Live || id != stream) return;
    if (type == 9)
      avc.packet(b, ts, [&](const Bytes& au, int64_t pts, bool config) {
        sink.video(au, pts, config, lastFrame);
        deadline = lastFrame + 12s;
      });
    if (type == 8) {
      Bytes adts;
      auto result = aac.packet(b, adts);
      sink.audioPacket(result, adts);
    }
  };
  for (;;) {
    tick();
    chunks.next(read, visit);
  }
}
}  // namespace rtmpview
