#include "voice_player.h"

#include <algorithm>
#include <cstring>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_audiodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <ohaudio/native_audiocapturer.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiostreambuilder.h>

#include "codec_records.h"

#define LOG_DOMAIN 0xC010
#define LOG_TAG "VoicePlayer"

namespace ipcam {

struct VoicePlayer::Impl {
  OH_AVCodec* dec = nullptr;
  OH_AudioStreamBuilder* builder = nullptr;
  OH_AudioRenderer* renderer = nullptr;

  std::mutex mu;
  std::vector<uint8_t> aacBuf;   // incoming ADTS bytes (may end mid-frame)
  std::vector<uint8_t> pcmRing;  // decoded PCM, S16LE
  size_t ringRead = 0;
  bool decoderConfigured = false;
  int sampleRate = 44100;
  int channels = 1;
  bool rendererStarted = false;
  bool eos = false;

  void PushPcm(const uint8_t* pcm, size_t bytes) {
    std::lock_guard<std::mutex> lk(mu);
    pcmRing.insert(pcmRing.end(), pcm, pcm + bytes);
    if (pcmRing.size() > (1u << 20)) {
      pcmRing.erase(pcmRing.begin(), pcmRing.end() - (1u << 19));
    }
  }
};

namespace {

void OnVoiceNeedInput(OH_AVCodec*, uint32_t index, OH_AVMemory* data, void* userData) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  std::vector<uint8_t> raw;
  {
    std::lock_guard<std::mutex> lk(impl->mu);
    // pull one ADTS frame
    while (impl->aacBuf.size() >= 7) {
      AdtsInfo info{};
      if (!ParseAdts(impl->aacBuf.data(), impl->aacBuf.size(), &info)) {
        impl->aacBuf.erase(impl->aacBuf.begin());  // resync
        continue;
      }
      if (impl->aacBuf.size() < static_cast<size_t>(info.frameLen)) return;  // wait for more
      raw.assign(impl->aacBuf.begin() + info.headerLen,
                 impl->aacBuf.begin() + info.frameLen);
      impl->aacBuf.erase(impl->aacBuf.begin(),
                         impl->aacBuf.begin() + info.frameLen);
      break;
    }
  }
  OH_AVCodecBufferAttr attr{};
  if (!raw.empty()) {
    int32_t cap = OH_AVMemory_GetSize(data);
    size_t n = std::min(raw.size(), static_cast<size_t>(cap));
    memcpy(OH_AVMemory_GetAddr(data), raw.data(), n);
    attr.size = static_cast<int32_t>(n);
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
  } else {
    return;  // keep buffer; retried on next feed
  }
  OH_AudioDecoder_PushInputData(impl->dec, index, attr);
}

void OnVoiceNewOutput(OH_AVCodec*, uint32_t index, OH_AVMemory* data,
                      OH_AVCodecBufferAttr* attr, void* userData) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  if (attr != nullptr && !(attr->flags & AVCODEC_BUFFER_FLAGS_EOS) && attr->size > 0 &&
      data != nullptr) {
    impl->PushPcm(OH_AVMemory_GetAddr(data), static_cast<size_t>(attr->size));
  }
  OH_AudioDecoder_FreeOutputData(impl->dec, index);
}

void OnVoiceError(OH_AVCodec*, int32_t err, void*) {
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "decoder error %{public}d", err);
}

void OnVoiceStreamChange(OH_AVCodec*, OH_AVFormat* format, void* userData) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  int32_t rate = impl->sampleRate;
  int32_t ch = impl->channels;
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, &rate);
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, &ch);
  if (rate > 0) impl->sampleRate = rate;
  if (ch > 0) impl->channels = ch;
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "pcm %{public}dHz x%{public}d",
               impl->sampleRate, impl->channels);
}

// renderer pulls decoded PCM
int32_t OnVoiceWriteData(OH_AudioRenderer*, void* userData, void* buffer, int32_t len) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  auto* dst = static_cast<uint8_t*>(buffer);
  size_t want = static_cast<size_t>(len);
  std::lock_guard<std::mutex> lk(impl->mu);
  size_t avail = impl->pcmRing.size() - impl->ringRead;
  size_t n = std::min(want, avail);
  memcpy(dst, impl->pcmRing.data() + impl->ringRead, n);
  impl->pcmRing.erase(impl->pcmRing.begin(), impl->pcmRing.begin() + static_cast<long>(n));
  impl->ringRead = 0;
  if (n < want) {
    memset(dst + n, 0, want - n);  // underrun: silence
  }
  return AUDIOSTREAM_SUCCESS;
}

}  // namespace

VoicePlayer::~VoicePlayer() { Stop(); }

void VoicePlayer::Feed(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return;
  if (impl_ == nullptr) impl_ = new Impl();
  active_ = true;
  impl_->aacBuf.insert(impl_->aacBuf.end(), data, data + size);

  if (impl_->dec == nullptr) {
    // wait until the first ADTS frame is complete to learn the config
    AdtsInfo info{};
    if (impl_->aacBuf.size() < 7 || !ParseAdts(impl_->aacBuf.data(), impl_->aacBuf.size(), &info)) {
      return;
    }
    impl_->sampleRate = info.sampleRate;
    impl_->channels = info.channels;

    impl_->dec = OH_AudioDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_AAC);
    if (impl_->dec == nullptr) {
      OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "decoder create failed");
      return;
    }
    OH_AVCodecAsyncCallback cb{};
    cb.onError = OnVoiceError;
    cb.onStreamChanged = OnVoiceStreamChange;
    cb.onNeedInputData = OnVoiceNeedInput;
    cb.onNeedOutputData = OnVoiceNewOutput;
    if (OH_AudioDecoder_SetCallback(impl_->dec, cb, impl_) != AV_ERR_OK) {
      return;
    }
    OH_AVFormat* fmt = OH_AVFormat_Create();
    OH_AVFormat_SetStringValue(fmt, OH_MD_KEY_CODEC_MIME, "audio/mp4a-latm");
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUD_SAMPLE_RATE, info.sampleRate);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUD_CHANNEL_COUNT, info.channels);
    std::vector<uint8_t> cfg = AacConfigBytes(info.profileBits, info.sfIndex, info.channels);
    OH_AVFormat_SetBuffer(fmt, OH_MD_KEY_CODEC_CONFIG, cfg.data(), static_cast<int>(cfg.size()));
    if (OH_AudioDecoder_Configure(impl_->dec, fmt) != AV_ERR_OK) {
      OH_AVFormat_Destroy(fmt);
      return;
    }
    OH_AVFormat_Destroy(fmt);
    if (OH_AudioDecoder_Start(impl_->dec) != AV_ERR_OK) return;
    impl_->decoderConfigured = true;

    if (OH_AudioStreamBuilder_Create(&impl_->builder, AUDIOSTREAM_TYPE_RENDERER) ==
        AUDIOSTREAM_SUCCESS) {
      OH_AudioStreamBuilder_SetSamplingRate(impl_->builder, impl_->sampleRate);
      OH_AudioStreamBuilder_SetChannelCount(impl_->builder, impl_->channels);
      OH_AudioStreamBuilder_SetSampleFormat(impl_->builder, AUDIOSTREAM_SAMPLE_S16LE);
      OH_AudioStreamBuilder_SetEncodingType(impl_->builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
      OH_AudioStreamBuilder_SetRendererInfo(impl_->builder, AUDIOSTREAM_USAGE_MUSIC);
      OH_AudioRenderer_Callbacks rcb{};
      rcb.OH_AudioRenderer_OnWriteData = OnVoiceWriteData;
      OH_AudioStreamBuilder_SetRendererCallback(impl_->builder, rcb, impl_);
      if (OH_AudioStreamBuilder_GenerateRenderer(impl_->builder, &impl_->renderer) ==
          AUDIOSTREAM_SUCCESS) {
        OH_AudioRenderer_Start(impl_->renderer);
        impl_->rendererStarted = true;
      }
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "talk-back started %{public}dHz",
                 impl_->sampleRate);
  }
}

void VoicePlayer::Stop() {
  if (impl_ == nullptr) return;
  if (impl_->renderer != nullptr) {
    OH_AudioRenderer_Stop(impl_->renderer);
    OH_AudioRenderer_Release(impl_->renderer);
    impl_->renderer = nullptr;
  }
  if (impl_->builder != nullptr) {
    OH_AudioStreamBuilder_Destroy(impl_->builder);
    impl_->builder = nullptr;
  }
  if (impl_->dec != nullptr) {
    OH_AudioDecoder_Stop(impl_->dec);
    OH_AudioDecoder_Destroy(impl_->dec);
    impl_->dec = nullptr;
  }
  delete impl_;
  impl_ = nullptr;
  active_ = false;
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "talk-back stopped");
}

}  // namespace ipcam
