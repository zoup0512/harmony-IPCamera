#include "snapshot.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmemory.h>

#include "codec_records.h"

#define LOG_DOMAIN 0xC010
#define LOG_TAG "Snapshotter"

namespace ipcam {
namespace {

constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};

struct NalView {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

std::vector<NalView> SplitAnnexB(const uint8_t* data, size_t size) {
  std::vector<NalView> nals;
  size_t i = 0;
  size_t nalStart = std::string::npos;
  while (i + 3 <= size) {
    if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
      if (nalStart != std::string::npos && i > nalStart) {
        nals.push_back({data + nalStart, i - nalStart});
      }
      i += 3;
      nalStart = i;
    } else {
      ++i;
    }
  }
  if (nalStart != std::string::npos && size > nalStart) {
    nals.push_back({data + nalStart, size - nalStart});
  }
  return nals;
}

void AppendNal(std::vector<uint8_t>& v, const uint8_t* d, size_t n) {
  v.insert(v.end(), kStartCode, kStartCode + 4);
  v.insert(v.end(), d, d + n);
}

// NV12 (W*H Y plane + interleaved UV) -> 24-bit bottom-up BMP.
bool WriteBmp(const std::string& path, const uint8_t* nv12, int w, int h) {
  int rowBytes = (w * 3 + 3) & ~3;
  int dataSize = rowBytes * h;
  std::vector<uint8_t> bmp(54 + dataSize, 0);
  bmp[0] = 'B';
  bmp[1] = 'M';
  uint32_t fileSize = 54 + dataSize;
  memcpy(&bmp[2], &fileSize, 4);
  uint32_t dataOff = 54;
  memcpy(&bmp[10], &dataOff, 4);
  uint32_t hdrSize = 40;
  memcpy(&bmp[14], &hdrSize, 4);
  memcpy(&bmp[18], &w, 4);
  memcpy(&bmp[22], &h, 4);
  uint16_t planes = 1;
  memcpy(&bmp[26], &planes, 2);
  uint16_t bpp = 24;
  memcpy(&bmp[28], &bpp, 2);
  memcpy(&bmp[34], &dataSize, 4);

  const uint8_t* yPlane = nv12;
  const uint8_t* uvPlane = nv12 + static_cast<size_t>(w) * h;
  for (int row = 0; row < h; ++row) {
    int y = h - 1 - row;  // BMP is bottom-up
    uint8_t* dst = bmp.data() + 54 + static_cast<size_t>(row) * rowBytes;
    for (int x = 0; x < w; ++x) {
      float yy = yPlane[static_cast<size_t>(y) * w + x];
      float u = uvPlane[static_cast<size_t>(y / 2) * w + (x & ~1)];
      float v = uvPlane[static_cast<size_t>(y / 2) * w + (x & ~1) + 1];
      int r = static_cast<int>(1.164f * (yy - 16) + 1.596f * (v - 128));
      int g = static_cast<int>(1.164f * (yy - 16) - 0.391f * (u - 128) - 0.813f * (v - 128));
      int b = static_cast<int>(1.164f * (yy - 16) + 2.018f * (u - 128));
      dst[x * 3 + 0] = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
      dst[x * 3 + 1] = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
      dst[x * 3 + 2] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
    }
  }
  int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return false;
  bool ok = write(fd, bmp.data(), bmp.size()) == static_cast<ssize_t>(bmp.size());
  close(fd);
  return ok;
}

// One decode session: fed params + IDR, yields the first NV12 frame.
struct DecodeSession {
  std::mutex mu;
  std::condition_variable cv;
  OH_AVCodec* dec = nullptr;
  std::vector<std::vector<uint8_t>> inputs;  // Annex-B chunks to push
  bool eosQueued = false;
  bool failed = false;
  bool gotFrame = false;
  std::vector<uint8_t> frame;
};

void OnNeedInput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData) {
  auto* s = static_cast<DecodeSession*>(userData);
  std::lock_guard<std::mutex> lk(s->mu);
  OH_AVCodecBufferAttr attr{};
  if (!s->inputs.empty()) {
    std::vector<uint8_t>& in = s->inputs.front();
    memcpy(OH_AVBuffer_GetAddr(buffer), in.data(), in.size());
    attr.size = static_cast<int32_t>(in.size());
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    s->inputs.erase(s->inputs.begin());
  } else if (!s->eosQueued) {
    attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
    s->eosQueued = true;
  } else {
    return;  // no more work; leave the buffer (decoder will be stopped anyway)
  }
  OH_AVBuffer_SetBufferAttr(buffer, &attr);
  OH_VideoDecoder_PushInputBuffer(s->dec, index);
}

void OnNewOutput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData) {
  auto* s = static_cast<DecodeSession*>(userData);
  OH_AVCodecBufferAttr attr{};
  OH_AVBuffer_GetBufferAttr(buffer, &attr);
  if (!(attr.flags & AVCODEC_BUFFER_FLAGS_EOS) && attr.size > 0 && !s->gotFrame) {
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->gotFrame) {
      s->frame.assign(OH_AVBuffer_GetAddr(buffer), OH_AVBuffer_GetAddr(buffer) + attr.size);
      s->gotFrame = true;
      s->cv.notify_all();
    }
  }
  OH_VideoDecoder_FreeOutputBuffer(s->dec, index);
}

void OnDecError(OH_AVCodec*, int32_t, void* userData) {
  auto* s = static_cast<DecodeSession*>(userData);
  std::lock_guard<std::mutex> lk(s->mu);
  s->failed = true;
  s->cv.notify_all();
}

void OnDecStreamChange(OH_AVCodec*, OH_AVFormat*, void*) {}

}  // namespace

Snapshotter::~Snapshotter() { Stop(); }

void Snapshotter::FeedVideo(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 5) return;
  std::vector<NalView> nals = SplitAnnexB(data, size);
  std::lock_guard<std::mutex> lk(stateMu_);
  std::vector<uint8_t> idr;
  for (auto& nal : nals) {
    while (nal.size > 1 && nal.data[nal.size - 1] == 0x00) nal.size--;
    if (nal.size < 2) continue;
    uint8_t h264Type = nal.data[0] & 0x1F;
    uint8_t h265Type = static_cast<uint8_t>((nal.data[0] >> 1) & 0x3F);
    bool h265Param = (h265Type == 32 || h265Type == 33 || h265Type == 34) &&
                     nal.data[1] == 0x01;
    if (h265Param) {
      h265_ = true;
      AppendNal(params_, nal.data, nal.size);
      continue;
    }
    if (h264Type == 7 || h264Type == 8) {
      h265_ = false;
      AppendNal(params_, nal.data, nal.size);
      continue;
    }
    if ((h265_ && (h265Type == 19 || h265Type == 20)) || (!h265_ && h264Type == 5)) {
      // fresh IDR: restart the cached parameter sets alongside it
      idr.clear();
      AppendNal(idr, nal.data, nal.size);
    }
  }
  if (!idr.empty()) {
    lastIdr_ = std::move(idr);
  }
}

void Snapshotter::SetParams(bool h265, const std::vector<uint8_t>& paramsAnnexB) {
  std::lock_guard<std::mutex> lk(stateMu_);
  h265_ = h265;
  params_ = paramsAnnexB;
}

void Snapshotter::CaptureAsync(const std::string& bmpPath, int width, int height,
                               std::function<void(bool, const std::string&)> done) {
  std::vector<uint8_t> params;
  std::vector<uint8_t> idr;
  bool h265 = false;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    params = params_;
    idr = lastIdr_;
    h265 = h265_;
  }
  if (idr.empty() || params.empty()) {
    if (done) done(false, "no keyframe cached yet");
    return;
  }
  if (workerBusy_->exchange(true)) {
    if (done) done(false, "snapshot already in progress");
    return;
  }
  std::lock_guard<std::mutex> workerLock(workerMu_);
  if (worker_.joinable()) worker_.join();
  try {
    auto busy = workerBusy_;
    worker_ = std::thread([busy, bmpPath, width, height, done, params, idr, h265]() {
      Snapshotter::DecodeWorker(bmpPath, width, height, done, params, idr, h265);
      busy->store(false);
    });
  } catch (...) {
    workerBusy_->store(false);
    if (done) done(false, "snapshot worker start failed");
  }
}

void Snapshotter::Stop() {
  std::thread worker;
  {
    std::lock_guard<std::mutex> workerLock(workerMu_);
    if (worker_.joinable()) worker = std::move(worker_);
  }
  if (!worker.joinable()) return;
  if (worker.get_id() == std::this_thread::get_id()) {
    worker.detach();
  } else {
    worker.join();
  }
}

void Snapshotter::DecodeWorker(std::string bmpPath, int width, int height,
                               std::function<void(bool, const std::string&)> done,
                               std::vector<uint8_t> params, std::vector<uint8_t> idr,
                               bool h265) {
  auto fail = [&done](const std::string& msg) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "%{public}s", msg.c_str());
    if (done) done(false, msg);
  };
  auto session = std::make_shared<DecodeSession>();
  session->inputs.push_back(params);
  session->inputs.push_back(idr);

  session->dec = OH_VideoDecoder_CreateByMime(h265 ? "video/hevc" : "video/avc");
  if (session->dec == nullptr) {
    fail("decoder create failed");
    return;
  }
  OH_AVCodecCallback cb{};
  cb.onError = OnDecError;
  cb.onStreamChanged = OnDecStreamChange;
  cb.onNeedInputBuffer = OnNeedInput;
  cb.onNewOutputBuffer = OnNewOutput;
  if (OH_VideoDecoder_RegisterCallback(session->dec, cb, session.get()) != AV_ERR_OK) {
    fail("decoder callback failed");
    OH_VideoDecoder_Destroy(session->dec);
    return;
  }
  OH_AVFormat* fmt = OH_AVFormat_Create();
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, width);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, height);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
  if (OH_VideoDecoder_Configure(session->dec, fmt) != AV_ERR_OK) {
    OH_AVFormat_Destroy(fmt);
    fail("decoder configure failed");
    OH_VideoDecoder_Destroy(session->dec);
    return;
  }
  OH_AVFormat_Destroy(fmt);
  if (OH_VideoDecoder_Start(session->dec) != AV_ERR_OK) {
    fail("decoder start failed");
    OH_VideoDecoder_Destroy(session->dec);
    return;
  }

  bool ok = false;
  {
    std::unique_lock<std::mutex> lk(session->mu);
    session->cv.wait_for(lk, std::chrono::seconds(4),
                         [&] { return session->gotFrame || session->failed; });
    ok = session->gotFrame;
  }
  OH_VideoDecoder_Stop(session->dec);
  OH_VideoDecoder_Destroy(session->dec);

  if (!ok || session->frame.empty()) {
    fail("decode timeout/failed");
    return;
  }
  if (session->frame.size() < static_cast<size_t>(width) * height * 3 / 2) {
    fail("short decode output");
    return;
  }
  if (!WriteBmp(bmpPath, session->frame.data(), width, height)) {
    fail("bmp write failed");
    return;
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "snapshot saved %{public}s",
               bmpPath.c_str());
  if (done) done(true, bmpPath);
}

}  // namespace ipcam
