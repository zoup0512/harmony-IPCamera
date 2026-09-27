#ifndef IPCAMERA_VOICE_PLAYER_H
#define IPCAMERA_VOICE_PLAYER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ipcam {

// Talk-back playback for /put_voice: HTTP body is a stream of AAC ADTS
// frames; frames are decoded with the system AAC decoder and the PCM is
// played through OH_AudioRenderer (callback-driven pull from a ring buffer).
class VoicePlayer {
 public:
  ~VoicePlayer();
  // Feed a chunk of the uploaded body (ADTS frames; may contain partial
  // frames — chunks are concatenated internally).
  void Feed(const uint8_t* data, size_t size);
  void Stop();
  bool Active() const { return active_.load(); }

  struct Impl;

 private:
  Impl* impl_ = nullptr;
  std::atomic<bool> active_{false};
};

}  // namespace ipcam

#endif  // IPCAMERA_VOICE_PLAYER_H
