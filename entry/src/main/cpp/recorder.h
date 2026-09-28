#ifndef IPCAMERA_RECORDER_H
#define IPCAMERA_RECORDER_H

#include <atomic>
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
class StreamRecorder {
 public:
  using ErrorSink = std::function<void(const std::string&)>;

  StreamRecorder() = default;
  ~StreamRecorder();
  StreamRecorder(const StreamRecorder&) = delete;
  StreamRecorder& operator=(const StreamRecorder&) = delete;

  // rotation: camera orientation in degrees (written as MP4 rotation matrix).
  bool Start(const std::string& filePath, int rotation, ErrorSink onError);
  void Stop();
  bool IsRecording() const { return running_.load(); }

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

  std::atomic<bool> running_{false};
  std::atomic<bool> accepting_{false};
  std::string filePath_;
  int rotation_ = 0;
  int outputFd_ = -1;
  ErrorSink onError_;
  std::mutex errMu_;
  bool errorReported_ = false;

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
};

}  // namespace ipcam

#endif  // IPCAMERA_RECORDER_H
