#ifndef IPCAMERA_SNAPSHOT_H
#define IPCAMERA_SNAPSHOT_H

#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ipcam {

// Caches the most recent codec parameter sets + IDR frame from the shared
// encoded stream and, on demand, decodes that frame to NV12 with the system
// video decoder and writes a 24-bit BMP. Runs its decode on a worker thread
// so ArkTS never blocks.
class Snapshotter {
 public:
  void FeedVideo(const uint8_t* data, size_t size);  // Annex-B, both codecs
  // Inject parameter sets captured at stream start (Annex-B built by caller).
  void SetParams(bool h265, const std::vector<uint8_t>& paramsAnnexB);
  // Asynchronous: spawns a thread, reports via the returned... fire-and-forget.
  void CaptureAsync(const std::string& bmpPath, int width, int height,
                    std::function<void(bool, const std::string&)> done);

 private:
  void DecodeWorker(std::string bmpPath, int width, int height,
                    std::function<void(bool, const std::string&)> done,
                    std::vector<uint8_t> params, std::vector<uint8_t> idr, bool h265);

  std::mutex stateMu_;
  bool h265_ = false;
  std::vector<uint8_t> params_;  // Annex-B: VPS+SPS+PPS or SPS+PPS
  std::vector<uint8_t> lastIdr_; // Annex-B IDR frame
};

}  // namespace ipcam

#endif  // IPCAMERA_SNAPSHOT_H
