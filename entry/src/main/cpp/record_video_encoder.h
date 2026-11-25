#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ipcam {
// Optional, single-use recorder branch. No camera or live publisher handles.
// Configure/Stop are serialized on the control thread; Feed* may run concurrently.
class RecordVideoEncoder {
 public:
  struct Options { bool h265; int bitrate; int gopMs; };
  // Raw NAL units, without start codes. The input cache must be from THIS source.
  struct Source { bool h265; int width, height; std::vector<uint8_t> vps, sps, pps; };
  using Feed = std::function<void(const uint8_t*, size_t, int64_t)>;
  struct Sinks {
    std::function<void(const Source&)> params;  // actual encoder parameter sets, before first video
    Feed video, audio;  // shared monotonic microsecond timebase; null audio disables AAC
    std::function<void(const std::string&)> fatal;  // worker thread; stop writer, NOT this object
  };
  RecordVideoEncoder();
  ~RecordVideoEncoder();
  bool Configure(const Options&, const Source&, Sinks, std::string& error);
  void FeedVideo(const uint8_t*, size_t, int64_t);
  void FeedAudio(const uint8_t*, size_t, int64_t);
  void Stop();  // drains accepted frames; no more sink calls after return
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace ipcam
