#include "voice_player.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <new>
#include <utility>
#include <vector>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_audiodecoder.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiostreambuilder.h>

#include "codec_records.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "VoicePlayer"

namespace ipcam {
namespace {

constexpr size_t kMaxAdtsTailBytes = 8192;
constexpr size_t kMaxQueuedFrames = 64;
constexpr size_t kMaxQueuedFrameBytes = 256u * 1024u;
constexpr size_t kPcmRingBytes = 1024u * 1024u;

struct AacConfig {
  int profileBits = 1;
  int sfIndex = 4;
  int channels = 1;
  int sampleRate = 44100;
};

bool SameConfig(const AacConfig& config, const AdtsInfo& info) {
  return config.profileBits == info.profileBits && config.sfIndex == info.sfIndex &&
         config.channels == info.channels && config.sampleRate == info.sampleRate;
}

AacConfig ConfigFromAdts(const AdtsInfo& info) {
  AacConfig config;
  config.profileBits = info.profileBits;
  config.sfIndex = info.sfIndex;
  config.channels = info.channels;
  config.sampleRate = info.sampleRate;
  return config;
}

}  // namespace

struct VoicePlayer::Impl {
  struct PendingInput {
    uint32_t index = 0;
    OH_AVMemory* memory = nullptr;
    size_t capacity = 0;
  };

  explicit Impl(std::atomic<bool>* activeFlag)
      : pcmRing(kPcmRingBytes), active(activeFlag) {}

  OH_AVCodec* decoder = nullptr;
  OH_AudioStreamBuilder* builder = nullptr;
  OH_AudioRenderer* renderer = nullptr;

  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> adtsTail;
  std::deque<std::vector<uint8_t>> frames;
  size_t queuedFrameBytes = 0;
  std::deque<PendingInput> pendingInputs;

  std::vector<uint8_t> pcmRing;
  size_t pcmRead = 0;
  size_t pcmWrite = 0;
  size_t pcmSize = 0;

  AacConfig config;
  bool haveConfig = false;
  bool decoderStarted = false;
  bool decoderStartAttempted = false;
  bool rendererStarted = false;
  bool rendererStartAttempted = false;
  bool stopping = false;
  bool fatal = false;
  bool drainActive = false;
  size_t callbacksInFlight = 0;
  int64_t nextPtsUs = 0;
  std::atomic<bool>* active = nullptr;

  void PushPcm(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || pcmRing.empty()) return;
    std::lock_guard<std::mutex> lock(mu);
    if (stopping) return;

    if (size >= pcmRing.size()) {
      data += size - pcmRing.size();
      size = pcmRing.size();
      pcmRead = 0;
      pcmWrite = 0;
      pcmSize = 0;
    } else if (pcmSize + size > pcmRing.size()) {
      size_t discard = pcmSize + size - pcmRing.size();
      pcmRead = (pcmRead + discard) % pcmRing.size();
      pcmSize -= discard;
    }

    size_t first = std::min(size, pcmRing.size() - pcmWrite);
    std::memcpy(pcmRing.data() + pcmWrite, data, first);
    if (size > first) std::memcpy(pcmRing.data(), data + first, size - first);
    pcmWrite = (pcmWrite + size) % pcmRing.size();
    pcmSize += size;
  }

  void ReadPcm(uint8_t* destination, size_t size) {
    if (destination == nullptr || size == 0) return;
    std::lock_guard<std::mutex> lock(mu);
    size_t copied = stopping ? 0 : std::min(size, pcmSize);
    size_t first = std::min(copied, pcmRing.size() - pcmRead);
    if (first > 0) std::memcpy(destination, pcmRing.data() + pcmRead, first);
    if (copied > first) std::memcpy(destination + first, pcmRing.data(), copied - first);
    pcmRead = (pcmRead + copied) % pcmRing.size();
    pcmSize -= copied;
    if (copied < size) std::memset(destination + copied, 0, size - copied);
  }
};

namespace {

class CallbackGuard {
 public:
  explicit CallbackGuard(VoicePlayer::Impl* impl) : impl_(impl) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    ++impl_->callbacksInFlight;
  }

  ~CallbackGuard() {
    {
      std::lock_guard<std::mutex> lock(impl_->mu);
      --impl_->callbacksInFlight;
    }
    impl_->cv.notify_all();
  }

 private:
  VoicePlayer::Impl* impl_;
};

void MarkFatal(VoicePlayer::Impl* impl) {
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->fatal = true;
  }
  if (impl->active != nullptr) impl->active->store(false);
}

void FinishDrain(VoicePlayer::Impl* impl) {
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->drainActive = false;
  }
  impl->cv.notify_all();
}

void DrainDecoderInput(VoicePlayer::Impl* impl) {
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    if (impl->drainActive || impl->stopping || impl->fatal || !impl->decoderStarted ||
        impl->decoder == nullptr) {
      return;
    }
    impl->drainActive = true;
  }

  for (;;) {
    VoicePlayer::Impl::PendingInput pending;
    std::vector<uint8_t> frame;
    OH_AVCodec* decoder = nullptr;
    int64_t pts = 0;
    bool droppedOversize = false;
    {
      std::lock_guard<std::mutex> lock(impl->mu);
      if (impl->stopping || impl->fatal || !impl->decoderStarted ||
          impl->decoder == nullptr) {
        impl->drainActive = false;
        impl->cv.notify_all();
        return;
      }

      while (!impl->frames.empty() && !impl->pendingInputs.empty()) {
        size_t frameSize = impl->frames.front().size();
        bool fitsAnyPending = false;
        for (const auto& candidate : impl->pendingInputs) {
          if (candidate.capacity >= frameSize) {
            fitsAnyPending = true;
            break;
          }
        }
        if (fitsAnyPending) break;
        impl->queuedFrameBytes -= frameSize;
        impl->frames.pop_front();
        droppedOversize = true;
      }
      if (impl->frames.empty() || impl->pendingInputs.empty()) {
        impl->drainActive = false;
        impl->cv.notify_all();
        return;
      }

      size_t frameSize = impl->frames.front().size();
      auto pendingIt = std::find_if(
          impl->pendingInputs.begin(), impl->pendingInputs.end(),
          [frameSize](const VoicePlayer::Impl::PendingInput& candidate) {
            return candidate.capacity >= frameSize;
          });
      if (pendingIt == impl->pendingInputs.end()) {
        impl->drainActive = false;
        impl->cv.notify_all();
        return;
      }
      pending = *pendingIt;
      impl->pendingInputs.erase(pendingIt);
      frame = std::move(impl->frames.front());
      impl->frames.pop_front();
      impl->queuedFrameBytes -= frame.size();
      decoder = impl->decoder;
      pts = impl->nextPtsUs;
      if (impl->config.sampleRate > 0) {
        impl->nextPtsUs += 1024LL * 1000000LL / impl->config.sampleRate;
      }
    }

    if (droppedOversize) {
      OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                   "dropped AAC frame larger than decoder input");
    }

    uint8_t* address = pending.memory == nullptr ? nullptr : OH_AVMemory_GetAddr(pending.memory);
    if (address == nullptr || frame.size() > pending.capacity) {
      MarkFatal(impl);
      FinishDrain(impl);
      return;
    }
    std::memcpy(address, frame.data(), frame.size());
    OH_AVCodecBufferAttr attr{};
    attr.pts = pts;
    attr.size = static_cast<int32_t>(frame.size());
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    if (OH_AudioDecoder_PushInputData(decoder, pending.index, attr) != AV_ERR_OK) {
      OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "decoder input push failed");
      MarkFatal(impl);
      FinishDrain(impl);
      return;
    }
  }
}

void OnVoiceNeedInput(OH_AVCodec* codec, uint32_t index, OH_AVMemory* data, void* userData) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  CallbackGuard guard(impl);
  if (data == nullptr) return;
  int32_t capacity = OH_AVMemory_GetSize(data);
  if (capacity <= 0) return;
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    if (impl->stopping || impl->fatal || codec != impl->decoder) return;
    impl->pendingInputs.push_back(
        VoicePlayer::Impl::PendingInput{index, data, static_cast<size_t>(capacity)});
  }
  DrainDecoderInput(impl);
}

void OnVoiceNewOutput(OH_AVCodec* codec, uint32_t index, OH_AVMemory* data,
                      OH_AVCodecBufferAttr* attr, void* userData) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  CallbackGuard guard(impl);
  if (data != nullptr && attr != nullptr && attr->size > 0 && attr->offset >= 0 &&
      (attr->flags & AVCODEC_BUFFER_FLAGS_EOS) == 0) {
    int32_t capacity = OH_AVMemory_GetSize(data);
    size_t offset = static_cast<size_t>(attr->offset);
    size_t size = static_cast<size_t>(attr->size);
    if (capacity >= 0 && offset <= static_cast<size_t>(capacity) &&
        size <= static_cast<size_t>(capacity) - offset) {
      uint8_t* address = OH_AVMemory_GetAddr(data);
      if (address != nullptr) impl->PushPcm(address + offset, size);
    }
  }
  if (codec != nullptr && OH_AudioDecoder_FreeOutputData(codec, index) != AV_ERR_OK) {
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG, "decoder output release failed");
  }
}

void OnVoiceError(OH_AVCodec*, int32_t error, void* userData) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  CallbackGuard guard(impl);
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "decoder error %{public}d", error);
  MarkFatal(impl);
}

void OnVoiceStreamChange(OH_AVCodec*, OH_AVFormat* format, void* userData) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  CallbackGuard guard(impl);
  int32_t rate = 0;
  int32_t channels = 0;
  if (format != nullptr) {
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, &rate);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, &channels);
  }
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    if (rate > 0) impl->config.sampleRate = rate;
    if (channels > 0) impl->config.channels = channels;
    rate = impl->config.sampleRate;
    channels = impl->config.channels;
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "pcm %{public}dHz x%{public}d",
               rate, channels);
}

int32_t OnVoiceWriteData(OH_AudioRenderer*, void* userData, void* buffer, int32_t length) {
  auto* impl = static_cast<VoicePlayer::Impl*>(userData);
  CallbackGuard guard(impl);
  if (buffer == nullptr || length <= 0) return AUDIOSTREAM_ERROR_INVALID_PARAM;
  impl->ReadPcm(static_cast<uint8_t*>(buffer), static_cast<size_t>(length));
  return AUDIOSTREAM_SUCCESS;
}

void DestroyLocalResources(OH_AVCodec* decoder, OH_AudioStreamBuilder* builder,
                           OH_AudioRenderer* renderer) {
  if (renderer != nullptr) OH_AudioRenderer_Release(renderer);
  if (builder != nullptr) OH_AudioStreamBuilder_Destroy(builder);
  if (decoder != nullptr) OH_AudioDecoder_Destroy(decoder);
}

void ShutdownImpl(VoicePlayer::Impl* impl) {
  OH_AVCodec* decoder = nullptr;
  OH_AudioStreamBuilder* builder = nullptr;
  OH_AudioRenderer* renderer = nullptr;
  bool stopDecoder = false;
  bool stopRenderer = false;
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->stopping = true;
    decoder = impl->decoder;
    builder = impl->builder;
    renderer = impl->renderer;
    stopDecoder = impl->decoderStartAttempted;
    stopRenderer = impl->rendererStartAttempted;
  }

  if (stopRenderer && renderer != nullptr) OH_AudioRenderer_Stop(renderer);
  if (stopDecoder && decoder != nullptr) OH_AudioDecoder_Stop(decoder);

  {
    std::unique_lock<std::mutex> lock(impl->mu);
    impl->cv.wait(lock, [impl]() {
      return impl->callbacksInFlight == 0 && !impl->drainActive;
    });
  }

  if (renderer != nullptr) OH_AudioRenderer_Release(renderer);
  if (builder != nullptr) OH_AudioStreamBuilder_Destroy(builder);
  if (decoder != nullptr) OH_AudioDecoder_Destroy(decoder);

  {
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->decoder = nullptr;
    impl->builder = nullptr;
    impl->renderer = nullptr;
    impl->decoderStarted = false;
    impl->rendererStarted = false;
    impl->pendingInputs.clear();
    impl->frames.clear();
    impl->queuedFrameBytes = 0;
    impl->adtsTail.clear();
    impl->pcmRead = 0;
    impl->pcmWrite = 0;
    impl->pcmSize = 0;
  }
}

bool InitializePlayback(VoicePlayer::Impl* impl, const AacConfig& config) {
  OH_AVCodec* decoder = OH_AudioDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_AAC);
  if (decoder == nullptr) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "decoder create failed");
    return false;
  }

  OH_AVCodecAsyncCallback callbacks{};
  callbacks.onError = OnVoiceError;
  callbacks.onStreamChanged = OnVoiceStreamChange;
  callbacks.onNeedInputData = OnVoiceNeedInput;
  callbacks.onNeedOutputData = OnVoiceNewOutput;
  if (OH_AudioDecoder_SetCallback(decoder, callbacks, impl) != AV_ERR_OK) {
    DestroyLocalResources(decoder, nullptr, nullptr);
    return false;
  }

  OH_AVFormat* format = OH_AVFormat_Create();
  std::vector<uint8_t> codecConfig =
      AacConfigBytes(config.profileBits, config.sfIndex, config.channels);
  bool formatOk =
      format != nullptr &&
      OH_AVFormat_SetStringValue(format, OH_MD_KEY_CODEC_MIME, "audio/mp4a-latm") &&
      OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, config.sampleRate) &&
      OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, config.channels) &&
      OH_AVFormat_SetBuffer(format, OH_MD_KEY_CODEC_CONFIG, codecConfig.data(),
                            codecConfig.size());
  if (!formatOk || OH_AudioDecoder_Configure(decoder, format) != AV_ERR_OK) {
    if (format != nullptr) OH_AVFormat_Destroy(format);
    DestroyLocalResources(decoder, nullptr, nullptr);
    return false;
  }
  OH_AVFormat_Destroy(format);
  if (OH_AudioDecoder_Prepare(decoder) != AV_ERR_OK) {
    DestroyLocalResources(decoder, nullptr, nullptr);
    return false;
  }

  OH_AudioStreamBuilder* builder = nullptr;
  OH_AudioRenderer* renderer = nullptr;
  if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) !=
          AUDIOSTREAM_SUCCESS ||
      builder == nullptr ||
      OH_AudioStreamBuilder_SetSamplingRate(builder, config.sampleRate) !=
          AUDIOSTREAM_SUCCESS ||
      OH_AudioStreamBuilder_SetChannelCount(builder, config.channels) !=
          AUDIOSTREAM_SUCCESS ||
      OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE) !=
          AUDIOSTREAM_SUCCESS ||
      OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW) !=
          AUDIOSTREAM_SUCCESS ||
      OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_VOICE_COMMUNICATION) !=
          AUDIOSTREAM_SUCCESS) {
    DestroyLocalResources(decoder, builder, renderer);
    return false;
  }
  OH_AudioRenderer_Callbacks rendererCallbacks{};
  rendererCallbacks.OH_AudioRenderer_OnWriteData = OnVoiceWriteData;
  if (OH_AudioStreamBuilder_SetRendererCallback(builder, rendererCallbacks, impl) !=
          AUDIOSTREAM_SUCCESS ||
      OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer) != AUDIOSTREAM_SUCCESS ||
      renderer == nullptr) {
    DestroyLocalResources(decoder, builder, renderer);
    return false;
  }

  bool stopping = false;
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    stopping = impl->stopping;
    if (!stopping) {
      impl->decoder = decoder;
      impl->builder = builder;
      impl->renderer = renderer;
      impl->config = config;
      impl->rendererStartAttempted = true;
    }
  }
  if (stopping) {
    DestroyLocalResources(decoder, builder, renderer);
    return false;
  }

  if (OH_AudioRenderer_Start(renderer) != AUDIOSTREAM_SUCCESS) {
    ShutdownImpl(impl);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->rendererStarted = true;
    impl->decoderStartAttempted = true;
  }
  if (OH_AudioDecoder_Start(decoder) != AV_ERR_OK) {
    ShutdownImpl(impl);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->decoderStarted = true;
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "talk-back started %{public}dHz x%{public}d", config.sampleRate,
               config.channels);
  return true;
}

struct ParsedFeed {
  std::vector<uint8_t> tail;
  std::deque<std::vector<uint8_t>> frames;
  size_t frameBytes = 0;
  AacConfig config;
  bool haveConfig = false;
  bool sawInvalidPrefix = false;
  bool hardInvalid = false;
};

ParsedFeed ParseFeedBytes(const std::vector<uint8_t>& oldTail, const uint8_t* data,
                          size_t size, bool haveConfig, const AacConfig& currentConfig) {
  ParsedFeed parsed;
  parsed.haveConfig = haveConfig;
  parsed.config = currentConfig;

  std::vector<uint8_t> input;
  input.reserve(oldTail.size() + size);
  input.insert(input.end(), oldTail.begin(), oldTail.end());
  input.insert(input.end(), data, data + size);

  size_t position = 0;
  while (position < input.size()) {
    size_t remaining = input.size() - position;
    if (remaining < 2) {
      if (input[position] == 0xff) {
        parsed.tail.assign(input.begin() + static_cast<std::ptrdiff_t>(position), input.end());
      } else {
        parsed.sawInvalidPrefix = true;
      }
      break;
    }
    if (input[position] != 0xff || (input[position + 1] & 0xf6) != 0xf0) {
      parsed.sawInvalidPrefix = true;
      ++position;
      continue;
    }
    if (remaining < 7) {
      parsed.tail.assign(input.begin() + static_cast<std::ptrdiff_t>(position), input.end());
      break;
    }

    AdtsInfo info{};
    if (!ParseAdts(input.data() + position, remaining, &info)) {
      parsed.sawInvalidPrefix = true;
      ++position;
      continue;
    }
    if (remaining < static_cast<size_t>(info.frameLen)) {
      parsed.tail.assign(input.begin() + static_cast<std::ptrdiff_t>(position), input.end());
      break;
    }
    if (!parsed.haveConfig) {
      parsed.config = ConfigFromAdts(info);
      parsed.haveConfig = true;
    } else if (!SameConfig(parsed.config, info)) {
      parsed.hardInvalid = true;
      return parsed;
    }

    size_t rawSize = static_cast<size_t>(info.frameLen - info.headerLen);
    std::vector<uint8_t> frame(input.begin() + static_cast<std::ptrdiff_t>(position + info.headerLen),
                               input.begin() + static_cast<std::ptrdiff_t>(position + info.frameLen));
    parsed.frameBytes += rawSize;
    parsed.frames.push_back(std::move(frame));
    position += static_cast<size_t>(info.frameLen);
  }
  if (parsed.tail.size() > kMaxAdtsTailBytes) parsed.hardInvalid = true;
  return parsed;
}

}  // namespace

VoicePlayer::VoicePlayer() = default;

VoicePlayer::~VoicePlayer() { Stop(); }

VoicePlayer::FeedResult VoicePlayer::Feed(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return FeedResult::Invalid;
  std::lock_guard<std::mutex> apiLock(apiMu_);

  if (impl_ == nullptr) {
    try {
      impl_ = std::make_unique<Impl>(&active_);
    } catch (const std::bad_alloc&) {
      return FeedResult::Unavailable;
    }
  }
  Impl* impl = impl_.get();

  ParsedFeed parsed;
  try {
    std::lock_guard<std::mutex> lock(impl->mu);
    if (impl->stopping || impl->fatal) return FeedResult::Unavailable;
    parsed = ParseFeedBytes(impl->adtsTail, data, size, impl->haveConfig, impl->config);
    if (parsed.hardInvalid) return FeedResult::Invalid;
    if (impl->frames.size() + parsed.frames.size() > kMaxQueuedFrames ||
        parsed.frameBytes > kMaxQueuedFrameBytes -
                                std::min(kMaxQueuedFrameBytes, impl->queuedFrameBytes)) {
      return FeedResult::Busy;
    }

    impl->adtsTail = parsed.tail;
    for (const auto& frame : parsed.frames) impl->frames.push_back(frame);
    impl->queuedFrameBytes += parsed.frameBytes;
    if (parsed.haveConfig) {
      impl->config = parsed.config;
      impl->haveConfig = true;
    }
  } catch (const std::bad_alloc&) {
    return FeedResult::Busy;
  }

  bool haveAcceptedData = false;
  bool needsInitialization = false;
  AacConfig config;
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    haveAcceptedData = parsed.frameBytes > 0 || !impl->adtsTail.empty();
    needsInitialization = impl->decoder == nullptr && impl->haveConfig && !impl->frames.empty();
    config = impl->config;
  }
  if (!haveAcceptedData) return FeedResult::Invalid;

  if (needsInitialization) {
    if (!InitializePlayback(impl, config)) {
      impl_.reset();
      active_.store(false);
      return FeedResult::Unavailable;
    }
    active_.store(true);
  }
  DrainDecoderInput(impl);
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    if (impl->fatal) return FeedResult::Unavailable;
  }
  return FeedResult::Accepted;
}

void VoicePlayer::Stop() {
  std::lock_guard<std::mutex> apiLock(apiMu_);
  active_.store(false);
  if (impl_ == nullptr) return;
  ShutdownImpl(impl_.get());
  impl_.reset();
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "talk-back stopped");
}

}  // namespace ipcam
