#include "test_source.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avformat.h>

#define LOG_DOMAIN 0xC010
#define LOG_TAG "TestPattern"

namespace ipcam {
namespace {

void LogErr(const char* what, int err) {
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "%{public}s err=%{public}d", what, err);
}

}  // namespace

TestPatternSource::~TestPatternSource() { Stop(); }

bool TestPatternSource::Start(int width, int height, int fps, int bitrate, FrameSink sink,
                              ErrorSink onError) {
  if (running_.load()) return false;
  width_ = width;
  height_ = height;
  fps_ = fps > 0 ? fps : 15;
  frameIndex_ = 0;
  nextPtsUs_ = 0;
  startWall_ = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = std::move(sink);
    onError_ = std::move(onError);
  }
  running_ = true;

  encoder_ = OH_VideoEncoder_CreateByMime("video/avc");
  if (encoder_ == nullptr) {
    running_ = false;
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "create encoder failed");
    return false;
  }

  OH_AVCodecCallback cb{};
  cb.onError = OnError;
  cb.onStreamChanged = OnStreamChanged;
  cb.onNeedInputBuffer = OnNeedInputBuffer;
  cb.onNewOutputBuffer = OnNewOutputBuffer;
  OH_AVErrCode err = OH_VideoEncoder_RegisterCallback(encoder_, cb, this);
  if (err != AV_ERR_OK) {
    LogErr("RegisterCallback", err);
    running_ = false;
    OH_VideoEncoder_Destroy(encoder_);
    encoder_ = nullptr;
    return false;
  }

  OH_AVFormat* format = OH_AVFormat_Create();
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width_);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height_);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_BITRATE, bitrate);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE, BITRATE_MODE_CBR);
  OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, static_cast<double>(fps_));
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, 2);
  err = OH_VideoEncoder_Configure(encoder_, format);
  OH_AVFormat_Destroy(format);
  if (err != AV_ERR_OK) {
    LogErr("Configure", err);
    running_ = false;
    OH_VideoEncoder_Destroy(encoder_);
    encoder_ = nullptr;
    return false;
  }
  err = OH_VideoEncoder_Prepare(encoder_);
  if (err != AV_ERR_OK) {
    LogErr("Prepare", err);
    running_ = false;
    OH_VideoEncoder_Destroy(encoder_);
    encoder_ = nullptr;
    return false;
  }
  err = OH_VideoEncoder_Start(encoder_);
  if (err != AV_ERR_OK) {
    LogErr("Start", err);
    running_ = false;
    OH_VideoEncoder_Destroy(encoder_);
    encoder_ = nullptr;
    return false;
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "encoder started %{public}dx%{public}d@%{public}d", width_, height_, fps_);
  return true;
}

void TestPatternSource::Stop() {
  if (!running_.exchange(false)) return;
  // Let an in-flight input callback finish (it pushes EOS once running_ is false).
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  if (encoder_ != nullptr) {
    OH_VideoEncoder_Stop(encoder_);
    OH_VideoEncoder_Destroy(encoder_);
    encoder_ = nullptr;
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "encoder stopped");
}

void TestPatternSource::OnError(OH_AVCodec*, int32_t errorCode, void* userData) {
  auto self = static_cast<TestPatternSource*>(userData);
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "codec error %{public}d", errorCode);
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(self->stateMu_);
    onError = self->onError_;
  }
  if (onError) onError("codec error " + std::to_string(errorCode));
}

void TestPatternSource::OnStreamChanged(OH_AVCodec*, OH_AVFormat*, void*) {
  // Encoder emits SPS/PPS in-band with Annex-B output; nothing to do here.
}

void TestPatternSource::WaitNextFrameSlot() {
  const int64_t frameDurUs = 1000000 / fps_;
  auto deadline = startWall_ + std::chrono::microseconds(nextPtsUs_ + frameDurUs);
  std::this_thread::sleep_until(deadline);
}

void TestPatternSource::OnNeedInputBuffer(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer,
                                          void* userData) {
  auto self = static_cast<TestPatternSource*>(userData);
  if (!self->running_.load()) {
    OH_AVCodecBufferAttr eos{};
    eos.flags = AVCODEC_BUFFER_FLAGS_EOS;
    OH_AVBuffer_SetBufferAttr(buffer, &eos);
    OH_VideoEncoder_PushInputBuffer(self->encoder_, index);
    return;
  }
  self->WaitNextFrameSlot();
  uint64_t ptsUs = static_cast<uint64_t>(self->nextPtsUs_);
  int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
  self->FillFrame(OH_AVBuffer_GetAddr(buffer), static_cast<size_t>(capacity), ptsUs);
  OH_AVCodecBufferAttr attr{};
  attr.size = self->width_ * self->height_ * 3 / 2;
  attr.pts = static_cast<int64_t>(ptsUs);
  attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
  OH_AVBuffer_SetBufferAttr(buffer, &attr);
  OH_VideoEncoder_PushInputBuffer(self->encoder_, index);
  self->frameIndex_++;
  self->nextPtsUs_ += 1000000 / self->fps_;
}

void TestPatternSource::OnNewOutputBuffer(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer,
                                          void* userData) {
  auto self = static_cast<TestPatternSource*>(userData);
  OH_AVCodecBufferAttr attr{};
  OH_AVBuffer_GetBufferAttr(buffer, &attr);
  if (!(attr.flags & AVCODEC_BUFFER_FLAGS_EOS) && attr.size > 0) {
    FrameSink sink;
    {
      std::lock_guard<std::mutex> lk(self->stateMu_);
      sink = self->sink_;
    }
    if (sink) {
      sink(OH_AVBuffer_GetAddr(buffer), static_cast<size_t>(attr.size),
           static_cast<uint64_t>(attr.pts));
    }
  }
  OH_VideoEncoder_FreeOutputBuffer(self->encoder_, index);
}

void TestPatternSource::FillFrame(uint8_t* dst, size_t capacity, uint64_t) {
  const size_t ySize = static_cast<size_t>(width_) * static_cast<size_t>(height_);
  const size_t need = ySize * 3 / 2;
  if (dst == nullptr || static_cast<size_t>(capacity) < need) return;
  uint8_t* yPlane = dst;
  uint8_t* uvPlane = dst + ySize;

  static const uint8_t barY[8] = {235, 210, 170, 145, 106, 81, 41, 16};
  static const uint8_t barU[8] = {128, 16, 166, 74, 83, 90, 109, 128};
  static const uint8_t barV[8] = {128, 146, 16, 26, 202, 240, 138, 128};
  int barW = width_ / 8;
  if (barW <= 0) barW = 1;

  for (int r = 0; r < height_; ++r) {
    uint8_t* row = yPlane + static_cast<size_t>(r) * width_;
    for (int c = 0; c < width_; ++c) {
      int bar = c / barW;
      if (bar > 7) bar = 7;
      row[c] = barY[bar];
    }
  }
  for (int r = 0; r < height_ / 2; ++r) {
    uint8_t* row = uvPlane + static_cast<size_t>(r) * width_;
    for (int c = 0; c < width_ / 2; ++c) {
      int bar = (2 * c) / barW;
      if (bar > 7) bar = 7;
      row[2 * c] = barU[bar];
      row[2 * c + 1] = barV[bar];
    }
  }

  const int box = 48;
  int range = width_ - box;
  if (range <= 0) range = 1;
  int x0 = static_cast<int>((frameIndex_ * 5) % static_cast<uint64_t>(range));
  int y0 = height_ - 80 > 0 ? height_ - 80 : 0;
  for (int r = y0; r < y0 + box && r < height_; ++r) {
    uint8_t* row = yPlane + static_cast<size_t>(r) * width_;
    for (int c = x0; c < x0 + box && c < width_; ++c) {
      row[c] = 235;
    }
  }
  for (int r = y0 / 2; r < (y0 + box) / 2 && r < height_ / 2; ++r) {
    uint8_t* row = uvPlane + static_cast<size_t>(r) * width_;
    for (int c = x0 / 2; c < (x0 + box) / 2 && c < width_ / 2; ++c) {
      row[2 * c] = 128;
      row[2 * c + 1] = 128;
    }
  }
}

}  // namespace ipcam
