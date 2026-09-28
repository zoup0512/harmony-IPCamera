#ifndef IPCAMERA_RECORDER_H
#define IPCAMERA_RECORDER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct OH_AVMuxer;

namespace ipcam {

// MP4 recording of the shared encoded streams (H.264/H.265 + AAC) via
// OH_AVMuxer. Video/audio samples arrive Annex-B / ADTS exactly like the RTSP
// and RTMP paths; the muxer track is created once codec parameters are known.
// Written by a dedicated thread so encoder callbacks never block on disk.
// Mirrors the Android recording enhancements: four_gb_limit closes the file
// before the MP4 32-bit sample offsets overflow, each_segment_length rotates
// to a numbered segment every N minutes (ipc_<ts>.mp4 -> ipc_<ts>_2.mp4 ...).
class StreamRecorder {
 public:
  using ErrorSink = std::function<void(const std::string&)>;

  struct Options {
    bool fourGbLimit = true;    // four_gb_limit
    int segmentMinutes = 10;    // each_segment_length, 0 disables time rotation
  };

  StreamRecorder() = default;
  ~StreamRecorder();
  StreamRecorder(const StreamRecorder&) = delete;
  StreamRecorder& operator=(const StreamRecorder&) = delete;

  // rotation: camera orientation in degrees (written as MP4 rotation matrix).
  bool Start(const std::string& filePath, int rotation, ErrorSink onError);
  void Stop();
  bool IsRecording() const { return running_.load(); }
  void SetOptions(const Options& options);

  // tsUs is an absolute monotonic timestamp in microseconds; -1 means missing.
  void FeedVideo(const uint8_t* data, size_t size, int64_t tsUs);  // Annex-B
  void FeedAudio(const uint8_t* adts, size_t size, int64_t tsUs);  // one ADTS frame

  // Inject codec params captured at stream start (encoders emit them once).
  void SetVideoParams(bool h265, const std::vector<uint8_t>& vps,
                      const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                      int width, int height);

 private:
  struct Item {
    bool audio = false;
    std::vector<uint8_t> data;
    int64_t tsUs = -1;
  };

  struct VideoParams {
    bool h265 = false;
    bool ready = false;
    std::vector<uint8_t> vps;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    int width = 0;
    int height = 0;
  };

  struct AudioCfg {
    int profileBits = 1;
    int sfIndex = 4;
    int channels = 2;
    int sampleRate = 44100;
  };

  void WriterLoop();
  bool CreateMuxer(const VideoParams& videoParams, bool audioSeen,
                   const AudioCfg& audioCfg);
  bool WriteItem(const Item& item, bool videoIsH265);
  bool Enqueue(Item item);
  void ReportFatalError(const std::string& message);
  void DestroyMuxerAndClose();
  // Closes the current file and opens the next numbered segment reusing the
  // active codec params; the timestamp base resets on the next written item.
  bool RotateSegment(const VideoParams& videoParams, bool audioSeen,
                     const AudioCfg& audioCfg);
  bool ShouldRotate() const;
  static std::string SegmentPath(const std::string& basePath, int index);

  std::atomic<bool> running_{false};
  std::atomic<bool> accepting_{false};
  std::string filePath_;
  int rotation_ = 0;
  int outputFd_ = -1;
  ErrorSink onError_;
  std::mutex errMu_;
  bool errorReported_ = false;
  Options options_;

  std::mutex queueMu_;
  std::condition_variable queueCv_;
  std::deque<Item> queue_;
  VideoParams configuredVideoParams_;
  uint64_t videoParamsGeneration_ = 0;
  std::thread writerThread_;

  // muxer state, owned by the writer thread
  OH_AVMuxer* muxer_ = nullptr;
  int videoTrack_ = -1;
  int audioTrack_ = -1;
  bool started_ = false;
  int64_t tsBaseUs_ = 0;
  bool tsBaseSet_ = false;
  int64_t lastVideoPtsUs_ = -1;
  int64_t lastAudioPtsUs_ = -1;
  bool pendingBaseReset_ = false;
  // segmentation state, owned by the writer thread
  int segmentIndex_ = 0;
  uint64_t segmentBytes_ = 0;
  std::chrono::steady_clock::time_point segmentStart_;
  VideoParams activeVideoParams_;
  AudioCfg activeAudioCfg_;
  bool activeAudioSeen_ = false;
};

}  // namespace ipcam

#endif  // IPCAMERA_RECORDER_H
