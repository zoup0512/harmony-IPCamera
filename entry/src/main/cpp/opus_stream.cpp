#include "opus_stream.h"

#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_audioencoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmemory.h>

#define LOG_DOMAIN 0xC010
#define LOG_TAG "OpusStream"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kBitrate = 32000;
constexpr size_t kPcmQueueLimit = 1u << 19;  // 512 KiB ≈ 5.4s of mono S16 @48k
constexpr size_t kClientPageLimit = 400;     // ≈ 8s of 20ms pages per client

// Ogg CRC32: polynomial 0x04c11db7, MSB first, init 0, no final xor.
struct OggCrcTable {
  uint32_t value[256];
  OggCrcTable() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i << 24;
      for (int b = 0; b < 8; ++b) {
        r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
      }
      value[i] = r;
    }
  }
};

const OggCrcTable kOggCrc{};

uint32_t OggCrc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc = (crc << 8) ^ kOggCrc.value[((crc >> 24) & 0xFF) ^ data[i]];
  }
  return crc;
}

}  // namespace

namespace ipcam {

OpusStream::~OpusStream() { Shutdown(); }

std::vector<uint8_t> OpusStream::BuildOpusHead() {
  std::vector<uint8_t> p;
  p.insert(p.end(), {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'});
  p.push_back(1);                        // version
  p.push_back(static_cast<uint8_t>(kChannels));
  p.push_back(0x38);                     // pre-skip 0x0138 = 312 (libopus delay)
  p.push_back(0x01);
  for (int i = 0; i < 4; ++i) {          // input sample rate
    p.push_back(static_cast<uint8_t>((kSampleRate >> (8 * i)) & 0xFF));
  }
  p.push_back(0);                        // output gain lsb
  p.push_back(0);                        // output gain msb
  p.push_back(0);                        // channel mapping family
  return p;
}

std::vector<uint8_t> OpusStream::BuildOpusTags() {
  std::vector<uint8_t> p;
  p.insert(p.end(), {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
  const char* vendor = "harmony-ipcamera";
  uint32_t vlen = static_cast<uint32_t>(strlen(vendor));
  for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>((vlen >> (8 * i)) & 0xFF));
  p.insert(p.end(), vendor, vendor + vlen);
  for (int i = 0; i < 4; ++i) p.push_back(0);  // comment count = 0
  return p;
}

std::vector<uint8_t> OpusStream::BuildOggPage(uint8_t headerType, uint64_t granule,
                                              uint32_t serial, uint32_t sequence,
                                              const std::vector<uint8_t>& packet) {
  std::vector<uint8_t> page;
  page.reserve(27 + packet.size() + (packet.size() / 255) + 1);
  page.insert(page.end(), {'O', 'g', 'g', 'S'});
  page.push_back(0);           // version
  page.push_back(headerType);
  for (int i = 0; i < 8; ++i) page.push_back(static_cast<uint8_t>((granule >> (8 * i)) & 0xFF));
  for (int i = 0; i < 4; ++i) page.push_back(static_cast<uint8_t>((serial >> (8 * i)) & 0xFF));
  for (int i = 0; i < 4; ++i) page.push_back(static_cast<uint8_t>((sequence >> (8 * i)) & 0xFF));
  page.push_back(0);
  page.push_back(0);
  page.push_back(0);
  page.push_back(0);           // CRC placeholder

  // segment lacing table
  size_t remaining = packet.size();
  size_t segments = remaining / 255 + 1;
  page.push_back(static_cast<uint8_t>(segments));
  while (remaining >= 255) {
    page.push_back(255);
    remaining -= 255;
  }
  page.push_back(static_cast<uint8_t>(remaining));

  page.insert(page.end(), packet.begin(), packet.end());

  uint32_t crc = OggCrc32(page.data(), page.size());
  for (int i = 0; i < 4; ++i) {
    page[22 + i] = static_cast<uint8_t>((crc >> (8 * i)) & 0xFF);
  }
  return page;
}

bool OpusStream::CreateEncoderLocked() {
  if (encoder_ != nullptr) return true;
  // Log what the system opus encoder actually supports so configuration can
  // be adapted per device (some builds reject S16LE or explicit bitrates).
  OH_AVCapability* cap = OH_AVCodec_GetCapability(OH_AVCODEC_MIMETYPE_AUDIO_OPUS, true);
  if (cap == nullptr) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                 "no opus encoder capability registered");
    return false;
  }
  {
    const int32_t* rates = nullptr;
    uint32_t rateCount = 0;
    if (OH_AVCapability_GetAudioSupportedSampleRates(cap, &rates, &rateCount) ==
            AV_ERR_OK &&
        rates != nullptr) {
      std::string list;
      for (uint32_t i = 0; i < rateCount && i < 24; ++i) {
        list += std::to_string(rates[i]);
        list += " ";
      }
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "opus encoder supports %{public}u rates: %{public}s", rateCount,
                   list.c_str());
    }
    OH_AVRange chRange{};
    if (OH_AVCapability_GetAudioChannelCountRange(cap, &chRange) == AV_ERR_OK) {
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "opus channel range [%{public}lld, %{public}lld]",
                   static_cast<long long>(chRange.minVal),
                   static_cast<long long>(chRange.maxVal));
    }
    OH_AVRange brRange{};
    if (OH_AVCapability_GetEncoderBitrateRange(cap, &brRange) == AV_ERR_OK) {
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "opus bitrate range [%{public}lld, %{public}lld]",
                   static_cast<long long>(brRange.minVal),
                   static_cast<long long>(brRange.maxVal));
    }
  }

  // Opus input format differs across builds (S16LE vs F32LE). Configure is a
  // one-shot state transition — after a failed Configure the codec instance
  // stays unusable (next Configure returns INVALID_STATE), so each variant
  // gets a freshly created encoder.
  struct ConfigVariant {
    int channels;
    bool setFormat;
    int32_t sampleFormat;
    bool floatInput;
    const char* label;
  };
  const ConfigVariant variants[] = {
      {1, true, SAMPLE_S16LE, false, "mono/S16LE"},
      {1, false, 0, false, "mono/bare"},
      {2, true, SAMPLE_S16LE, false, "stereo/S16LE"},
      {2, false, 0, false, "stereo/bare"},
      {1, true, SAMPLE_F32LE, true, "mono/F32LE"},
      {2, true, SAMPLE_F32LE, true, "stereo/F32LE"},
  };
  bool configured = false;
  (void)0;
  OH_AVErrCode lastErr = AV_ERR_UNKNOWN;
  // Diagnostic: some codec builds only accept the new buffer API; an old-style
  // SetCallback before Configure can lock the state machine. Try Configure on
  // a bare instance first to isolate the cause.
  if (encoder_ != nullptr) {
    OH_AudioEncoder_Destroy(encoder_);
    encoder_ = nullptr;
  }
  // Some vendor builds register opus only under its explicit codec name (or
  // only in the SOFTWARE category); CreateByMime then yields an unusable
  // instance whose Configure always returns UNSUPPORT. Probe both and create
  // by name when available.
  OH_AVCapability* softCap =
      OH_AVCodec_GetCapabilityByCategory(OH_AVCODEC_MIMETYPE_AUDIO_OPUS, true,
                                          SOFTWARE);
  if (softCap != nullptr) {
    const char* name = OH_AVCapability_GetName(softCap);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                 "opus software codec name: %{public}s", name != nullptr ? name : "(null)");
    if (name != nullptr && name[0] != '\0') {
      encoder_ = OH_AudioEncoder_CreateByName(name);
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "CreateByName -> %{public}p", encoder_);
      if (encoder_ != nullptr) {
        OH_AVCodecAsyncCallback cb0{};
        cb0.onError = OnError;
        cb0.onStreamChanged = OnStreamChanged;
        cb0.onNeedInputData = OnNeedInputData;
        cb0.onNeedOutputData = OnNewOutputData;
        if (OH_AudioEncoder_SetCallback(encoder_, cb0, this) == AV_ERR_OK) {
          OH_AVFormat* f = OH_AVFormat_Create();
          OH_AVFormat_SetIntValue(f, OH_MD_KEY_AUD_SAMPLE_RATE, kSampleRate);
          OH_AVFormat_SetIntValue(f, OH_MD_KEY_AUD_CHANNEL_COUNT, kChannels);
          OH_AVFormat_SetIntValue(f, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE);
          lastErr = OH_AudioEncoder_Configure(encoder_, f);
          OH_AVFormat_Destroy(f);
          OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                       "Configure(by-name)=%{public}d", static_cast<int>(lastErr));
          if (lastErr == AV_ERR_OK) {
            inputFloat_ = false;
            encChannels_ = kChannels;
            configured = true;

          }
        }
      }
    }
  }
  if (!configured && encoder_ != nullptr) {
    OH_AudioEncoder_Destroy(encoder_);
    encoder_ = nullptr;
  }
  encoder_ = OH_AudioEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_OPUS);
  if (encoder_ != nullptr) {
    OH_AVFormat* format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, kSampleRate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, kChannels);
    lastErr = OH_AudioEncoder_Configure(encoder_, format);
    OH_AVFormat_Destroy(format);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                 "Configure(no-callback)=%{public}d", static_cast<int>(lastErr));
    if (lastErr == AV_ERR_OK) {
      // State machine accepts configure without prior callback; the old-style
      // SetCallback was the blocker. Register callbacks now (INITIALIZED state
      // after configure still accepts them on most builds).
      OH_AVCodecAsyncCallback cb{};
      cb.onError = OnError;
      cb.onStreamChanged = OnStreamChanged;
      cb.onNeedInputData = OnNeedInputData;
      cb.onNeedOutputData = OnNewOutputData;
      if (OH_AudioEncoder_SetCallback(encoder_, cb, this) == AV_ERR_OK) {
        inputFloat_ = false;
        encChannels_ = kChannels;
        configured = true;

      }
    }
  }
  if (!configured) {
  for (const ConfigVariant& variant : variants) {
    if (encoder_ != nullptr) {
      OH_AudioEncoder_Destroy(encoder_);
      encoder_ = nullptr;
    }
    encoder_ = OH_AudioEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_OPUS);
    if (encoder_ == nullptr) {
      OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "CreateByMime null");
      return false;
    }
    OH_AVCodecAsyncCallback cb{};
    cb.onError = OnError;
    cb.onStreamChanged = OnStreamChanged;
    cb.onNeedInputData = OnNeedInputData;
    cb.onNeedOutputData = OnNewOutputData;
    if (OH_AudioEncoder_SetCallback(encoder_, cb, this) != AV_ERR_OK) {
      OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "SetCallback failed");
      OH_AudioEncoder_Destroy(encoder_);
      encoder_ = nullptr;
      return false;
    }
    OH_AVFormat* format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, kSampleRate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, variant.channels);
    if (variant.setFormat) {
      OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, variant.sampleFormat);
    }
    lastErr = OH_AudioEncoder_Configure(encoder_, format);
    OH_AVFormat_Destroy(format);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "Configure(%{public}s)=%{public}d",
                 variant.label, static_cast<int>(lastErr));
    if (lastErr == AV_ERR_OK) {
      inputFloat_ = variant.floatInput;
      encChannels_ = variant.channels;
      configured = true;
      break;
    }
  }  // variant loop
  }  // !configured guard (diagnostic path first)
  if (!configured) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                 "Configure all variants failed");
    OH_AudioEncoder_Destroy(encoder_);
    encoder_ = nullptr;
    return false;
  }
  if (OH_AudioEncoder_Prepare(encoder_) != AV_ERR_OK ||
      OH_AudioEncoder_Start(encoder_) != AV_ERR_OK) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "Prepare/Start failed");
    OH_AudioEncoder_Destroy(encoder_);
    encoder_ = nullptr;
    return false;
  }
  nextSampleFrame_ = 0;
  pcmQueue_.clear();
  pendingInputs_.clear();
  encoderFailed_.store(false);
  pageSequence_ = 0;
  granule_ = 0;
  headersSent_ = false;
  headPages_.clear();
  headPages_ = BuildOggPage(0x02, 0, pageSerial_, pageSequence_++, BuildOpusHead());
  std::vector<uint8_t> tagsPage =
      BuildOggPage(0x00, 0, pageSerial_, pageSequence_++, BuildOpusTags());
  headPages_.insert(headPages_.end(), tagsPage.begin(), tagsPage.end());
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "opus encoder started (%{public}dHz x%{public}d @%{public}d)", kSampleRate,
               kChannels, kBitrate);
  return true;
}

void OpusStream::DestroyEncoderLocked() {
  if (encoder_ != nullptr) {
    OH_AudioEncoder_Stop(encoder_);
    OH_AudioEncoder_Destroy(encoder_);
    encoder_ = nullptr;
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "opus encoder stopped");
  }
  pcmQueue_.clear();
  pendingInputs_.clear();
}

bool OpusStream::EnsureReady() {
  std::lock_guard<std::mutex> lk(pcmMu_);
  if (readyChecked_) return encoderAvailable_;
  // One-time probe: create and immediately destroy; the real encoder is
  // created when the first client connects.
  encoderAvailable_ = CreateEncoderLocked();
  DestroyEncoderLocked();
  readyChecked_ = true;
  return encoderAvailable_;
}

void OpusStream::OnError(OH_AVCodec*, int32_t errorCode, void* userData) {
  auto self = static_cast<OpusStream*>(userData);
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "encoder error %{public}d",
               errorCode);
  std::lock_guard<std::mutex> lk(self->pcmMu_);
  self->encoderFailed_ = true;
}

void OpusStream::OnStreamChanged(OH_AVCodec*, OH_AVFormat*, void*) {}

void OpusStream::OnNeedInputData(OH_AVCodec*, uint32_t index, OH_AVMemory* data,
                                 void* userData) {
  auto self = static_cast<OpusStream*>(userData);
  {
    std::lock_guard<std::mutex> lk(self->pcmMu_);
    if (self->encoder_ == nullptr) return;
    self->pendingInputs_.push_back({index, data});
  }
  self->DrainPendingInputs();
}

void OpusStream::DrainPendingInputs() {
  {
    std::lock_guard<std::mutex> lk(pcmMu_);
    if (drainActive_) return;
    drainActive_ = true;
  }
  for (;;) {
    PendingInput pending;
    size_t pushBytes = 0;
    uint64_t ptsUs = 0;
    {
      std::lock_guard<std::mutex> lk(pcmMu_);
      if (encoder_ == nullptr || pendingInputs_.empty() || pcmQueue_.empty()) {
        drainActive_ = false;
        return;
      }
      pending = pendingInputs_.front();
      uint8_t* dst = pending.memory != nullptr ? OH_AVMemory_GetAddr(pending.memory)
                                               : nullptr;
      int32_t capacity = pending.memory != nullptr ? OH_AVMemory_GetSize(pending.memory) : 0;
      if (dst == nullptr || capacity <= 0) {
        pendingInputs_.pop_front();
        continue;
      }
      // The PCM tap is mono S16LE; the encoder may be mono or stereo, S16 or F32.
      const size_t srcSamples = pcmQueue_.size() / 2;
      const size_t bytesPerSampleOut = inputFloat_ ? 4 : 2;
      const size_t outCapacity =
          static_cast<size_t>(capacity) / (bytesPerSampleOut * encChannels_);
      size_t samples = std::min(srcSamples, outCapacity);
      if (samples == 0) {
        drainActive_ = false;
        return;
      }
      const size_t filled = samples * 2;  // bytes consumed from the mono tap
      pushBytes = samples * bytesPerSampleOut * encChannels_;
      const int16_t* src = reinterpret_cast<const int16_t*>(pcmQueue_.data());
      if (inputFloat_) {
        float* out = reinterpret_cast<float*>(dst);
        for (size_t i = 0; i < samples; ++i) {
          float v = static_cast<float>(src[i]) / 32768.0f;
          for (int c = 0; c < encChannels_; ++c) out[i * encChannels_ + c] = v;
        }
      } else if (encChannels_ == 1) {
        memcpy(dst, pcmQueue_.data(), filled);
      } else {
        int16_t* out = reinterpret_cast<int16_t*>(dst);
        for (size_t i = 0; i < samples; ++i) {
          out[i * 2] = src[i];
          out[i * 2 + 1] = src[i];
        }
      }
      pcmQueue_.erase(pcmQueue_.begin(),
                      pcmQueue_.begin() + static_cast<long>(filled));
      pendingInputs_.pop_front();
      ptsUs = static_cast<uint64_t>(nextSampleFrame_ * 1000000ULL) / kSampleRate;
      nextSampleFrame_ += samples;
    }
    OH_AVCodecBufferAttr attr{};
    attr.size = static_cast<int32_t>(pushBytes);
    attr.pts = static_cast<int64_t>(ptsUs);
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    if (OH_AudioEncoder_PushInputData(encoder_, pending.index, attr) != AV_ERR_OK) {
      std::lock_guard<std::mutex> lk(pcmMu_);
      drainActive_ = false;
      return;
    }
  }
}

void OpusStream::OnNewOutputData(OH_AVCodec*, uint32_t index, OH_AVMemory* data,
                                 OH_AVCodecBufferAttr* attr, void* userData) {
  auto self = static_cast<OpusStream*>(userData);
  if (attr != nullptr && !(attr->flags & AVCODEC_BUFFER_FLAGS_EOS) && attr->size > 0 &&
      data != nullptr) {
    const uint8_t* packet = OH_AVMemory_GetAddr(data);
    if (packet != nullptr) {
      // Opus granule position = PCM samples @48k; derive it from the source
      // pts so encoder-internal delay handling does not matter.
      uint64_t granule = static_cast<uint64_t>(attr->pts) * kSampleRate / 1000000ULL;
      std::vector<uint8_t> body(packet, packet + attr->size);
      std::vector<uint8_t> page =
          BuildOggPage(0x00, granule, self->pageSerial_, self->pageSequence_++, body);
      self->BroadcastPage(page);
    }
  }
  OH_AudioEncoder_FreeOutputData(self->encoder_, index);
}

void OpusStream::BroadcastPage(const std::vector<uint8_t>& page) {
  std::lock_guard<std::mutex> lk(clientsMu_);
  for (auto& client : clients_) {
    if (client->closed) continue;
    if (client->pages.size() >= kClientPageLimit) {
      client->closed = true;  // slow consumer: drop the connection
      continue;
    }
    client->pages.push_back(page);
  }
  clientsCv_.notify_all();
}

void OpusStream::FeedPcm(const uint8_t* pcm, size_t bytes, int sampleRate, int channels) {
  if (!sourceActive_.load() || bytes == 0) return;
  if (sampleRate != sampleRate_ || channels != channels_) return;
  bool needDrain = false;
  {
    std::lock_guard<std::mutex> lk(pcmMu_);
    if (encoder_ == nullptr || encoderFailed_.load()) return;
    pcmQueue_.insert(pcmQueue_.end(), pcm, pcm + bytes);
    if (pcmQueue_.size() > kPcmQueueLimit) {
      size_t drop = pcmQueue_.size() - kPcmQueueLimit / 2;
      drop -= drop % (2 * kChannels);
      pcmQueue_.erase(pcmQueue_.begin(), pcmQueue_.begin() + static_cast<long>(drop));
      nextSampleFrame_ += drop / (2 * kChannels);
    }
    needDrain = !pendingInputs_.empty();
  }
  if (needDrain) DrainPendingInputs();
}

bool OpusStream::Serve(int fd, const std::atomic<bool>& serverRunning) {
  if (!EnsureReady()) return false;

  auto client = std::make_shared<Client>();
  client->fd = fd;
  std::vector<uint8_t> headPages;
  {
    std::lock_guard<std::mutex> lk(pcmMu_);
    if (encoder_ == nullptr && !CreateEncoderLocked()) {
      return false;
    }
    headPages = headPages_;
  }
  {
    std::lock_guard<std::mutex> lk(clientsMu_);
    clients_.push_back(client);
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto sendAll = [&](const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
      pollfd p{fd, POLLOUT, 0};
      int r = ::poll(&p, 1, 2000);
      if (r <= 0) return false;
      ssize_t sent = ::send(fd, data + offset, size - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
      if (sent > 0) {
        offset += static_cast<size_t>(sent);
        continue;
      }
      if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      return false;
    }
    return true;
  };

  if (!sendAll(headPages.data(), headPages.size())) {
    client->closed = true;
  }

  while (!client->closed && serverRunning.load() && !encoderFailed_) {
    std::vector<uint8_t> page;
    {
      std::unique_lock<std::mutex> lk(clientsMu_);
      clientsCv_.wait_for(lk, std::chrono::seconds(1), [&client] {
        return client->closed || !client->pages.empty();
      });
      if (client->closed) break;
      if (client->pages.empty()) continue;
      page = std::move(client->pages.front());
      client->pages.pop_front();
    }
    if (!sendAll(page.data(), page.size())) {
      client->closed = true;
    }
  }

  {
    std::lock_guard<std::mutex> lk(clientsMu_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    bool none = clients_.empty();
    clientsCv_.notify_all();
    if (none) {
      std::lock_guard<std::mutex> lk2(pcmMu_);
      DestroyEncoderLocked();
    }
  }
  return !client->closed;
}

void OpusStream::Shutdown() {
  {
    std::lock_guard<std::mutex> lk(clientsMu_);
    for (auto& client : clients_) client->closed = true;
    clients_.clear();
    clientsCv_.notify_all();
  }
  std::lock_guard<std::mutex> lk(pcmMu_);
  DestroyEncoderLocked();
}

}  // namespace ipcam
