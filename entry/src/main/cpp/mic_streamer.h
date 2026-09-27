#ifndef IPCAMERA_MIC_STREAMER_H
#define IPCAMERA_MIC_STREAMER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <multimedia/player_framework/native_avcodec_base.h>
#include <ohaudio/native_audiocapturer.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audio_common.h>

namespace ipcam {

// Microphone capture (OH_AudioCapturer, PCM S16LE) -> OH_AudioEncoder AAC ->
// ADTS-wrapped frames handed to the RtspServer via FrameSink (rtspSendAdts
// contract). Uses the @since-9 audio encoder data API (OH_AVMemory +
// PushInputData/FreeOutputData) because that is what the audio encoder
// exposes.
class MicStreamer {
 public:
  using FrameSink = std::function<void(const uint8_t*, size_t, uint64_t)>;
  using ErrorSink = std::function<void(const std::string&)>;

  MicStreamer() = default;
  ~MicStreamer();
  MicStreamer(const MicStreamer&) = delete;
  MicStreamer& operator=(const MicStreamer&) = delete;

  bool Start(int sampleRate, int channels, int bitrate, FrameSink sink, ErrorSink onError);
  void Stop();
  bool IsRunning() const { return running_.load(); }

 private:
  void Fail(const std::string& what, int err);

  static int32_t OnReadData(OH_AudioCapturer* capturer, void* userData, void* buffer,
                            int32_t bufferLen);
  static void OnError(OH_AVCodec* codec, int32_t errorCode, void* userData);
  static void OnStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData);
  static void OnNeedInputData(OH_AVCodec* codec, uint32_t index, OH_AVMemory* data,
                              void* userData);
  static void OnNewOutputData(OH_AVCodec* codec, uint32_t index, OH_AVMemory* data,
                              OH_AVCodecBufferAttr* attr, void* userData);

  void Cleanup();

  OH_AVCodec* encoder_ = nullptr;
  OH_AudioStreamBuilder* builder_ = nullptr;
  OH_AudioCapturer* capturer_ = nullptr;
  std::atomic<bool> running_{false};
  int sampleRate_ = 48000;
  int channels_ = 1;
  uint64_t totalSamples_ = 0;
  // Input-buffer handoff: never push zero-size input (the encoder would spin
  // empty output frames). Hold the buffer until the mic callback provides PCM.
  std::mutex pcmMu_;
  std::vector<uint8_t> pcmQueue_;
  bool hasPendingInput_ = false;
  uint32_t pendingIndex_ = 0;
  OH_AVMemory* pendingMem_ = nullptr;
  FrameSink sink_;
  ErrorSink onError_;
  std::mutex stateMu_;
};

}  // namespace ipcam

#endif  // IPCAMERA_MIC_STREAMER_H
