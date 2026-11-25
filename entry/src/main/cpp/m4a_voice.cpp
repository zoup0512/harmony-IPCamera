#include "m4a_voice.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <utility>
#include <sys/mman.h>
#include <unistd.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avdemuxer.h>

namespace ipcam {
namespace {
using Result = VoiceClipResult;
constexpr int kMaxUnit = 8191;  // never grow an SDK sample buffer on a read error
constexpr int64_t kDurationUs = kVoiceClipSeconds * 1000000LL;
constexpr int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                          22050, 16000, 12000, 11025, 8000, 7350};

bool Supported(const AdtsInfo& c) {
  // Normal AAC-LC (1024 samples/access unit), S16 mono/stereo renderer.
  return c.profileBits == 1 && c.channels >= 1 && c.channels <= 2 &&
         c.sampleRate >= 8000 && c.sampleRate <= 48000;
}

bool Append(VoiceClip& clip, const uint8_t* p, size_t n) {
  if (!n || n > kMaxUnit || n > kVoiceClipBytes - clip.bytes ||
      clip.frames.size() >= static_cast<size_t>(clip.config.sampleRate * kVoiceClipSeconds / 1024)) {
    return false;
  }
  clip.frames.emplace_back(p, p + n);
  clip.bytes += n;
  return true;
}

bool AdtsUnit(const uint8_t* p, size_t n, AdtsInfo& c) {
  return ParseAdts(p, n, &c) && static_cast<size_t>(c.frameLen) <= n &&
         (p[6] & 3) == 0 && Supported(c);
}

bool Matches(const AdtsInfo& a, const AdtsInfo& b) {
  return a.profileBits == b.profileBits && a.sfIndex == b.sfIndex && a.channels == b.channels;
}

Result ReadAdts(const uint8_t* data, size_t size, VoiceClip& clip) {
  while (size) {
    AdtsInfo c;
    if (!AdtsUnit(data, size, c)) return Result::Invalid;
    if (clip.frames.empty()) clip.config = c;
    if (!Matches(c, clip.config) || !Append(clip, data + c.headerLen, c.frameLen - c.headerLen)) {
      return Result::Invalid;
    }
    size -= c.frameLen;
    data += c.frameLen;
  }
  return Result::Ok;
}

// Not a BMFF parser: the native AVSource owns all container and track parsing.
// Only accept the AAC-LC AudioSpecificConfig that our decoder setup reproduces.
bool ReadConfig(OH_AVFormat* track, AdtsInfo& c) {
  int32_t type, rate, channels;
  const char* mime = nullptr;
  uint8_t* asc = nullptr;
  size_t n = 0;
  if (!OH_AVFormat_GetIntValue(track, OH_MD_KEY_TRACK_TYPE, &type) || type != MEDIA_TYPE_AUD ||
      !OH_AVFormat_GetStringValue(track, OH_MD_KEY_CODEC_MIME, &mime) || !mime ||
      std::strcmp(mime, OH_AVCODEC_MIMETYPE_AUDIO_AAC) != 0 ||
      !OH_AVFormat_GetIntValue(track, OH_MD_KEY_AUD_SAMPLE_RATE, &rate) ||
      !OH_AVFormat_GetIntValue(track, OH_MD_KEY_AUD_CHANNEL_COUNT, &channels) ||
      !OH_AVFormat_GetBuffer(track, OH_MD_KEY_CODEC_CONFIG, &asc, &n) || !asc || n < 2 || n > 5) {
    return false;
  }
  // Also allow FFmpeg's syncExtensionType=0x2b7, AOT=SBR, sbrPresentFlag=0.
  if (n != 2 && !(n == 5 && asc[2] == 0x56 && asc[3] == 0xe5 && asc[4] == 0)) return false;
  c.profileBits = (asc[0] >> 3) - 1;
  c.sfIndex = ((asc[0] & 7) << 1) | (asc[1] >> 7);
  c.channels = (asc[1] >> 3) & 15;
  c.sampleRate = rate;
  return (asc[1] & 7) == 0 && c.sfIndex < 13 && kRates[c.sfIndex] == rate &&
         c.channels == channels && Supported(c);
}

template <auto Destroy, class T>
auto Owned(T* p) { return std::unique_ptr<T, decltype(Destroy)>(p, Destroy); }

Result ReadM4a(const uint8_t* data, size_t size, VoiceClip& clip) {
  // Anonymous per request; no shared path, no temporary-file cleanup race.
  // Keep the fd open until after demuxer AND source destruction.
  struct Fd { int value; ~Fd() { if (value >= 0) close(value); } } fd{memfd_create("talkback", MFD_CLOEXEC)};
  if (fd.value < 0) return Result::Unavailable;
  size_t done = 0;
  while (done < size) {
    const auto n = write(fd.value, data + done, size - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return Result::Unavailable;
    done += static_cast<size_t>(n);
  }
  if (lseek(fd.value, 0, SEEK_SET) != 0) return Result::Unavailable;
  auto source = Owned<OH_AVSource_Destroy>(OH_AVSource_CreateWithFD(fd.value, 0, size));
  if (!source) return Result::Invalid;
  auto meta = Owned<OH_AVFormat_Destroy>(OH_AVSource_GetSourceFormat(source.get()));
  int32_t tracks = 0;
  int64_t duration = 0;
  if (!meta || !OH_AVFormat_GetIntValue(meta.get(), OH_MD_KEY_TRACK_COUNT, &tracks) || tracks != 1 ||
      !OH_AVFormat_GetLongValue(meta.get(), OH_MD_KEY_DURATION, &duration) || duration <= 0 ||
      duration > kDurationUs) return Result::Invalid;
  auto track = Owned<OH_AVFormat_Destroy>(OH_AVSource_GetTrackFormat(source.get(), 0));
  if (!track || !ReadConfig(track.get(), clip.config)) return Result::Invalid;
  auto demux = Owned<OH_AVDemuxer_Destroy>(OH_AVDemuxer_CreateWithSource(source.get()));
  if (!demux || OH_AVDemuxer_SelectTrackByID(demux.get(), 0) != AV_ERR_OK) return Result::Invalid;
  auto buf = Owned<OH_AVBuffer_Destroy>(OH_AVBuffer_Create(kMaxUnit));
  if (!buf) return Result::Unavailable;
  int64_t lastPts = -1000000;  // allow a leading AAC encoder priming frame
  for (;;) {
    OH_AVCodecBufferAttr a{};
    if (OH_AVDemuxer_ReadSampleBuffer(demux.get(), 0, buf.get()) != AV_ERR_OK ||
        OH_AVBuffer_GetBufferAttr(buf.get(), &a) != AV_ERR_OK) return Result::Invalid;
    if (a.flags == AVCODEC_BUFFER_FLAGS_EOS && !a.size) {
      return clip.frames.empty() ? Result::Invalid : Result::Ok;
    }
    int cap = OH_AVBuffer_GetCapacity(buf.get());
    if (cap > kMaxUnit) cap = kMaxUnit;  // SDK may round capacity up for alignment.
    if ((a.flags & ~AVCODEC_BUFFER_FLAGS_SYNC_FRAME) || a.size <= 0 ||
        a.offset < 0 || a.offset > cap || a.size > cap - a.offset ||
        a.pts < lastPts || a.pts > kDurationUs) return Result::Invalid;
    lastPts = a.pts;
    const uint8_t* p = OH_AVBuffer_GetAddr(buf.get());
    if (!p) return Result::Invalid;
    p += a.offset;
    size_t n = static_cast<size_t>(a.size);
    // Some demuxers expose AAC with ADTS transport headers. Never forward one
    // as raw AAC or silently accept a partial/mismatched ADTS access unit.
    if (n >= 2 && p[0] == 0xff && (p[1] & 0xf6) == 0xf0) {
      AdtsInfo c;
      if (!AdtsUnit(p, n, c) || static_cast<size_t>(c.frameLen) != n || !Matches(clip.config, c)) {
        return Result::Invalid;
      }
      p += c.headerLen;
      n -= c.headerLen;
    }
    if (!Append(clip, p, n)) return Result::Invalid;
  }
}
}  // namespace

VoiceClipResult ParseVoiceUpload(const uint8_t* data, size_t size, VoiceClip& out) {
  if (!data || !size || size > kVoiceUploadBytes) return Result::Invalid;
  try {
    VoiceClip candidate;
    Result result = size >= 8 && std::memcmp(data + 4, "ftyp", 4) == 0
                        ? ReadM4a(data, size, candidate) : ReadAdts(data, size, candidate);
    if (result == Result::Ok) out = std::move(candidate);
    return result;
  } catch (const std::bad_alloc&) {
    return Result::Unavailable;
  }
}
}  // namespace ipcam
