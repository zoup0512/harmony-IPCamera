#ifndef IPCAMERA_MEDIA_TIME_H
#define IPCAMERA_MEDIA_TIME_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <time.h>

namespace ipcam {

using TimestampUs = int64_t;

constexpr TimestampUs kNoTimestampUs = -1;

inline TimestampUs NowMonotonicUs() {
  struct timespec ts {};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  constexpr TimestampUs kUsPerSecond = 1000000;
  return static_cast<TimestampUs>(ts.tv_sec) * kUsPerSecond +
         static_cast<TimestampUs>(ts.tv_nsec / 1000);
}

// Maps a source-local timestamp (normally beginning at zero) onto the process
// monotonic clock while preserving the source's frame spacing.
class RelativeTimestampMapper {
 public:
  void Reset() {
    std::lock_guard<std::mutex> lk(mu_);
    anchored_ = false;
    sourceBaseUs_ = 0;
    lastSourceUs_ = 0;
    monotonicBaseUs_ = 0;
    lastMappedUs_ = kNoTimestampUs;
  }

  TimestampUs Map(TimestampUs sourceUs) {
    if (sourceUs == kNoTimestampUs) return NowMonotonicUs();
    if (sourceUs < 0) return kNoTimestampUs;

    std::lock_guard<std::mutex> lk(mu_);
    if (!anchored_) {
      anchored_ = true;
      sourceBaseUs_ = sourceUs;
      lastSourceUs_ = sourceUs;
      monotonicBaseUs_ = NowMonotonicUs();
      lastMappedUs_ = monotonicBaseUs_;
      return lastMappedUs_;
    }

    if (sourceUs < lastSourceUs_) {
      return lastMappedUs_;
    }
    lastSourceUs_ = sourceUs;

    const TimestampUs delta = sourceUs - sourceBaseUs_;
    TimestampUs mapped = monotonicBaseUs_;
    if (delta > std::numeric_limits<TimestampUs>::max() - monotonicBaseUs_) {
      mapped = std::numeric_limits<TimestampUs>::max();
    } else {
      mapped += delta;
    }
    lastMappedUs_ = std::max(lastMappedUs_, mapped);
    return lastMappedUs_;
  }

 private:
  std::mutex mu_;
  bool anchored_ = false;
  TimestampUs sourceBaseUs_ = 0;
  TimestampUs lastSourceUs_ = 0;
  TimestampUs monotonicBaseUs_ = 0;
  TimestampUs lastMappedUs_ = kNoTimestampUs;
};

}  // namespace ipcam

#endif  // IPCAMERA_MEDIA_TIME_H
