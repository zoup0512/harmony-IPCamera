#ifndef IPCAMERA_TEST_SOURCE_H
#define IPCAMERA_TEST_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <multimedia/player_framework/native_avcodec_base.h>

#include "media_time.h"

namespace ipcam {

// Color-bar test pattern encoded with the system H.264 hardware/software
// encoder (OH_AVCodec, buffer mode) and pushed into an RtspServer as Annex-B.
// This is the Route-B evaluation piece: capture/encode replaced by system Kit.
class TestPatternSource {
 public:
  using FrameSink = std::function<void(const uint8_t*, size_t, TimestampUs)>;
  using ErrorSink = std::function<void(const std::string&)>;

  TestPatternSource() = default;
  ~TestPatternSource();
  TestPatternSource(const TestPatternSource&) = delete;
  TestPatternSource& operator=(const TestPatternSource&) = delete;

  bool Start(int width, int height, int fps, int bitrate, FrameSink sink, ErrorSink onError);
  void Stop();
  bool IsRunning() const { return running_.load(); }

 private:
  static void OnError(OH_AVCodec* codec, int32_t errorCode, void* userData);
  static void OnStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData);
  static void OnNeedInputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer,
                                void* userData);
  static void OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer,
                                void* userData);

  void FillFrame(uint8_t* dst, size_t capacity, uint64_t ptsUs);
  void WaitNextFrameSlot();

  OH_AVCodec* encoder_ = nullptr;
  std::atomic<bool> running_{false};
  int width_ = 640;
  int height_ = 480;
  int fps_ = 15;
  uint64_t frameIndex_ = 0;
  int64_t nextPtsUs_ = 0;
  std::chrono::steady_clock::time_point startWall_{};
  FrameSink sink_;
  ErrorSink onError_;
  std::mutex stateMu_;
};

}  // namespace ipcam

#endif  // IPCAMERA_TEST_SOURCE_H
