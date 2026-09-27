#ifndef IPCAMERA_RECORDER_H
#define IPCAMERA_RECORDER_H

#include <atomic>
#include <cstdint>
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

  void FeedVideo(const uint8_t* data, size_t size, uint64_t tsUs);  // Annex-B
  void FeedAudio(const uint8_t* adts, size_t size, uint64_t tsUs);  // one ADTS frame

  // Inject codec params captured at stream start (encoders emit them once).
  void SetVideoParams(bool h265, const std::vector<uint8_t>& vps,
                      const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                      int width, int height);

 private:
  struct Item {
    bool audio = false;
    std::vector<uint8_t> data;
    uint64_t tsUs = 0;
  };

  void WriterLoop();
  void CreateMuxer();
  void Enqueue(Item item);

  std::atomic<bool> running_{false};
  std::string filePath_;
  int rotation_ = 0;
  ErrorSink onError_;
  std::mutex errMu_;

  std::mutex queueMu_;
  std::vector<Item> queue_;
  std::thread writerThread_;

  // muxer state, owned by the writer thread
  OH_AVMuxer* muxer_ = nullptr;
  int videoTrack_ = -1;
  int audioTrack_ = -1;
  bool started_ = false;
  bool videoIsH265_ = false;
  bool videoParamsReady_ = false;
  bool audioSeen_ = false;
  struct AudioCfg {
    int profileBits = 1;
    int sfIndex = 4;
    int channels = 2;
    int sampleRate = 44100;
  };
  AudioCfg audioCfg_;

  std::vector<uint8_t> vps_;
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
  int width_ = 0;
  int height_ = 0;
  uint64_t tsBaseUs_ = 0;
  bool tsBaseSet_ = false;
};

}  // namespace ipcam

#endif  // IPCAMERA_RECORDER_H
