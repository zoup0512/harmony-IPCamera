#ifndef IPCAMERA_FLV_STREAM_H
#define IPCAMERA_FLV_STREAM_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "codec_records.h"
#include "media_time.h"

namespace ipcam {

// Classic FLV only. One encoder callback = one Annex-B AVC access unit or one
// ADTS frame. The live encoder has no reordered pictures (DTS=PTS, CTS=0).
// Call ResetVideoFormat after draining the old encoder and before feeding a new one.
class FlvStream {
  using Bytes = std::vector<uint8_t>;
  using Payload = std::shared_ptr<const Bytes>;
  using Clock = std::chrono::steady_clock;
  static constexpr size_t kMaxAccessUnit = 2 * 1024 * 1024;
  static constexpr size_t kMaxQueuedBytes = 4 * 1024 * 1024;
  static constexpr size_t kMaxTags = 128;
  static constexpr size_t kMaxReaders = 4;

 public:
  struct Tag {
    uint8_t type;
    uint32_t ms;
    Payload body;

    // FLV tag header (including StreamID=0), body, PreviousTagSize.
    Bytes Serialize() const {
      const auto n = static_cast<uint32_t>(body->size());
      Bytes out{type, uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n),
                uint8_t(ms >> 16), uint8_t(ms >> 8), uint8_t(ms), uint8_t(ms >> 24), 0, 0, 0};
      out.reserve(n + 15);
      out.insert(out.end(), body->begin(), body->end());
      Put4(out, n + 11);
      return out;
    }
  };

  class Reader {
    friend class FlvStream;
    std::deque<Tag> queue;
    size_t bytes = 0;
    bool closed = false, needIdr = true, hasAudio = false, headerCommitted = false;
    Clock::time_point audioProbeUntil{};
    TimestampUs base = kNoTimestampUs;
    uint32_t lastAudio = 0, lastVideo = 0;
    Payload audioConfig;
  };
  enum class OpenResult { Ok, NotReady, Busy, Unsupported };
  enum class ReadResult { TagReady, Idle, Closed };

  OpenResult Open(std::shared_ptr<Reader>& reader) {
    std::lock_guard<std::mutex> lock(mu_);
    Prune();
    if (changing_ || readers_.size() >= kMaxReaders) return OpenResult::Busy;
    if (hevc_) return OpenResult::Unsupported;
    if (!videoConfig_) return OpenResult::NotReady;
    reader = std::make_shared<Reader>();
    readers_.push_back(reader);
    return OpenResult::Ok;
  }

  // Caller owns the Reader for the duration of its HTTP worker; no socket I/O
  // under mu_. Short timed waits also notice HTTP stop or a vanished idle peer.
  ReadResult Read(const std::shared_ptr<Reader>& r, Tag& tag, bool& hasAudio) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return r->closed || !r->queue.empty(); });
    if (!r->closed && !r->queue.empty() && !r->headerCommitted && !r->hasAudio) {
      // Video and microphone callbacks may start in either order. Briefly wait
      // for a real AAC frame before committing the immutable FLV track flags.
      cv_.wait_until(lock, r->audioProbeUntil, [&] { return r->closed || r->hasAudio; });
    }
    if (r->closed) return ReadResult::Closed;
    if (r->queue.empty()) return ReadResult::Idle;
    r->headerCommitted = true;
    tag = std::move(r->queue.front());
    r->bytes -= tag.body->size() + 15;
    r->queue.pop_front();
    hasAudio = r->hasAudio;
    return ReadResult::TagReady;
  }

  static Bytes Header(bool audio) {
    return {'F','L','V',1,uint8_t(audio ? 5 : 1),0,0,0,9,0,0,0,0};
  }

  // Diagnostics only; use the atomic reservation below for reconfiguration.
  size_t ViewerCount() {
    std::lock_guard<std::mutex> lock(mu_);
    Prune();
    return readers_.size();
  }

  bool TryBeginVideoChange() {
    std::lock_guard<std::mutex> lock(mu_);
    Prune();
    if (changing_ || !readers_.empty()) return false;
    changing_ = true;
    return true;
  }
  void EndVideoChange() {
    std::lock_guard<std::mutex> lock(mu_);
    changing_ = false;
  }
  void ResetVideoFormat(bool hevc) {
    std::lock_guard<std::mutex> lock(mu_);
    hevc_ = hevc;
    sps_.clear(); pps_.clear(); videoConfig_.reset();
    ForEach([](Reader& r) { Close(r); });
    cv_.notify_all();
  }

  void FeedVideo(const uint8_t* d, size_t n, TimestampUs us) {
    if (!d || us < 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (hevc_) return;
    // A missing reference picture must not be followed by dependent P frames.
    auto lost = [&] { ForEach([](Reader& r) { r.needIdr = true; }); };
    if (n > kMaxAccessUnit || n < 4) { lost(); return; }
    bool changed = false, key = false, slice = false;
    size_t start = n, nalCount = 0;
    Bytes frame;
    Prune();
    auto nal = [&](size_t end) {
      while (end > start && d[end - 1] == 0) --end;
      if (end <= start) return true;
      const size_t len = end - start;
      const uint8_t type = d[start] & 31;
      if (++nalCount > 256 || (d[start] & 0x80)) return false;
      if (type == 7 || type == 8) {
        if (len > 65535 || len < (type == 7 ? 4u : 2u)) return false;
        auto& param = type == 7 ? sps_ : pps_;
        if (len != param.size() || !std::equal(param.begin(), param.end(), d + start)) {
          param.assign(d + start, d + end);
          changed = true;
        }
      } else if (type == 1 || type == 5 || type == 6) {
        key |= type == 5;
        slice |= type != 6;
        if (!readers_.empty()) {
          if (frame.empty()) frame = {0x27, 1, 0, 0, 0};
          Put4(frame, static_cast<uint32_t>(len));
          frame.insert(frame.end(), d + start, d + end);
        }
      }
      return true;
    };
    // Accept both 3- and 4-byte start codes; trim Annex-B trailing_zero_8bits.
    bool valid = true;
    for (size_t i = 0; i + 2 < n; ++i) {
      if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
      if (start != n && !nal(i)) { valid = false; break; }
      i += 2;
      start = i + 1;
    }
    if (!valid || start == n || !nal(n)) {
      if (changed) { sps_.clear(); pps_.clear(); videoConfig_.reset(); }
      lost(); return;
    }
    if (changed) {
      lost();
      if (!sps_.empty() && !pps_.empty()) {
        Bytes body{0x17, 0, 0, 0, 0};
        auto record = AvcDecoderConfigRecord(sps_, pps_);
        body.insert(body.end(), record.begin(), record.end());
        videoConfig_ = std::make_shared<Bytes>(std::move(body));
      }
    }
    if (!slice || !videoConfig_ || frame.empty()) return;
    frame[0] = key ? 0x17 : 0x27;
    auto payload = std::make_shared<Bytes>(std::move(frame));
    ForEach([&](Reader& r) {
      if (r.needIdr) {
        if (!key) return;
        if (r.base < 0) {
          r.base = us;
          r.audioProbeUntil = Clock::now() + std::chrono::milliseconds(200);
        }
        Queue(r, 9, us, videoConfig_);
        r.needIdr = false;
      }
      Queue(r, 9, us, payload);
    });
    cv_.notify_all();
  }

  void FeedAudio(const uint8_t* d, size_t n, TimestampUs us) {
    AdtsInfo a;
    if (us < 0 || !ParseAdts(d, n, &a) || n != static_cast<size_t>(a.frameLen) || (d[6] & 3)) return;
    std::lock_guard<std::mutex> lock(mu_);
    const int config = (a.profileBits << 8) | (a.sfIndex << 4) | a.channels;
    if (config != audioKey_) {
      auto asc = AacConfigBytes(a.profileBits, a.sfIndex, a.channels);
      audioConfig_ = std::make_shared<Bytes>(Bytes{0xaf, 0, asc[0], asc[1]});
      audioKey_ = config;
    }
    Payload payload;
    ForEach([&](Reader& r) {
      if (r.base < 0 || us < r.base) return;
      // A video-only FLV header cannot acquire a new track in place. Finish
      // that response; reconnecting gets an accurate A/V header at the next IDR.
      if (r.needIdr) return;
      if (!r.hasAudio && r.headerCommitted) { Close(r); return; }
      r.hasAudio = true;
      if (r.audioConfig != audioConfig_) {
        Queue(r, 8, us, audioConfig_);
        r.audioConfig = audioConfig_;
      }
      if (!payload) {
        Bytes b{0xaf, 1};
        b.insert(b.end(), d + a.headerLen, d + n);
        payload = std::make_shared<Bytes>(std::move(b));
      }
      Queue(r, 8, us, payload);
    });
    cv_.notify_all();
  }

 private:
  static void Put4(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v));
  }
  static void Close(Reader& r) {
    r.closed = true;
    r.queue.clear();
    r.bytes = 0;
  }
  static void Queue(Reader& r, uint8_t type, TimestampUs us, const Payload& p) {
    if (r.closed) return;
    const size_t size = p->size() + 15;
    if (r.queue.size() >= kMaxTags || size > kMaxQueuedBytes - r.bytes) { Close(r); return; }
    const auto ms = std::max<TimestampUs>(0, us - r.base) / 1000;
    if (ms > UINT32_MAX) { Close(r); return; }
    auto& last = type == 9 ? r.lastVideo : r.lastAudio;
    last = std::max(last, static_cast<uint32_t>(ms));
    r.queue.push_back({type, last, p});
    r.bytes += size;
  }
  void Prune() {
    readers_.erase(std::remove_if(readers_.begin(), readers_.end(),
        [](const auto& w) { return w.expired(); }), readers_.end());
  }
  template<class F> void ForEach(F fn) {
    for (auto& w : readers_) if (auto r = w.lock(); r && !r->closed) fn(*r);
  }

  std::mutex mu_;
  std::condition_variable cv_;
  bool hevc_ = false, changing_ = false;
  Bytes sps_, pps_;
  Payload videoConfig_, audioConfig_;
  int audioKey_ = -1;
  std::vector<std::weak_ptr<Reader>> readers_;
};

}  // namespace ipcam
#endif  // IPCAMERA_FLV_STREAM_H
