"""Linux/WSL: python3 tests/media_cache_reset_test.py. No device or Harmony SDK.

Build all three production translation units. The fake cold decoder returns a
flat NV12 frame whose level is the last byte of the supplied IDR; JPEG encoding
is gated in the fixture. RTSP uses real sockets restricted to loopback.
"""
import os
from pathlib import Path
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
CPP = TESTS.parent / 'entry/src/main/cpp'
SDK = r'''
#pragma once
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <vector>
struct OH_AVCodecBufferAttr { int32_t size = 0, flags = 0; };
struct OH_AVBuffer { std::vector<uint8_t> bytes; OH_AVCodecBufferAttr attr; };
inline auto OH_AVBuffer_GetAddr(OH_AVBuffer* b) { return b->bytes.data(); }
inline auto OH_AVBuffer_GetCapacity(OH_AVBuffer* b) { return b->bytes.size(); }
inline void OH_AVBuffer_SetBufferAttr(OH_AVBuffer* b, OH_AVCodecBufferAttr* a) { b->attr = *a; }
inline void OH_AVBuffer_GetBufferAttr(OH_AVBuffer* b, OH_AVCodecBufferAttr* a) { *a = b->attr; }
struct OH_AVCodec;
struct OH_AVFormat { int w, h; };
struct OH_AVCodecCallback {
  void (*onError)(OH_AVCodec*, int32_t, void*);
  void (*onStreamChanged)(OH_AVCodec*, OH_AVFormat*, void*);
  void (*onNeedInputBuffer)(OH_AVCodec*, uint32_t, OH_AVBuffer*, void*);
  void (*onNewOutputBuffer)(OH_AVCodec*, uint32_t, OH_AVBuffer*, void*);
};
struct OH_AVCodec { OH_AVCodecCallback cb; void* user; OH_AVFormat fmt; };
enum { AV_ERR_OK, AVCODEC_BUFFER_FLAGS_EOS, OH_MD_KEY_WIDTH, OH_MD_KEY_HEIGHT,
       OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12, AVCODEC_BUFFER_FLAGS_NONE = 0 };
inline auto OH_AVFormat_Create() { return new OH_AVFormat{}; }
inline void OH_AVFormat_Destroy(OH_AVFormat* f) { delete f; }
inline void OH_AVFormat_SetIntValue(OH_AVFormat* f, int k, int v) {
  if (k == OH_MD_KEY_WIDTH) f->w = v;
  if (k == OH_MD_KEY_HEIGHT) f->h = v;
}
inline auto OH_VideoDecoder_CreateByMime(const char*) { return new OH_AVCodec{}; }
inline int OH_VideoDecoder_RegisterCallback(OH_AVCodec* d, OH_AVCodecCallback cb, void* u) {
  d->cb = cb; d->user = u; return AV_ERR_OK;
}
inline int OH_VideoDecoder_Configure(OH_AVCodec* d, OH_AVFormat* f) { d->fmt = *f; return AV_ERR_OK; }
void BeforeDecodedOutput(uint8_t level);
inline int OH_VideoDecoder_Start(OH_AVCodec* d) {
  OH_AVBuffer b{std::vector<uint8_t>(128), {}};
  d->cb.onNeedInputBuffer(d, 0, &b, d->user); // parameters
  d->cb.onNeedInputBuffer(d, 1, &b, d->user); // IDR
  assert(b.attr.size > 0);
  const auto level = b.bytes.at(b.attr.size - 1);
  BeforeDecodedOutput(level);
  b.attr.size = d->fmt.w * d->fmt.h * 3 / 2;
  b.bytes.assign(b.attr.size, level);
  d->cb.onNewOutputBuffer(d, 0, &b, d->user);
  return AV_ERR_OK;
}
inline void OH_VideoDecoder_PushInputBuffer(OH_AVCodec*, uint32_t) {}
inline void OH_VideoDecoder_FreeOutputBuffer(OH_AVCodec*, uint32_t) {}
inline void OH_VideoDecoder_Stop(OH_AVCodec*) {}
inline void OH_VideoDecoder_Destroy(OH_AVCodec* d) { delete d; }
'''


def main():
    with tempfile.TemporaryDirectory(prefix='media-reset-') as tmp:
        build = Path(tmp)
        (build / 'hilog').mkdir()
        (build / 'hilog/log.h').write_text('#define OH_LOG_Print(...) ((void)0)\n')
        sdk = build / 'multimedia/player_framework'
        sdk.mkdir(parents=True)
        (sdk / 'fake.h').write_text(SDK)
        for header in ('avbuffer', 'avcodec_base', 'avcodec_videodecoder', 'avformat', 'avmemory'):
            (sdk / f'native_{header}.h').write_text('#include "fake.h"\n')
        exe = build / 'media_reset'
        subprocess.run([os.getenv('CXX', 'c++'), '-std=c++17', '-pthread', '-I', str(build), '-I', str(CPP),
                        str(TESTS / 'media_cache_reset_host.cpp'),
                        *(str(CPP / (name + '.cpp')) for name in ('rtsp_server', 'snapshot', 'web_stream')),
                        '-Wl,--wrap=bind,--wrap=accept', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], cwd=build, timeout=45, check=True)


if __name__ == '__main__':
    main()
