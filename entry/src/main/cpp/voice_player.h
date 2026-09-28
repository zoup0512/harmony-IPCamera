#ifndef IPCAMERA_VOICE_PLAYER_H
#define IPCAMERA_VOICE_PLAYER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ipcam {

// Talk-back playback for /put_voice: HTTP bodies contain AAC ADTS bytes. Feed
// accepts arbitrary chunk boundaries and queues complete raw AAC frames for the
// system decoder.
class VoicePlayer {
 public:
  enum class FeedResult {
    Accepted,
    Invalid,
    Busy,
    Unavailable,
  };

  VoicePlayer();
  ~VoicePlayer();

  FeedResult Feed(const uint8_t* data, size_t size);
  void Stop();
  bool Active() const { return active_.load(); }

  struct Impl;

 private:
  std::mutex apiMu_;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> active_{false};
};

}  // namespace ipcam

#endif  // IPCAMERA_VOICE_PLAYER_H
