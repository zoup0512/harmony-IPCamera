#ifndef IPCAMERA_OPUS_STREAM_H
#define IPCAMERA_OPUS_STREAM_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <multimedia/player_framework/native_avcodec_base.h>

#include "opus_pcm.h"

namespace ipcam {

// Live Opus audio stream for the web console (/audio.opus), mirroring the
// original Android endpoint. PCM tapped from MicStreamer is encoded with the
// system OH_AudioEncoder (audio/opus — no libopus port needed) and packed
// into an Ogg Opus stream by a small self-written page muxer. Each HTTP
// client gets the pages pushed through its own bounded queue.
//
// The encoder is created when the first client connects and destroyed with
// the last one; without a running microphone the HTTP route answers 503.
class OpusStream {
 public:
  OpusStream() = default;
  ~OpusStream();
  OpusStream(const OpusStream&) = delete;
  OpusStream& operator=(const OpusStream&) = delete;

  // MicStreamer taps raw S16LE interleaved PCM through here.
  void FeedPcm(const uint8_t* pcm, size_t bytes, int sampleRate, int channels);
  // Reflects whether the microphone capture is running (HTTP 503 otherwise).
  void SetSourceActive(bool active, int sampleRate = 48000, int channels = 1);
  bool SourceActive() const { return sourceActive_.load(); }
  unsigned ViewerCount() {
    std::lock_guard<std::mutex> lk(clientsMu_);
    unsigned count = 0;
    for (const auto& client : clients_) if (!client->closed.load()) ++count;
    return count;
  }

  // Encoder availability probe/creation. Returns false when the system has
  // no Opus encoder (route answers 501 "not supported").
  bool EnsureReady();

  // Streams the Ogg Opus flow into fd until the connection drops or the
  // server stops. Returns false when the connection is gone.
  bool Serve(int fd, const std::atomic<bool>& serverRunning);

  // Tears down clients + encoder (server shutdown).
  void Shutdown();

 private:
  struct Client {
    int fd = -1;
    std::deque<std::vector<uint8_t>> pages;
    std::atomic<bool> closed{false};
    uint32_t sequence = 2;  // OpusHead and OpusTags precede the first audio page.
    uint64_t granule = 0;
  };

  static void OnError(OH_AVCodec* codec, int32_t errorCode, void* userData);
  static void OnStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData);
  static void OnNeedInputData(OH_AVCodec* codec, uint32_t index, OH_AVMemory* data,
                              void* userData);
  static void OnNewOutputData(OH_AVCodec* codec, uint32_t index, OH_AVMemory* data,
                              OH_AVCodecBufferAttr* attr, void* userData);

  bool CreateEncoderLocked();
  void DestroyEncoderLocked();
  void CloseClientsLocked();
  void DrainPendingInputs();
  void BroadcastPacket(const std::vector<uint8_t>& packet, unsigned ticks);
  static std::vector<uint8_t> BuildOggPage(uint8_t headerType, uint64_t granule,
                                           uint32_t serial, uint32_t sequence,
                                           const std::vector<uint8_t>& packet);
  static std::vector<uint8_t> BuildOpusHead(int inputRate, int encodedChannels);
  static std::vector<uint8_t> BuildOpusTags();

  // Serialize lifecycle operations, never acquire this from a codec callback.
  std::mutex controlMu_;
  // PCM queue, input slots and published encoder handle.
  std::mutex pcmMu_;
  OH_AVCodec* encoder_ = nullptr;
  std::atomic<bool> encoderFailed_{false};
  bool readyChecked_ = false;
  bool encoderAvailable_ = false;
  bool inputFloat_ = false;  // encoder configured for F32LE input (else S16LE)
  OpusPcmConverter converter_;
  int encRate_ = 48000;
  int encChannels_ = 1;      // encoder channel count (stereo fallback = 2)
  int sampleRate_ = 48000;
  int channels_ = 1;
  uint64_t nextSampleFrame_ = 0;
  std::vector<uint8_t> pcmQueue_;
  struct PendingInput {
    uint32_t index = 0;
    OH_AVMemory* memory = nullptr;
  };
  std::deque<PendingInput> pendingInputs_;
  bool drainActive_ = false;
  bool encoderDestroying_ = false;
  std::condition_variable pcmCv_;
  std::vector<uint8_t> headPages_;  // cached OpusHead+OpusTags ogg pages

  // clients (guarded by clientsMu_)
  std::mutex clientsMu_;
  std::condition_variable clientsCv_;
  std::vector<std::shared_ptr<Client>> clients_;
  uint32_t pageSerial_ = 0x4950434DU;  // 'IPCM'


  std::atomic<bool> sourceActive_{false};
};

}  // namespace ipcam

#endif  // IPCAMERA_OPUS_STREAM_H
