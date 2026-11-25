#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <utility>

// Synchronous callbacks intentionally challenge lock ordering (Start/Stop and
// PushInput can re-enter the app). No SDK services or audio hardware.
#define OH_LOG_Print(...) ((void)0)
constexpr int AUDIOSTREAM_SUCCESS = 0, AV_ERR_OK = 0, SOFTWARE = 1;
constexpr int AVCODEC_BUFFER_FLAGS_NONE = 0, AVCODEC_BUFFER_FLAGS_EOS = 1, AVCODEC_BUFFER_FLAGS_CODEC_DATA = 8;
constexpr int AUDIOSTREAM_TYPE_CAPTURER = 0, AUDIOSTREAM_SAMPLE_S16LE = 1, AUDIOSTREAM_ENCODING_TYPE_RAW = 0;
constexpr int SAMPLE_S16LE = 1, SAMPLE_F32LE = 2;
constexpr int OH_AVCODEC_MIMETYPE_AUDIO_AAC = 1, OH_AVCODEC_MIMETYPE_AUDIO_OPUS = 2;
constexpr int OH_MD_KEY_AUD_SAMPLE_RATE = 0, OH_MD_KEY_AUD_CHANNEL_COUNT = 1, OH_MD_KEY_AUDIO_SAMPLE_FORMAT = 2, OH_MD_KEY_BITRATE = 3;
using OH_AudioStream_EncodingType = int; using OH_AudioStream_SampleFormat = int;
struct OH_AVFormat { int v[4]{}; };
struct OH_AVCapability {};
struct OH_AVMemory { std::vector<uint8_t> bytes = std::vector<uint8_t>(8192); };
struct OH_AVCodecBufferAttr { int64_t pts = 0; int32_t size = 0, offset = 0, flags = 0; };
struct OH_AVCodec;
struct OH_AVCodecAsyncCallback {
  void (*onError)(OH_AVCodec*, int32_t, void*);
  void (*onStreamChanged)(OH_AVCodec*, OH_AVFormat*, void*);
  void (*onNeedInputData)(OH_AVCodec*, uint32_t, OH_AVMemory*, void*);
  void (*onNeedOutputData)(OH_AVCodec*, uint32_t, OH_AVMemory*, OH_AVCodecBufferAttr*, void*);
};
struct OH_AVCodec { int type; OH_AVFormat fmt{}; OH_AVCodecAsyncCallback cb{}; void* ctx{}; OH_AVMemory in; };
struct OH_AudioCapturer;
struct OH_AudioCapturer_Callbacks { int32_t (*OH_AudioCapturer_OnReadData)(OH_AudioCapturer*, void*, void*, int32_t); };
struct OH_AudioStreamBuilder { int rate{}, ch{}; OH_AudioCapturer_Callbacks cb{}; void* ctx{}; };
struct OH_AudioCapturer : OH_AudioStreamBuilder {};
namespace mock {
inline int builders = 0, capturers = 0, codecs = 0;
inline std::vector<std::pair<int,int>> attempts;
inline int acceptRate = 24000, acceptCh = 2, actualRate = 44100, actualCh = 2;
inline bool badRead = false, rejectAac = false;
inline int opusRate = 48000, opusCh = 2, opusFmt = SAMPLE_S16LE;
inline OH_AVFormat aacConfig;
inline OH_AudioCapturer* cap = nullptr;
inline std::vector<uint8_t> lastOpusInput;
inline std::vector<int64_t> opusPts;
}
inline int OH_AudioStreamBuilder_Create(OH_AudioStreamBuilder** out, int) { ++mock::builders; *out = new OH_AudioStreamBuilder; return 0; }
inline int OH_AudioStreamBuilder_Destroy(OH_AudioStreamBuilder* b) { --mock::builders; delete b; return 0; }
inline int OH_AudioStreamBuilder_SetSamplingRate(OH_AudioStreamBuilder* b, int rate) { b->rate = rate; return 0; }
inline int OH_AudioStreamBuilder_SetChannelCount(OH_AudioStreamBuilder* b, int ch) { b->ch = ch; return 0; }
inline int OH_AudioStreamBuilder_SetSampleFormat(OH_AudioStreamBuilder*, int) { return 0; }
inline int OH_AudioStreamBuilder_SetEncodingType(OH_AudioStreamBuilder*, int) { return 0; }
inline int OH_AudioStreamBuilder_SetCapturerCallback(OH_AudioStreamBuilder* b, OH_AudioCapturer_Callbacks cb, void* ctx) { b->cb = cb; b->ctx = ctx; return 0; }
inline int OH_AudioStreamBuilder_GenerateCapturer(OH_AudioStreamBuilder* b, OH_AudioCapturer** out) {
  mock::attempts.emplace_back(b->rate,b->ch);
  if (b->rate != mock::acceptRate || b->ch != mock::acceptCh) return 4;
  *out = new OH_AudioCapturer{*b}; ++mock::capturers; mock::cap = *out; return 0;
}
inline int OH_AudioCapturer_Stop(OH_AudioCapturer*) { return 0; }
inline int OH_AudioCapturer_Start(OH_AudioCapturer*) { return 0; }
inline int OH_AudioCapturer_Release(OH_AudioCapturer* c) { mock::cap = nullptr; delete c; --mock::capturers; return 0; }
inline int OH_AudioCapturer_GetSamplingRate(OH_AudioCapturer*, int* rate) { *rate = mock::actualRate; return 0; }
inline int OH_AudioCapturer_GetChannelCount(OH_AudioCapturer*, int* ch) { *ch = mock::actualCh; return 0; }
inline int OH_AudioCapturer_GetSampleFormat(OH_AudioCapturer*, int* fmt) { *fmt = AUDIOSTREAM_SAMPLE_S16LE; return mock::badRead ? 1 : 0; }
inline int OH_AudioCapturer_GetEncodingType(OH_AudioCapturer*, int* enc) { *enc = AUDIOSTREAM_ENCODING_TYPE_RAW; return 0; }
inline bool opusRegistered = true;
inline OH_AVCapability* OH_AVCodec_GetCapability(int, bool) { static OH_AVCapability cap; return opusRegistered ? &cap : nullptr; }
inline OH_AVCapability* OH_AVCodec_GetCapabilityByCategory(int type, bool enc, int) { return OH_AVCodec_GetCapability(type,enc); }
inline const char* OH_AVCapability_GetName(OH_AVCapability*) { return "mock.opus"; }
inline OH_AVCodec* OH_AudioEncoder_CreateByMime(int type) { ++mock::codecs; return new OH_AVCodec{type}; }
inline OH_AVCodec* OH_AudioEncoder_CreateByName(const char*) { return OH_AudioEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_OPUS); }
inline OH_AVFormat* OH_AVFormat_Create() { return new OH_AVFormat; }
inline void OH_AVFormat_Destroy(OH_AVFormat* f) { delete f; }
inline bool OH_AVFormat_SetIntValue(OH_AVFormat* f, int k, int v) { assert(k != OH_MD_KEY_BITRATE); f->v[k] = v; return true; }
inline bool OH_AVFormat_SetLongValue(OH_AVFormat* f, int k, int64_t v) { assert(k == OH_MD_KEY_BITRATE); f->v[k] = static_cast<int>(v); return true; }
inline int OH_AudioEncoder_SetCallback(OH_AVCodec* c, OH_AVCodecAsyncCallback cb, void* ctx) { c->cb = cb; c->ctx = ctx; return 0; }
inline int OH_AudioEncoder_Configure(OH_AVCodec* c, OH_AVFormat* f) {
  c->fmt = *f;
  if (c->type == OH_AVCODEC_MIMETYPE_AUDIO_AAC) { mock::aacConfig = *f; return mock::rejectAac ? 1 : 0; }
  return !(f->v[0] == mock::opusRate && f->v[1] == mock::opusCh && f->v[2] == mock::opusFmt);
}
inline int OH_AudioEncoder_Prepare(OH_AVCodec*) { return 0; }
inline int OH_AudioEncoder_Start(OH_AVCodec* c) { c->cb.onNeedInputData(c,0,&c->in,c->ctx); return 0; }
inline int OH_AudioEncoder_Stop(OH_AVCodec* c) { c->cb.onNeedInputData(c,0,&c->in,c->ctx); return 0; }
inline int OH_AudioEncoder_Destroy(OH_AVCodec* c) { --mock::codecs; delete c; return 0; }
inline uint8_t* OH_AVMemory_GetAddr(OH_AVMemory* mem) { return mem->bytes.data(); }
inline int32_t OH_AVMemory_GetSize(OH_AVMemory* mem) { return static_cast<int32_t>(mem->bytes.size()); }
inline int OH_AudioEncoder_FreeOutputData(OH_AVCodec*, uint32_t) { return 0; }
inline int OH_AudioEncoder_PushInputData(OH_AVCodec* c, uint32_t, OH_AVCodecBufferAttr a) {
  assert(a.size > 0);
  if (c->type == OH_AVCODEC_MIMETYPE_AUDIO_OPUS) {
    mock::lastOpusInput.assign(c->in.bytes.begin(), c->in.bytes.begin()+a.size);
    mock::opusPts.push_back(a.pts);
  }
  OH_AVMemory packet{{0x00, 0x98, 0x55}}; // offset=1; 0x98 is 20ms single Opus frame
  a.offset = 1; a.size = 2;
  c->cb.onNeedOutputData(c,0,&packet,&a,c->ctx);
  c->cb.onNeedInputData(c,0,&c->in,c->ctx);
  return 0;
}
