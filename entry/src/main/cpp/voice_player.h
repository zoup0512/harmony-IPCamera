#ifndef IPCAMERA_VOICE_PLAYER_H
#define IPCAMERA_VOICE_PLAYER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ipcam {

// Feed is for streaming ADTS (including split frames). FeedUpload validates one
// complete Android AAC-LC M4A or ADTS message before replacing prior playback.
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
  FeedResult FeedUpload(const uint8_t* data, size_t size);
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
