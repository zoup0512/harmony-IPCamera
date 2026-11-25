#ifndef IPCAMERA_M4A_VOICE_H
#define IPCAMERA_M4A_VOICE_H

#include <cstddef>
#include <deque>
#include "codec_records.h"

namespace ipcam {

// Limits on a complete talk-back message, not on the live ADTS stream.
constexpr size_t kVoiceUploadBytes = 8u << 20;
constexpr size_t kVoiceClipBytes = 1u << 20;
constexpr int kVoiceClipSeconds = 30;

struct VoiceClip {
  AdtsInfo config;
  std::deque<std::vector<uint8_t>> frames;  // one complete raw AAC access unit each
  size_t bytes = 0;
};

enum class VoiceClipResult { Ok, Invalid, Unavailable };
// Recognize Android's ftyp-first M4A and complete ADTS messages. The caller's
// clip is untouched unless the entire message (including EOS) validates.
VoiceClipResult ParseVoiceUpload(const uint8_t* data, size_t size, VoiceClip& out);

}  // namespace ipcam
#endif
