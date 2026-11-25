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

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "MicStreamer"

namespace ipcam {
namespace {

constexpr size_t kAacSamplesPerFrame = 1024;

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

int32_t MicStreamer::OnReadData(OH_AudioCapturer*, void* userData, void* buffer,
                                int32_t bufferLen) {
  auto self = static_cast<MicStreamer*>(userData);
  if (buffer != nullptr && bufferLen > 0 && self->running_.load()) {
    const uint8_t* p = static_cast<const uint8_t*>(buffer);
    decltype(self->pcmTap_) tap;
    {
      std::lock_guard<std::mutex> lk(self->stateMu_);
      tap = self->pcmTap_;
    }
    if (tap) tap(p, static_cast<size_t>(bufferLen), self->sampleRate_, self->channels_);
    const size_t frameBytes = 2 * static_cast<size_t>(self->channels_);
    {
      std::lock_guard<std::mutex> lk(self->pcmMu_);
      self->pcmQueue_.insert(self->pcmQueue_.end(), p, p + bufferLen);
      if (self->pcmQueue_.size() > (1u << 20)) {
        size_t drop = self->pcmQueue_.size() - (1u << 19);
        drop -= drop % frameBytes;
        self->pcmQueue_.erase(self->pcmQueue_.begin(),
                              self->pcmQueue_.begin() + static_cast<long>(drop));
        self->nextSampleFrame_ += drop / frameBytes;
      }
    }
    self->DrainPendingInputs();
  }
  return AUDIOSTREAM_SUCCESS;
}

void MicStreamer::OnError(OH_AVCodec*, int32_t errorCode, void* userData) {
  auto self = static_cast<MicStreamer*>(userData);
  ErrorSink onError;
  {
    std::lock_guard<std::mutex> lk(self->stateMu_);
    self->codecError_ = errorCode;
    onError = self->onError_;
  }
  if (onError) onError("aac encoder error " + std::to_string(errorCode));
}

void MicStreamer::OnStreamChanged(OH_AVCodec*, OH_AVFormat*, void*) {}

void MicStreamer::OnNeedInputData(OH_AVCodec*, uint32_t index, OH_AVMemory* data,
                                  void* userData) {
  auto self = static_cast<MicStreamer*>(userData);
  if (!self->running_.load()) return;
  {
    std::lock_guard<std::mutex> lk(self->pcmMu_);
    self->pendingInputs_.push_back({index, data});
  }
  self->DrainPendingInputs();
}

void MicStreamer::DrainPendingInputs() {
  {
    std::lock_guard<std::mutex> lk(pcmMu_);
    if (drainActive_) return;
    drainActive_ = true;
  }

  for (;;) {
    PendingInput pending;
    size_t filled = 0;
    TimestampUs ptsUs = 0;
    {
      std::lock_guard<std::mutex> lk(pcmMu_);
      if (!running_.load() || pendingInputs_.empty() || pcmQueue_.empty()) {
        drainActive_ = false;
        pcmCv_.notify_all();
        return;
      }
      pending = pendingInputs_.front();
      uint8_t* dst = pending.memory != nullptr ? OH_AVMemory_GetAddr(pending.memory) : nullptr;
      int32_t capacity = pending.memory != nullptr ? OH_AVMemory_GetSize(pending.memory) : 0;
      if (dst == nullptr || capacity <= 0) {
        pendingInputs_.pop_front();
        continue;
      }
      const size_t frameBytes = 2 * static_cast<size_t>(channels_);
      filled = kAacSamplesPerFrame * frameBytes;
      if (static_cast<size_t>(capacity) < filled) {
        pendingInputs_.pop_front();
        drainActive_ = false;
        pcmCv_.notify_all();
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                     "AAC input buffer smaller than one 1024-sample frame");
        return;
      }
      if (pcmQueue_.size() < filled) {
        drainActive_ = false;
        pcmCv_.notify_all();
        return;
      }
      ptsUs = static_cast<TimestampUs>((nextSampleFrame_ * 1000000ULL) /
                                      static_cast<uint64_t>(sampleRate_));
      memcpy(dst, pcmQueue_.data(), filled);
      pcmQueue_.erase(pcmQueue_.begin(),
                      pcmQueue_.begin() + static_cast<long>(filled));
      pendingInputs_.pop_front();
      nextSampleFrame_ += filled / frameBytes;
    }

    OH_AVCodecBufferAttr attr{};
    attr.size = static_cast<int32_t>(filled);
    attr.pts = ptsUs;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    if (OH_AudioEncoder_PushInputData(encoder_, pending.index, attr) != AV_ERR_OK) {
      std::lock_guard<std::mutex> lk(pcmMu_);
      drainActive_ = false;
      pcmCv_.notify_all();
      return;
    }
  }
}

void MicStreamer::OnNewOutputData(OH_AVCodec*, uint32_t index, OH_AVMemory* data,
                                  OH_AVCodecBufferAttr* attr, void* userData) {
  auto self = static_cast<MicStreamer*>(userData);
  if (attr != nullptr &&
      !(attr->flags & (AVCODEC_BUFFER_FLAGS_EOS | AVCODEC_BUFFER_FLAGS_CODEC_DATA)) &&
      attr->size > 0) {
    FrameSink sink;
    {
      std::lock_guard<std::mutex> lk(self->stateMu_);
      sink = self->sink_;
    }
    if (sink && data != nullptr) {
      const uint8_t* aac = OH_AVMemory_GetAddr(data);
      int32_t capacity = OH_AVMemory_GetSize(data);
      int sfIndex = SfIndexForRate(self->sampleRate_);
      if (aac != nullptr && sfIndex >= 0 && attr->size <= 8191 - 7 &&
          attr->offset >= 0 && attr->offset <= capacity && attr->size <= capacity - attr->offset) {
        uint8_t frame[8191];
        BuildAdts(frame, 1 /* AAC-LC */, sfIndex, self->channels_, attr->size);
        memcpy(frame + 7, aac + attr->offset, static_cast<size_t>(attr->size));
        sink(frame, static_cast<size_t>(attr->size) + 7, attr->pts);
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
    {
      std::unique_lock<std::mutex> lk(pcmMu_);
      pcmCv_.wait(lk, [this] { return !drainActive_; });
    }
    OH_AudioEncoder_Stop(encoder_);
    OH_AudioEncoder_Destroy(encoder_);
    encoder_ = nullptr;
  }
}

bool MicStreamer::TryFormat(int rate, int channels, int bitrate, std::string& error) {
  auto ok = [&error](const char* stage, int result) {
    if (result == 0) return true;
    error = std::string(stage) + " err=" + std::to_string(result);
    return false;
  };
  OH_AudioCapturer_Callbacks cb{};
  cb.OH_AudioCapturer_OnReadData = OnReadData;
  if (!ok("StreamBuilderCreate", OH_AudioStreamBuilder_Create(&builder_, AUDIOSTREAM_TYPE_CAPTURER)) ||
      !builder_ || !ok("SetSamplingRate", OH_AudioStreamBuilder_SetSamplingRate(builder_, rate)) ||
      !ok("SetChannelCount", OH_AudioStreamBuilder_SetChannelCount(builder_, channels)) ||
      !ok("SetSampleFormat", OH_AudioStreamBuilder_SetSampleFormat(builder_, AUDIOSTREAM_SAMPLE_S16LE)) ||
      !ok("SetEncodingType", OH_AudioStreamBuilder_SetEncodingType(builder_, AUDIOSTREAM_ENCODING_TYPE_RAW)) ||
      !ok("SetCapturerCallback", OH_AudioStreamBuilder_SetCapturerCallback(builder_, cb, this)) ||
      !ok("GenerateCapturer", OH_AudioStreamBuilder_GenerateCapturer(builder_, &capturer_)) || !capturer_) {
    return false;
  }
  // A successful builder setter is not a read-back. Configure every consumer
  // from the stream that was actually generated, before any callback can run.
  OH_AudioStream_SampleFormat pcmFormat{};
  OH_AudioStream_EncodingType encoding{};
  if (!ok("GetSamplingRate", OH_AudioCapturer_GetSamplingRate(capturer_, &sampleRate_)) ||
      !ok("GetChannelCount", OH_AudioCapturer_GetChannelCount(capturer_, &channels_)) ||
      !ok("GetSampleFormat", OH_AudioCapturer_GetSampleFormat(capturer_, &pcmFormat)) ||
      !ok("GetEncodingType", OH_AudioCapturer_GetEncodingType(capturer_, &encoding))) return false;
  if (SfIndexForRate(sampleRate_) < 0 || channels_ < 1 || channels_ > 2 ||
      pcmFormat != AUDIOSTREAM_SAMPLE_S16LE || encoding != AUDIOSTREAM_ENCODING_TYPE_RAW) {
    error = "unsupported capturer read-back";
    return false;
  }
  encoder_ = OH_AudioEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_AAC);
  if (!encoder_) { error = "CreateAudioEncoder"; return false; }
  OH_AVCodecAsyncCallback codecCb{};
  codecCb.onError = OnError;
  codecCb.onStreamChanged = OnStreamChanged;
  codecCb.onNeedInputData = OnNeedInputData;
  codecCb.onNeedOutputData = OnNewOutputData;
  if (!ok("EncoderSetCallback", OH_AudioEncoder_SetCallback(encoder_, codecCb, this))) return false;
  OH_AVFormat* format = OH_AVFormat_Create();
  if (!format) { error = "CreateAudioFormat"; return false; }
  bool configured = OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, sampleRate_) &&
                    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, channels_) &&
                    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, bitrate) &&
                    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE) &&
                    ok("EncoderConfigure", OH_AudioEncoder_Configure(encoder_, format));
  OH_AVFormat_Destroy(format);
  if (!configured || !ok("EncoderPrepare", OH_AudioEncoder_Prepare(encoder_))) return false;
  running_ = true;  // Start may synchronously request encoder input.
  return ok("EncoderStart", OH_AudioEncoder_Start(encoder_)) &&
         ok("CapturerStart", OH_AudioCapturer_Start(capturer_));
}

bool MicStreamer::Start(int sampleRate, int channels, int bitrate, FrameSink sink,
                        ErrorSink onError) {
  if (running_.load()) return false;
  if ((sampleRate != 0 && SfIndexForRate(sampleRate) < 0) || channels < 1 || channels > 2 || bitrate <= 0) {
    if (onError) onError("unsupported microphone parameters");
    return false;
  }
  // Publish sinks only once the complete candidate starts successfully.
  // AppUtils.g: prefer the requested channel mask at each rate, then try the
  // other mask before moving to the next rate. An explicit preferred Hz leads.
  const int rates[] = {sampleRate, 48000, 24000, 12000, 16000, 8000, 32000, 44100, 22050};
  std::string error;
  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
    if (!rates[i] || (i && rates[i] == sampleRate)) continue;
    for (int ch : {channels, 3 - channels}) {
      error = "configure microphone";
      {
        std::lock_guard<std::mutex> lk(stateMu_);
        codecError_ = 0;
      }
      if (TryFormat(rates[i], ch, bitrate, error)) {
        std::lock_guard<std::mutex> lk(stateMu_);
        if (!codecError_) {
          onError_ = std::move(onError);
          sink_ = std::move(sink);
          OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                       "mic streaming %{public}dHz x%{public}d -> AAC", sampleRate_, channels_);
          return true;
        }
        error = "aac encoder error " + std::to_string(codecError_);
      }
      running_ = false;
      Cleanup();  // Native stop/release quiesces callbacks before the next attempt.
      std::lock_guard<std::mutex> lk(pcmMu_);
      pcmQueue_.clear();
      pendingInputs_.clear();
      nextSampleFrame_ = 0;
    }
  }
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = nullptr;
  }
  if (onError) onError("no usable microphone/AAC format: " + error);
  return false;
}

void MicStreamer::Stop() {
  if (!running_.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    sink_ = nullptr;
    onError_ = nullptr;
  }
  Cleanup();  // Quiesce the last capturer/encoder callbacks before discarding their slots.
  {
    std::lock_guard<std::mutex> lk(pcmMu_);
    pcmQueue_.clear();
    pendingInputs_.clear();
    nextSampleFrame_ = 0;
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "mic streaming stopped");
}

}  // namespace ipcam
