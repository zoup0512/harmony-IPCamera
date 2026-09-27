#include "mic_streamer.h"

#include <algorithm>
#include <cstring>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avcodec_audioencoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmemory.h>
#include <ohaudio/native_audiocapturer.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiostreambuilder.h>

#define LOG_DOMAIN 0xC010
#define LOG_TAG "MicStreamer"

namespace ipcam {
namespace {

// Sample-rate -> ADTS sampling_frequency_index.
int SfIndexForRate(int rate) {
  const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                         22050, 16000, 12000, 11025, 8000,  7350};
  for (int i = 0; i < 13; ++i) {
    if (rates[i] == rate) return i;
  }
  return -1;
}

// 7-byte ADTS header, protection_absent=1, one raw data block.
void BuildAdts(uint8_t out[7], int profileBits, int sfIndex, int channels, int aacLen) {
  int frameLen = aacLen + 7;
  out[0] = 0xFF;
  out[1] = 0xF1;  // sync + MPEG-4 + layer 0 + protection absent
  out[2] = static_cast<uint8_t>((profileBits << 6) | (sfIndex << 2) | ((channels >> 2) & 0x01));
  out[3] = static_cast<uint8_t>(((channels & 0x03) << 6) | ((frameLen >> 11) & 0x03));
  out[4] = static_cast<uint8_t>((frameLen >> 3) & 0xFF);
  out[5] = static_cast<uint8_t>(((frameLen & 0x07) << 5) | 0x1F);
  out[6] = 0xFC;
}

}  // namespace

MicStreamer::~MicStreamer() { Stop(); }

void MicStreamer::Fail(const std::string& what, int err) {
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "%{public}s err=%{public}d", what.c_str(),
               err);
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    onError = onError_;
  }
  if (onError) onError(what + " err=" + std::to_string(err));
}

int32_t MicStreamer::OnReadData(OH_AudioCapturer*, void* userData, void* buffer,
                                int32_t bufferLen) {
  auto self = static_cast<MicStreamer*>(userData);
  if (buffer != nullptr && bufferLen > 0) {
    const uint8_t* p = static_cast<const uint8_t*>(buffer);
    std::lock_guard<std::mutex> lk(self->pcmMu_);
    self->pcmQueue_.insert(self->pcmQueue_.end(), p, p + bufferLen);
    // Cap the queue: drop oldest PCM if the encoder falls far behind.
    if (self->pcmQueue_.size() > 1u << 20) {
      self->pcmQueue_.erase(self->pcmQueue_.begin(), self->pcmQueue_.end() - (1u << 19));
    }
    if (self->hasPendingInput_ && self->pendingMem_ != nullptr) {
      // Drain queued PCM into the held encoder input buffer and push it.
      uint8_t* dst = OH_AVMemory_GetAddr(self->pendingMem_);
      int32_t capacity = OH_AVMemory_GetSize(self->pendingMem_);
      size_t filled = 0;
      if (dst != nullptr && capacity > 0) {
        filled = std::min(static_cast<size_t>(capacity), self->pcmQueue_.size());
        if (filled > 0) {
          memcpy(dst, self->pcmQueue_.data(), filled);
          self->pcmQueue_.erase(self->pcmQueue_.begin(),
                                self->pcmQueue_.begin() + static_cast<long>(filled));
        }
      }
      OH_AVCodecBufferAttr attr{};
      attr.size = static_cast<int32_t>(filled);
      attr.pts = static_cast<int64_t>((self->totalSamples_ * 1000000ULL) /
                                      static_cast<uint64_t>(self->sampleRate_));
      attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
      OH_AudioEncoder_PushInputData(self->encoder_, self->pendingIndex_, attr);
      self->totalSamples_ += filled / (2 * static_cast<size_t>(self->channels_));
      self->hasPendingInput_ = false;
      self->pendingMem_ = nullptr;
    }
  }
  return AUDIOSTREAM_SUCCESS;
}

void MicStreamer::OnError(OH_AVCodec*, int32_t errorCode, void* userData) {
  auto self = static_cast<MicStreamer*>(userData);
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(self->stateMu_);
    onError = self->onError_;
  }
  if (onError) onError("aac encoder error " + std::to_string(errorCode));
}

void MicStreamer::OnStreamChanged(OH_AVCodec*, OH_AVFormat*, void*) {}

void MicStreamer::OnNeedInputData(OH_AVCodec*, uint32_t index, OH_AVMemory* data,
                                  void* userData) {
  auto self = static_cast<MicStreamer*>(userData);
  if (!self->running_.load()) {
    OH_AVCodecBufferAttr attr{};
    attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
    OH_AudioEncoder_PushInputData(self->encoder_, index, attr);
    return;
  }
  uint8_t* dst = OH_AVMemory_GetAddr(data);
  int32_t capacity = OH_AVMemory_GetSize(data);
  size_t filled = 0;
  {
    std::lock_guard<std::mutex> lk(self->pcmMu_);
    if (dst != nullptr && capacity > 0) {
      filled = std::min(static_cast<size_t>(capacity), self->pcmQueue_.size());
      if (filled > 0) {
        memcpy(dst, self->pcmQueue_.data(), filled);
        self->pcmQueue_.erase(self->pcmQueue_.begin(),
                              self->pcmQueue_.begin() + static_cast<long>(filled));
      }
    }
    if (filled == 0) {
      // No PCM yet: hold this input buffer for the mic callback to fill.
      self->hasPendingInput_ = true;
      self->pendingIndex_ = index;
      self->pendingMem_ = data;
      return;
    }
  }
  OH_AVCodecBufferAttr attr{};
  attr.size = static_cast<int32_t>(filled);
  attr.pts = static_cast<int64_t>((self->totalSamples_ * 1000000ULL) /
                                  static_cast<uint64_t>(self->sampleRate_));
  attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
  OH_AudioEncoder_PushInputData(self->encoder_, index, attr);
  self->totalSamples_ += filled / (2 * static_cast<size_t>(self->channels_));
}

void MicStreamer::OnNewOutputData(OH_AVCodec*, uint32_t index, OH_AVMemory* data,
                                  OH_AVCodecBufferAttr* attr, void* userData) {
  auto self = static_cast<MicStreamer*>(userData);
  if (attr != nullptr && !(attr->flags & AVCODEC_BUFFER_FLAGS_EOS) && attr->size > 0) {
    FrameSink sink;
    {
      std::lock_guard<std::mutex> lk(self->stateMu_);
      sink = self->sink_;
    }
    if (sink && data != nullptr) {
      const uint8_t* aac = OH_AVMemory_GetAddr(data);
      int sfIndex = SfIndexForRate(self->sampleRate_);
      if (aac != nullptr && sfIndex >= 0 && attr->size <= 8192) {
        uint8_t frame[7 + 8192];
        BuildAdts(frame, 1 /* AAC-LC */, sfIndex, self->channels_, attr->size);
        memcpy(frame + 7, aac, static_cast<size_t>(attr->size));
        sink(frame, static_cast<size_t>(attr->size) + 7,
             static_cast<uint64_t>(attr->pts));
      }
    }
  }
  OH_AudioEncoder_FreeOutputData(self->encoder_, index);
}

void MicStreamer::Cleanup() {
  if (capturer_ != nullptr) {
    OH_AudioCapturer_Stop(capturer_);
    OH_AudioCapturer_Release(capturer_);
    capturer_ = nullptr;
  }
  if (builder_ != nullptr) {
    OH_AudioStreamBuilder_Destroy(builder_);
    builder_ = nullptr;
  }
  if (encoder_ != nullptr) {
    OH_AudioEncoder_Stop(encoder_);
    OH_AudioEncoder_Destroy(encoder_);
    encoder_ = nullptr;
  }
}

bool MicStreamer::Start(int sampleRate, int channels, int bitrate, FrameSink sink,
                        ErrorSink onError) {
  if (running_.load()) return false;
  if (SfIndexForRate(sampleRate) < 0 || channels < 1 || channels > 2) {
    Fail("unsupported audio params", 0);
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = std::move(sink);
    onError_ = std::move(onError);
  }
  sampleRate_ = sampleRate;
  channels_ = channels;
  totalSamples_ = 0;
  {
    std::lock_guard<std::mutex> lk(pcmMu_);
    pcmQueue_.clear();
  }
  running_ = true;

  encoder_ = OH_AudioEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_AAC);
  if (encoder_ == nullptr) {
    Fail("CreateAudioEncoder", 0);
    Cleanup();
    running_ = false;
    return false;
  }
  OH_AVCodecAsyncCallback cb{};
  cb.onError = OnError;
  cb.onStreamChanged = OnStreamChanged;
  cb.onNeedInputData = OnNeedInputData;
  cb.onNeedOutputData = OnNewOutputData;
  OH_AVErrCode aerr = OH_AudioEncoder_SetCallback(encoder_, cb, this);
  if (aerr != AV_ERR_OK) {
    Fail("EncoderSetCallback", aerr);
    Cleanup();
    running_ = false;
    return false;
  }
  OH_AVFormat* format = OH_AVFormat_Create();
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, sampleRate_);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, channels_);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_BITRATE, bitrate);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE);
  aerr = OH_AudioEncoder_Configure(encoder_, format);
  OH_AVFormat_Destroy(format);
  if (aerr != AV_ERR_OK) {
    Fail("EncoderConfigure", aerr);
    Cleanup();
    running_ = false;
    return false;
  }
  aerr = OH_AudioEncoder_Prepare(encoder_);
  if (aerr != AV_ERR_OK) {
    Fail("EncoderPrepare", aerr);
    Cleanup();
    running_ = false;
    return false;
  }
  aerr = OH_AudioEncoder_Start(encoder_);
  if (aerr != AV_ERR_OK) {
    Fail("EncoderStart", aerr);
    Cleanup();
    running_ = false;
    return false;
  }

  OH_AudioStream_Result res =
      OH_AudioStreamBuilder_Create(&builder_, AUDIOSTREAM_TYPE_CAPTURER);
  if (res != AUDIOSTREAM_SUCCESS || builder_ == nullptr) {
    Fail("StreamBuilderCreate", res);
    Cleanup();
    running_ = false;
    return false;
  }
  OH_AudioStreamBuilder_SetSamplingRate(builder_, sampleRate_);
  OH_AudioStreamBuilder_SetChannelCount(builder_, channels_);
  OH_AudioStreamBuilder_SetSampleFormat(builder_, AUDIOSTREAM_SAMPLE_S16LE);
  OH_AudioStreamBuilder_SetEncodingType(builder_, AUDIOSTREAM_ENCODING_TYPE_RAW);
  OH_AudioCapturer_Callbacks capturerCb{};
  capturerCb.OH_AudioCapturer_OnReadData = OnReadData;
  OH_AudioStreamBuilder_SetCapturerCallback(builder_, capturerCb, this);
  res = OH_AudioStreamBuilder_GenerateCapturer(builder_, &capturer_);
  if (res != AUDIOSTREAM_SUCCESS || capturer_ == nullptr) {
    Fail("GenerateCapturer", res);
    Cleanup();
    running_ = false;
    return false;
  }
  res = OH_AudioCapturer_Start(capturer_);
  if (res != AUDIOSTREAM_SUCCESS) {
    Fail("CapturerStart (permission?)", res);
    Cleanup();
    running_ = false;
    return false;
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "mic streaming %{public}dHz x%{public}d -> AAC", sampleRate_, channels_);
  return true;
}

void MicStreamer::Stop() {
  if (!running_.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = nullptr;
    onError_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(pcmMu_);
    pcmQueue_.clear();
    hasPendingInput_ = false;
    pendingMem_ = nullptr;
  }
  Cleanup();
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "mic streaming stopped");
}

}  // namespace ipcam
