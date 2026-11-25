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

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "OpusStream"

namespace {

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

std::vector<uint8_t> OpusStream::BuildOpusHead(int inputRate, int encodedChannels) {
  std::vector<uint8_t> p;
  p.insert(p.end(), {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'});
  p.push_back(1);                        // version
  p.push_back(static_cast<uint8_t>(encodedChannels));
  p.push_back(0x38);                     // pre-skip 0x0138 = 312 (libopus delay)
  p.push_back(0x01);
  for (int i = 0; i < 4; ++i) {          // input sample rate
    p.push_back(static_cast<uint8_t>((inputRate >> (8 * i)) & 0xFF));
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
  if (encoder_) return true;
  OH_AVCodecAsyncCallback cb{};
  cb.onError = OnError;
  cb.onStreamChanged = OnStreamChanged;
  cb.onNeedInputData = OnNeedInputData;
  cb.onNeedOutputData = OnNewOutputData;
  OH_AVCapability* soft = OH_AVCodec_GetCapabilityByCategory(OH_AVCODEC_MIMETYPE_AUDIO_OPUS, true, SOFTWARE);
  if (!soft && !OH_AVCodec_GetCapability(OH_AVCODEC_MIMETYPE_AUDIO_OPUS, true)) return false;
  const char* name = soft ? OH_AVCapability_GetName(soft) : nullptr;
  const int preferred = IsOpusRate(sampleRate_) ? sampleRate_ : 48000;
  const int rates[] = {preferred, 48000, 24000, 16000, 12000, 8000};
  // Rate conversion can preserve stereo; only mix channels if no matching
  // channel count works at ANY native Opus input rate.
  for (int channels : {channels_, 3 - channels_}) {
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
      if (i && rates[i] == preferred) continue;
      for (bool byName : {true, false}) {
        if (byName && (!name || !*name)) continue;
        for (bool asFloat : {false, true}) {
          OH_AVCodec* codec = byName ? OH_AudioEncoder_CreateByName(name) :
                                       OH_AudioEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_OPUS);
          if (!codec) break;
          OH_AVFormat* format = OH_AVFormat_Create();
          bool ok = format && OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, rates[i]) &&
                    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, channels) &&
                    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT,
                                            asFloat ? SAMPLE_F32LE : SAMPLE_S16LE) &&
                    OH_AudioEncoder_SetCallback(codec, cb, this) == AV_ERR_OK &&
                    OH_AudioEncoder_Configure(codec, format) == AV_ERR_OK &&
                    OH_AudioEncoder_Prepare(codec) == AV_ERR_OK;
          if (format) OH_AVFormat_Destroy(format);
          if (!ok) {
            OH_AudioEncoder_Destroy(codec);
            continue;
          }
          {
            std::lock_guard<std::mutex> lk(pcmMu_);
            converter_.Reset(sampleRate_, channels_, rates[i], channels);
            encRate_ = rates[i]; encChannels_ = channels; inputFloat_ = asFloat;
            nextSampleFrame_ = 0;
            encoderFailed_ = false;
            encoderDestroying_ = false;
            encoder_ = codec;
          }
          // Native Start/Stop may wait for callbacks: neither the PCM mutex
          // nor the clients mutex can be held across these calls.
          if (OH_AudioEncoder_Start(codec) != AV_ERR_OK || encoderFailed_) {
            DestroyEncoderLocked();
            continue;
          }
          headPages_ = BuildOggPage(0x02, 0, pageSerial_, 0, BuildOpusHead(sampleRate_, channels));
          auto tags = BuildOggPage(0x00, 0, pageSerial_, 1, BuildOpusTags());
          headPages_.insert(headPages_.end(), tags.begin(), tags.end());
          return true;
        }
      }
    }
  }
  return false;
}

void OpusStream::DestroyEncoderLocked() {
  OH_AVCodec* codec;
  {
    std::unique_lock<std::mutex> lk(pcmMu_);
    encoderDestroying_ = true;
    pcmCv_.wait(lk, [this] { return !drainActive_; });
    codec = encoder_;
    encoder_ = nullptr;
    pendingInputs_.clear();
    pcmQueue_.clear();
  }
  if (codec) {
    OH_AudioEncoder_Stop(codec);
    OH_AudioEncoder_Destroy(codec);
  }
}

bool OpusStream::EnsureReady() {
  std::lock_guard<std::mutex> lk(controlMu_);
  if (!SourceActive()) return false;
  if (encoder_) return !encoderFailed_;
  if (!readyChecked_) {
    encoderAvailable_ = CreateEncoderLocked();
    DestroyEncoderLocked();
    readyChecked_ = true;
  }
  return encoderAvailable_;
}

void OpusStream::OnError(OH_AVCodec* codec, int32_t errorCode, void* userData) {
  auto self = static_cast<OpusStream*>(userData);
  OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "encoder error %{public}d", errorCode);
  std::lock_guard<std::mutex> lk(self->pcmMu_);
  if (self->encoder_ == codec) self->encoderFailed_ = true;
  self->clientsCv_.notify_all();
}

void OpusStream::OnStreamChanged(OH_AVCodec*, OH_AVFormat*, void*) {}

void OpusStream::OnNeedInputData(OH_AVCodec* codec, uint32_t index, OH_AVMemory* data,
                                 void* userData) {
  auto self = static_cast<OpusStream*>(userData);
  {
    std::lock_guard<std::mutex> lk(self->pcmMu_);
    if (self->encoder_ != codec || self->encoderDestroying_) return;
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
      if (encoder_ == nullptr || encoderDestroying_ || pendingInputs_.empty() ||
          pcmQueue_.empty()) {
        drainActive_ = false;
        pcmCv_.notify_all();
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
      const size_t frameBytes = 2 * encChannels_;
      const size_t outFrameBytes = (inputFloat_ ? 4 : 2) * encChannels_;
      const size_t outCapacity = static_cast<size_t>(capacity) / outFrameBytes;
      // One complete Opus frame per buffer (20, 10, 5 or 2.5 ms).
      size_t samples = encRate_ / 50;
      while (samples > outCapacity && samples > static_cast<size_t>(encRate_ / 400)) samples /= 2;
      const size_t filled = samples * frameBytes;
      if (samples > outCapacity || pcmQueue_.size() < filled) {
        if (samples > outCapacity) encoderFailed_ = true;
        drainActive_ = false;
        pcmCv_.notify_all();
        return;
      }
      pushBytes = samples * outFrameBytes;
      if (inputFloat_) {
        for (size_t i = 0; i < samples * encChannels_; ++i) {
          float v = OpusPcmConverter::Read(pcmQueue_.data() + 2 * i) / 32768.0f;
          memcpy(dst + 4 * i, &v, sizeof(v));
        }
      } else {
        memcpy(dst, pcmQueue_.data(), filled);
      }
      pcmQueue_.erase(pcmQueue_.begin(),
                      pcmQueue_.begin() + static_cast<long>(filled));
      pendingInputs_.pop_front();
      ptsUs = static_cast<uint64_t>(nextSampleFrame_ * 1000000ULL) / encRate_;
      nextSampleFrame_ += samples;
    }
    OH_AVCodecBufferAttr attr{};
    attr.size = static_cast<int32_t>(pushBytes);
    attr.pts = static_cast<int64_t>(ptsUs);
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    if (OH_AudioEncoder_PushInputData(encoder_, pending.index, attr) != AV_ERR_OK) {
      encoderFailed_ = true;
      std::lock_guard<std::mutex> lk(pcmMu_);
      drainActive_ = false;
      pcmCv_.notify_all();
      return;
    }
  }
}

void OpusStream::OnNewOutputData(OH_AVCodec* codec, uint32_t index, OH_AVMemory* data,
                                 OH_AVCodecBufferAttr* attr, void* userData) {
  auto self = static_cast<OpusStream*>(userData);
  if (attr && data && !(attr->flags & (AVCODEC_BUFFER_FLAGS_EOS | AVCODEC_BUFFER_FLAGS_CODEC_DATA)) &&
      attr->size > 0 && attr->size < 255 * 255 && attr->offset >= 0) {
    const auto* base = OH_AVMemory_GetAddr(data);
    const int capacity = OH_AVMemory_GetSize(data);
    if (base && attr->offset <= capacity && attr->size <= capacity - attr->offset) {
      const uint8_t* packet = base + attr->offset;
      const unsigned ticks = OpusPacketTicks(packet, attr->size);
      if (ticks) self->BroadcastPacket({packet, packet + attr->size}, ticks);
    }
  }
  OH_AudioEncoder_FreeOutputData(codec, index);
}

void OpusStream::BroadcastPacket(const std::vector<uint8_t>& packet, unsigned ticks) {
  std::lock_guard<std::mutex> lk(clientsMu_);
  for (auto& client : clients_) {
    if (client->closed) continue;
    if (client->pages.size() >= kClientPageLimit) {
      client->closed = true;
      continue;
    }
    client->granule += ticks;
    client->pages.push_back(BuildOggPage(0, client->granule, pageSerial_, client->sequence++, packet));
  }
  clientsCv_.notify_all();
}

void OpusStream::FeedPcm(const uint8_t* pcm, size_t bytes, int sampleRate, int channels) {
  if (!SourceActive() || !pcm || !bytes) return;
  bool needDrain;
  {
    std::lock_guard<std::mutex> lk(pcmMu_);
    if (!encoder_ || encoderDestroying_ || encoderFailed_ || sampleRate != sampleRate_ || channels != channels_) return;
    converter_.Append(pcm, bytes / (2 * channels), pcmQueue_);
    if (pcmQueue_.size() > kPcmQueueLimit) {
      const size_t frameBytes = 2 * encChannels_;
      size_t drop = pcmQueue_.size() - kPcmQueueLimit / 2;
      drop -= drop % frameBytes;
      pcmQueue_.erase(pcmQueue_.begin(), pcmQueue_.begin() + drop);
      nextSampleFrame_ += drop / frameBytes;
    }
    needDrain = !pendingInputs_.empty();
  }
  if (needDrain) DrainPendingInputs();
}

bool OpusStream::Serve(int fd, const std::atomic<bool>& serverRunning) {
  auto client = std::make_shared<Client>();
  client->fd = fd;
  std::vector<uint8_t> headPages;
  {
    std::lock_guard<std::mutex> lifecycle(controlMu_);
    if (!SourceActive() || !CreateEncoderLocked()) return false;
    headPages = headPages_;
    std::lock_guard<std::mutex> lk(clientsMu_);
    clients_.push_back(client);
  }

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
    std::lock_guard<std::mutex> lifecycle(controlMu_);
    bool last = false;
    {
      std::lock_guard<std::mutex> lk(clientsMu_);
      auto it = std::find(clients_.begin(), clients_.end(), client);
      if (it != clients_.end()) {
        clients_.erase(it);
        last = clients_.empty();
      }
    }
    if (last) DestroyEncoderLocked();
  }
  return !client->closed;
}

void OpusStream::CloseClientsLocked() {
  std::lock_guard<std::mutex> lk(clientsMu_);
  for (auto& client : clients_) client->closed = true;
  clients_.clear();
  clientsCv_.notify_all();
}

void OpusStream::SetSourceActive(bool active, int sampleRate, int channels) {
  std::lock_guard<std::mutex> lifecycle(controlMu_);
  if (active && (sampleRate <= 0 || (channels != 1 && channels != 2))) active = false;
  if (active && SourceActive() && sampleRate_ == sampleRate && channels_ == channels) return;
  sourceActive_ = false;
  CloseClientsLocked();
  DestroyEncoderLocked();
  readyChecked_ = false;
  if (active) {
    std::lock_guard<std::mutex> lk(pcmMu_);
    sampleRate_ = sampleRate;
    channels_ = channels;
    sourceActive_ = true;
  }
}

void OpusStream::Shutdown() { SetSourceActive(false); }

}  // namespace ipcam
