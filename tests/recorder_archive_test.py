"""MSVC host regressions; Python stdlib only, native Harmony SDK APIs mocked.

Run from any directory: python <repo>/tests/recorder_archive_test.py -v
Requires Windows and MSVC C++17 (a Developer Command Prompt, an installed
Visual Studio, or VCVARS64 pointing to vcvars64.bat). All generated sources,
executables and tiny recording files live in an automatically cleaned temp dir.

The recorder harness includes the actual recorder.cpp and codec_records.h.
Public Start/Feed/Stop exercise the writer thread; rotation tests access private
state without a writer thread to advance time/byte thresholds deterministically.
The archive harness compiles RespondFile verbatim from http_server.cpp, mocking
file/socket I/O, not the streaming loop. No SDK, device, decoding, real MP4,
network, or large file is required or validated by these tests.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


COMMON_CPP = r'''
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expr); } while (false)
'''

SDK_STUB = r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
using OH_AVErrCode = int;
constexpr int AV_ERR_OK = 0, AV_ERR_INVALID_VAL = 1, AV_OUTPUT_FORMAT_MPEG_4 = 0;
constexpr int AVCODEC_BUFFER_FLAGS_NONE = 0, AVCODEC_BUFFER_FLAGS_SYNC_FRAME = 1;
constexpr auto OH_MD_KEY_CODEC_MIME = "mime", OH_MD_KEY_CODEC_CONFIG = "config",
    OH_MD_KEY_WIDTH = "width", OH_MD_KEY_HEIGHT = "height",
    OH_MD_KEY_AUD_SAMPLE_RATE = "rate", OH_MD_KEY_AUD_CHANNEL_COUNT = "channels";
struct OH_AVCodecBufferAttr { int64_t pts = 0; int32_t size = 0, offset = 0; uint32_t flags = 0; };
struct OH_AVBuffer { std::vector<uint8_t> data; OH_AVCodecBufferAttr attr; };
struct OH_AVFormat {
  std::string mime;
  std::vector<uint8_t> config;
  int width = 0, height = 0, rate = 0, channels = 0;
};
struct OH_AVMuxer { int id, fd; };
namespace sdk_mock {
struct Track { int id; OH_AVFormat format; };
struct Muxer {
  std::vector<Track> tracks;
  bool started = false, stopped = false, destroyed = false;
};
struct Sample { int muxer, track; OH_AVCodecBufferAttr attr; std::vector<uint8_t> data; };
// Only the writer touches these until Stop joins it; direct tests have no thread.
inline std::vector<Muxer> muxers;
inline std::vector<Sample> samples;
inline int liveBuffers = 0, liveFormats = 0;
inline std::string fail;
inline Muxer& State(OH_AVMuxer* m) { return muxers.at(m->id - 1); }
}
inline OH_AVFormat* OH_AVFormat_Create() { ++sdk_mock::liveFormats; return new OH_AVFormat; }
inline void OH_AVFormat_Destroy(OH_AVFormat* f) { --sdk_mock::liveFormats; delete f; }
inline bool OH_AVFormat_SetStringValue(OH_AVFormat* f, const char*, const char* v) {
  f->mime = v; return true;
}
inline bool OH_AVFormat_SetBuffer(OH_AVFormat* f, const char*, const uint8_t* v, size_t n) {
  f->config.assign(v, v + n); return true;
}
inline bool OH_AVFormat_SetIntValue(OH_AVFormat* f, const char* key, int v) {
  const std::string k(key);
  if (k == "width") f->width = v;
  else if (k == "height") f->height = v;
  else if (k == "rate") f->rate = v;
  else if (k == "channels") f->channels = v;
  else return false;
  return true;
}
inline OH_AVBuffer* OH_AVBuffer_Create(int n) {
  ++sdk_mock::liveBuffers;
  return new OH_AVBuffer{std::vector<uint8_t>(n), {}};
}
inline uint8_t* OH_AVBuffer_GetAddr(OH_AVBuffer* b) { return b->data.data(); }
inline int OH_AVBuffer_SetBufferAttr(OH_AVBuffer* b, const OH_AVCodecBufferAttr* a) {
  b->attr = *a; return AV_ERR_OK;
}
inline void OH_AVBuffer_Destroy(OH_AVBuffer* b) { --sdk_mock::liveBuffers; delete b; }
inline OH_AVMuxer* OH_AVMuxer_Create(int fd, int) {
  if (sdk_mock::fail == "create") return nullptr;
  sdk_mock::muxers.emplace_back();
  return new OH_AVMuxer{static_cast<int>(sdk_mock::muxers.size()), fd};
}
inline int OH_AVMuxer_SetRotation(OH_AVMuxer*, int) { return AV_ERR_OK; }
inline int OH_AVMuxer_AddTrack(OH_AVMuxer* m, int32_t* t, OH_AVFormat* f) {
  auto& state = sdk_mock::State(m);
  // Deliberately different IDs per segment: stale track reuse must fail.
  *t = m->id * 10 + static_cast<int>(state.tracks.size());
  state.tracks.push_back({*t, *f}); return AV_ERR_OK;
}
inline int OH_AVMuxer_Start(OH_AVMuxer* m) { sdk_mock::State(m).started = true; return AV_ERR_OK; }
inline int OH_AVMuxer_Stop(OH_AVMuxer* m) {
  sdk_mock::State(m).stopped = true;
  // Simulate a nonempty trailer even for a muxer with zero accepted samples.
  if (sdk_mock::fail != "empty") CHECK(_write(m->fd, "T", 1) == 1);
  return sdk_mock::fail == "stop" ? AV_ERR_INVALID_VAL : AV_ERR_OK;
}
inline int OH_AVMuxer_Destroy(OH_AVMuxer* m) {
  sdk_mock::State(m).destroyed = true; delete m;
  return sdk_mock::fail == "destroy" ? AV_ERR_INVALID_VAL : AV_ERR_OK;
}
inline int OH_AVMuxer_WriteSampleBuffer(OH_AVMuxer* m, uint32_t t, OH_AVBuffer* b) {
  const auto& s = sdk_mock::State(m);
  if (!s.started || s.stopped || s.destroyed ||
      std::none_of(s.tracks.begin(), s.tracks.end(),
                   [t](const auto& track) { return track.id == static_cast<int>(t); })) {
    return AV_ERR_INVALID_VAL;
  }
  if (sdk_mock::fail == "write" && !sdk_mock::samples.empty()) return AV_ERR_INVALID_VAL;
  if (sdk_mock::fail != "empty") CHECK(_write(m->fd, b->data.data(), b->attr.size) == b->attr.size);
  sdk_mock::samples.push_back({m->id, static_cast<int>(t), b->attr, b->data});
  return AV_ERR_OK;
}
'''

RECORDER_CPP = r'''
// Preload standard headers before exposing recorder state to the host harness.
#define private public
#include "recorder.h"
#undef private
#include <io.h>
#include "multimedia/player_framework/stub.h"
static int CloseRecorderFd(int fd) {
  const auto result = _close(fd);
  return sdk_mock::fail == "close" ? -1 : result;
}
static int StatRecorderFd(int fd, struct stat* st) {
  const int result = fstat(fd, st);
  if (result == 0) { st->st_dev = 1; st->st_ino = 7; } // Windows CRT has no POSIX inode identity
  return sdk_mock::fail == "stat" ? -1 : result;
}
static int StatNamedFile(const char* path, struct stat* st) {
  const auto result = stat(path, st);
  if (result == 0) { st->st_dev = 1; st->st_ino = sdk_mock::fail == "replaced" ? 8 : 7; }
  return result;
}
#define close CloseRecorderFd
#define lstat StatNamedFile
#define fstat StatRecorderFd
#include "recorder.cpp"
#undef close
#undef fstat
using R = ipcam::StreamRecorder;
using Bytes = std::vector<uint8_t>;
const Bytes kAudio = {0xff, 0xf1, 0x50, 0x80, 0x01, 0x5f, 0xfc, 0x11, 0x22, 0x33};
const Bytes kAudioPayload = {0x11, 0x22, 0x33};
Bytes AnnexB(const Bytes& nal) {
  Bytes result = {0, 0, 0, 1};
  result.insert(result.end(), nal.begin(), nal.end());
  return result;
}
Bytes Mp4Sample(const Bytes& nal) {
  CHECK(nal.size() < 256);
  Bytes result = {0, 0, 0, static_cast<uint8_t>(nal.size())};
  result.insert(result.end(), nal.begin(), nal.end());
  return result;
}
R::VideoParams Params(bool h265) {
  R::VideoParams p;
  p.ready = true; p.h265 = h265; p.width = 1280; p.height = 720;
  p.vps = h265 ? Bytes{0x40, 1, 0x0c, 1} : Bytes{};
  p.sps = h265 ? Bytes{0x42, 1, 1, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x78}
               : Bytes{0x67, 0x64, 0, 0x1f, 0x12};
  p.pps = h265 ? Bytes{0x44, 1, 0xc1} : Bytes{0x68, 0x11};
  return p;
}
void CheckSample(size_t index, int muxer, bool audio, int64_t pts,
                 bool key, const Bytes& payload) {
  const auto& s = sdk_mock::samples.at(index);
  const auto& tracks = sdk_mock::muxers.at(muxer - 1).tracks;
  CHECK(tracks.size() == 2);
  CHECK(s.muxer == muxer);
  CHECK(s.track == tracks.at(audio ? 1 : 0).id);
  CHECK(s.attr.pts == pts);
  CHECK(s.attr.flags == (key ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE));
  CHECK(s.attr.offset == 0);
  CHECK(s.attr.size == static_cast<int32_t>(payload.size()));
  CHECK(s.data == payload);
}
void CheckTracks(int muxer, bool h265) {
  const auto& tracks = sdk_mock::muxers.at(muxer - 1).tracks;
  CHECK(tracks.size() == 2);
  CHECK(tracks[0].format.mime == (h265 ? "video/hevc" : "video/avc"));
  CHECK(!tracks[0].format.config.empty());
  CHECK(tracks[0].format.width == 1280 && tracks[0].format.height == 720);
  CHECK(tracks[1].format.mime == "audio/mp4a-latm");
  CHECK(tracks[1].format.config == (Bytes{0x12, 0x10}));
  CHECK(tracks[1].format.rate == 44100 && tracks[1].format.channels == 2);
}
void CheckCleanup() {
  CHECK(sdk_mock::liveBuffers == 0 && sdk_mock::liveFormats == 0);
  for (const auto& m : sdk_mock::muxers) CHECK(m.started && m.stopped && m.destroyed);
}
void Startup(int keyType, bool cancel = false) {
  const bool h265 = keyType != 5;
  const auto vp = Params(h265);
  const Bytes p = h265 ? Bytes{0x02, 1, 0x9a} : Bytes{0x41, 0x9a, 0x22};
  const Bytes key = h265 ? Bytes{static_cast<uint8_t>(keyType << 1), 1, 0x88}
                         : Bytes{0x65, 0x88, 0x84};
  std::vector<std::string> errors;
  R r;
  r.SetVideoParams(h265, vp.vps, vp.sps, vp.pps, vp.width, vp.height);
  r.SetOptions({false, 0});
  CHECK(r.Start("startup.mp4", 0, [&](const auto& error) { errors.push_back(error); }));
  CHECK(lockedWriterFd >= 0 && lockedWriterFd == r.outputFd_); // Start uses the locked exclusive creator.
  auto video = [&](const Bytes& nal, int64_t ts) {
    const auto data = AnnexB(nal); r.FeedVideo(data.data(), data.size(), ts);
  };
  video(p, 1000000);
  r.FeedAudio(kAudio.data(), kAudio.size(), 1050000);
  video(vp.sps, 1100000);  // Codec headers are not picture samples.
  if (!cancel) {
    video(key, 2000000);  // CRA/BLA tests never send an IDR.
    if (keyType == 21 || keyType == 16) {
      video(vp.sps, 2005000);  // Non-VCL headers must not end leading-picture filtering.
      int64_t ts = 2010000;
      for (const uint8_t type : {8, 9, 6, 7}) {
        // Advancing timestamps deliberately prevent PTS rejection from masking
        // a missing RASL/RADL filter (or startup sorting from moving them earlier).
        video({static_cast<uint8_t>(type << 1), 1, 0x55}, ts);
        ts += 5000;
      }
    }
    video(p, 2040000);  // HEVC TRAIL_R (type 1) must be accepted after leading pictures.
    r.FeedAudio(kAudio.data(), kAudio.size(), 2050000);
  }
  r.Stop();  // Joins/drains the writer; no sleeps or concurrent state inspection.
  if (!errors.empty()) throw std::runtime_error(errors.front());
  CHECK(!r.IsRecording() && !r.writerThread_.joinable() && r.outputFd_ == -1);
  CHECK(sdk_mock::muxers.size() == 1);
  CheckTracks(1, h265);
  CHECK(sdk_mock::samples.size() == (cancel ? 0 : 3));
  if (!cancel) {
    CheckSample(0, 1, false, 0, true, Mp4Sample(key));
    CheckSample(1, 1, false, 40000, false, Mp4Sample(p));
    CheckSample(2, 1, true, 50000, false, kAudioPayload);
  }
  CheckCleanup();
}
void Rotation(bool sizeLimit, bool h265 = false) {
  R r;
  const auto vp = Params(h265);
  const Bytes key = h265 ? Bytes{0x2a, 1, 0x88} : Bytes{0x65, 0x88, 0x84};
  const Bytes nextKey = h265 ? Bytes{0x2a, 1, 0x99} : Bytes{0x65, 0x99, 0x85};
  const Bytes p = h265 ? Bytes{0x02, 1, 0x9a} : Bytes{0x41, 0x9a, 0x22};
  r.activeOptions_ = {sizeLimit, sizeLimit ? 0 : 10};  // direct writer test (no Start)
  std::vector<R::CompletedFile> closed;
  r.onCompleted_ = [&](const R::CompletedFile& file) { closed.push_back(file); };
  r.filePath_ = sizeLimit ? "size.mp4" : "time.mp4";
  r.segmentIndex_ = 1;
  r.outputFd_ = open(r.filePath_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
  CHECK(r.outputFd_ >= 0);
  r.tsBaseSet_ = true; r.tsBaseUs_ = 1000000; r.pendingBaseReset_ = true;
  CHECK(r.CreateMuxer(vp, true, R::AudioCfg{}));
  auto write = [&](const Bytes& data, bool audio, int64_t ts) {
    R::Item item; item.data = data; item.audio = audio; item.tsUs = ts;
    CHECK(r.WriteItem(item, h265));
  };
  write(AnnexB(key), false, 1000000);
  CHECK(!r.ShouldRotate());
  if (sizeLimit) {
    // The recorder's 4 GiB safety threshold is 3968 MiB; no large file is made.
    r.segmentBytes_ = 3968ULL * 1024 * 1024 - 1;
    CHECK(!r.ShouldRotate());
  } else {
    r.segmentStart_ = std::chrono::steady_clock::now() - std::chrono::minutes(11);
    CHECK(r.ShouldRotate());
  }
  write(AnnexB(p), false, 1050000);  // Cross the byte limit with a P frame.
  CHECK(r.ShouldRotate());
  if (h265) CHECK(!r.discardLeadingPictures_);
  write(kAudio, true, 1060000);
  write(AnnexB(vp.sps), false, 1500000);
  CHECK(sdk_mock::muxers.size() == 1 && sdk_mock::samples.size() == 3);
  CheckSample(0, 1, false, 0, true, Mp4Sample(key));
  CheckSample(1, 1, false, 50000, false, Mp4Sample(p));
  CheckSample(2, 1, true, 60000, false, kAudioPayload);

  write(AnnexB(nextKey), false, 2000000);  // HEVC rollover uses CRA21, never IDR.
  CHECK(sdk_mock::muxers.size() == 2 && sdk_mock::samples.size() == 4);
  CHECK(sdk_mock::muxers[0].stopped && sdk_mock::muxers[0].destroyed);
  CheckSample(3, 2, false, 0, true, Mp4Sample(nextKey));
  CHECK(lockedWriterFd >= 0 && lockedWriterFd == r.outputFd_); // Rotation must use it too.
  CHECK(r.segmentIndex_ == 2 && !r.pendingBaseReset_ && !r.ShouldRotate());
  CHECK(r.segmentBytes_ == Mp4Sample(nextKey).size());
  if (h265) {
    CHECK(r.discardLeadingPictures_);  // Filtering must re-arm for the new segment.
    write(AnnexB(vp.sps), false, 2005000);
    CHECK(r.discardLeadingPictures_ && sdk_mock::samples.size() == 4);
    int64_t ts = 2010000;
    for (const uint8_t type : {8, 9, 6, 7}) {
      // Positive, advancing PTS isolates NAL-type filtering from timestamp drops.
      write(AnnexB({static_cast<uint8_t>(type << 1), 1, 0x55}), false, ts);
      CHECK(r.discardLeadingPictures_ && sdk_mock::samples.size() == 4);
      CHECK(r.segmentBytes_ == Mp4Sample(nextKey).size() && r.lastVideoPtsUs_ == 0);
      ts += 5000;
    }
  }
  write(AnnexB(p), false, 2040000);  // HEVC trailing VCL type 1 is accepted.
  if (h265) CHECK(!r.discardLeadingPictures_);
  write(kAudio, true, 2050000);
  CHECK(sdk_mock::samples.size() == 6);
  CheckSample(4, 2, false, 40000, false, Mp4Sample(p));
  CheckSample(5, 2, true, 50000, false, kAudioPayload);
  CheckTracks(1, h265); CheckTracks(2, h265);
  CHECK(sdk_mock::muxers[0].tracks[0].format.config ==
        sdk_mock::muxers[1].tracks[0].format.config);
  CHECK(closed.size() == 1 && closed[0].path == r.filePath_ && closed[0].sizeBytes > 0);
  CHECK(closed[0].segmentIndex == 1 && closed[0].reason == R::CloseReason::Rotation);
  r.Stop(); r.Stop();
  CHECK(closed.size() == 2 && closed[1].path == R::SegmentPath(r.filePath_, 2));
  CHECK(closed[1].segmentIndex == 2 && closed[1].reason == R::CloseReason::Stop);
  CheckCleanup();
}
void FailedRotation(bool collision) {
  R r;
  r.filePath_ = "rotation.mp4"; r.segmentIndex_ = 1;
  r.outputFd_ = open(r.filePath_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
  CHECK(r.outputFd_ >= 0);
  r.tsBaseSet_ = true; r.pendingBaseReset_ = true;
  CHECK(r.CreateMuxer(Params(false), false, R::AudioCfg{}));
  std::vector<std::string> events;
  r.onError_ = [&](const auto&){ events.push_back("error"); };
  r.onCompleted_ = [&](const R::CompletedFile& f){ events.push_back(f.path); };
  const R::Item key{false, AnnexB({0x65, 0x88, 0x84}), 100};
  CHECK(r.WriteItem(key, false));
  if (collision) {
    const int other = open("rotation_2.mp4", O_CREAT | O_RDWR | O_EXCL, 0600);
    CHECK(other >= 0 && _write(other, "keep", 4) == 4 && _close(other) == 0);
  } else sdk_mock::fail = "stop";
  r.segmentStart_ -= std::chrono::hours(1);
  CHECK(!r.WriteItem(key, false));
  r.Stop();
  CHECK(sdk_mock::muxers.size() == 1);
  struct stat st{};
  if (collision) {
    CHECK((events == std::vector<std::string>{"rotation.mp4", "error"}));
    CHECK(stat("rotation_2.mp4", &st) == 0 && st.st_size == 4);
  } else {
    CHECK((events == std::vector<std::string>{"error"}));
    CHECK(stat("rotation_2.mp4", &st) != 0);
  }
}
void Completion(const std::string& mode) {
  R r;
  const auto vp = Params(false);
  r.SetVideoParams(false, {}, vp.sps, vp.pps, vp.width, vp.height);
  r.SetOptions({false, 0, false});
  sdk_mock::fail = mode;
  const std::string path = "completed.mp4";
  std::vector<R::CompletedFile> closed;
  std::vector<std::string> archived, errors;
  bool finalizedWhenCalled = false;
  r.SetArchiveSink([&](const auto& p) { archived.push_back(p); });
  const auto error = [&](const auto& e){ errors.push_back(e); };
  R::CompletedSink sink = [&](const R::CompletedFile& f){
    closed.push_back(f);
    finalizedWhenCalled = !r.IsWritingPath(f.path) && r.outputFd_ == -1 &&
                          sdk_mock::muxers.back().destroyed;
    throw std::runtime_error("subscriber failure must not fail the recording");
  };
  if (mode == "invalid_path") {
    CHECK(!r.Start(path + std::string(1, '\0') + ".other", 0, error, sink));
    struct stat st{};
    CHECK(stat(path.c_str(), &st) != 0 && errors.size() == 1 && closed.empty() && archived.empty());
    return;
  }
  CHECK(r.Start(path, 0, error, mode == "archive" ? R::CompletedSink{} : sink));
  r.SetOptions({true, 1, true}); // current session cannot acquire an audio track mid-run
  r.SetArchiveSink({});
  r.FeedAudio(kAudio.data(), kAudio.size(), 0);
  const Bytes key = AnnexB({0x65, 0x88, 0x84}), p = AnnexB({0x41, 0x88, 0x84});
  if (mode != "no_key") r.FeedVideo(key.data(), key.size(), 1);
  r.FeedVideo(p.data(), p.size(), 2);
  r.Stop(); r.Stop();
  const bool ok = mode == "ok" || mode == "archive";
  CHECK(errors.size() == (ok || mode == "no_key" ? 0 : 1));
  CHECK(closed.size() == (mode == "ok" ? 1 : 0));
  CHECK(archived.size() == (mode == "archive" ? 1 : 0));
  if (mode == "archive") CHECK(archived[0] == path);
  if (ok) {
    CHECK(sdk_mock::muxers.size() == 1 && sdk_mock::muxers.front().tracks.size() == 1);
    CHECK(sdk_mock::samples.size() == 2);
    if (mode == "ok") {
      CHECK(finalizedWhenCalled && closed[0].reason == R::CloseReason::Stop && closed[0].segmentIndex == 1);
      CHECK(closed[0].path == path && closed[0].sizeBytes == 15); // 2 x 7 payload bytes + trailer
    }
    CHECK(!r.Start(path, 0, error));  // never truncate an earlier event's file
    struct stat st{}; CHECK(stat(path.c_str(), &st) == 0 && st.st_size == 15);
  }
}
int main(int argc, char** argv) {
  try {
    CHECK(argc == 2);
    const std::string name(argv[1]);
    if (name.rfind("completion_", 0) == 0) Completion(name.substr(11));
    else if (name == "rotation_collision" || name == "rotation_stop_error") FailedRotation(name == "rotation_collision");
    else if (name == "startup_h264") Startup(5);
    else if (name == "startup_h265_19") Startup(19);
    else if (name == "startup_h265_20") Startup(20);
    else if (name == "startup_h265_cra21") Startup(21);
    else if (name == "startup_h265_bla16") Startup(16);
    else if (name == "cancel_before_idr") Startup(5, true);
    else if (name == "timed_rotation") Rotation(false);
    else if (name == "size_rotation") Rotation(true);
    else if (name == "cra_timed_rotation") Rotation(false, true);
    else if (name == "cra_size_rotation") Rotation(true, true);
    else throw std::runtime_error("unknown recorder test: " + name);
    std::cout << "PASS " << name << " (native SDK mocked)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL (native SDK mocked): " << error.what() << '\n';
    return 1;
  }
}
'''

ARCHIVE_PREFIX = r'''
using ssize_t = std::ptrdiff_t;
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
constexpr size_t kChunk = 65536;
constexpr int kFileFd = 71, kSocketFd = 19;
struct ArchiveMock {
  int64_t size = 2 * kChunk + 137;
  uint64_t position = 0, sent = 0, eofAfter = UINT64_MAX;
  size_t reads = 0, bodies = 0, headers = 0, closes = 0, stats = 0;
  size_t maxRead = 0, maxSend = 0, lastRead = 0, pending = 0;
  size_t shortLimit = kChunk, failRead = 0, failBody = 0, stopAfterBody = 0;
  std::vector<size_t> interruptCalls;
  size_t interrupts = 0;
  uint8_t pattern = 0;
  bool failStat = false, regular = true, failHeader = false, closed = false;
  int responseCode = 0;
  std::string header;
  std::atomic<bool>* running = nullptr;
} io;
int MockClose(int fd) {
  CHECK(fd == kFileFd && !io.closed);  // Never close the connection descriptor.
  ++io.closes; io.closed = true; return 0;
}
int MockFstat(int fd, struct stat* st) {
  CHECK(fd == kFileFd && !io.closed);
  ++io.stats;
  if (io.failStat) { errno = EIO; return -1; }
  st->st_size = static_cast<decltype(st->st_size)>(io.size);
  st->st_mode = io.regular ? _S_IFREG : _S_IFDIR;
  return 0;
}
ssize_t MockRead(int fd, void* buffer, size_t count) {
  CHECK(fd == kFileFd && !io.closed);
  // Each chunk must be sent before another read: no whole-file accumulation.
  CHECK(io.pending == 0 && io.sent == io.position);
  CHECK(count > 0 && count <= kChunk);
  CHECK(count == std::min<uint64_t>(kChunk, io.size - io.position));
  ++io.reads; io.maxRead = std::max(io.maxRead, count);
  if (std::find(io.interruptCalls.begin(), io.interruptCalls.end(), io.reads) !=
      io.interruptCalls.end()) {
    ++io.interrupts; errno = EINTR; return -1;
  }
  if (io.reads == io.failRead) { errno = EIO; return -1; }
  if (io.position >= io.eofAfter) return 0;
  const size_t n = static_cast<size_t>(std::min<uint64_t>(
      std::min(count, io.shortLimit), io.eofAfter - io.position));
  io.pattern = static_cast<uint8_t>(io.reads % 251);
  std::memset(buffer, io.pattern, n);  // Only the caller's <=64 KiB buffer exists.
  io.position += n; io.pending = n; io.lastRead = n;
  return static_cast<ssize_t>(n);
}
bool SendAll(int fd, const std::string& header) {
  CHECK(fd == kSocketFd && !io.closed && io.headers == 0);
  ++io.headers; io.header = header; return !io.failHeader;
}
bool SendAll(int fd, const uint8_t* data, size_t size) {
  CHECK(fd == kSocketFd && !io.closed);
  CHECK(size > 0 && size <= kChunk && size == io.pending);
  CHECK(std::all_of(data, data + size, [](uint8_t b) { return b == io.pattern; }));
  ++io.bodies; io.maxSend = std::max(io.maxSend, size);
  if (io.bodies == io.failBody) return false;
  io.sent += size; io.pending = 0;
  if (io.bodies == io.stopAfterBody) io.running->store(false);
  return true;
}
class HttpServer {
 public:
  std::atomic<bool> running_{true};
  bool RespondFile(int fd, int fileFd);
  void Respond(int fd, int code, const std::string& type, const std::vector<uint8_t>& body) {
    CHECK(fd == kSocketFd && type == "text/plain" && body.empty());
    io.responseCode = code;
  }
};
// Redirect only the syscall names; the extracted production definition is unchanged.
#define close MockClose
#define fstat MockFstat
#define read MockRead
'''

ARCHIVE_SUFFIX = r'''
#undef close
#undef fstat
#undef read
void ArchiveCase(const std::string& name) {
  HttpServer server;
  io.running = &server.running_;
  bool expected = false, invalid = false;
  if (name == "large_archive") {
    io.size = 314LL * 1024 * 1024 + 123; expected = true;
  } else if (name == "eintr_short_reads") {
    io.interruptCalls = {1, 4, 9}; io.shortLimit = 7001; expected = true;
  } else if (name == "early_eof") io.eofAfter = kChunk + 9;
  else if (name == "read_error") io.failRead = 2;
  else if (name == "header_send_failure") io.failHeader = true;
  else if (name == "body_send_failure") io.failBody = 2;
  else if (name == "stopped_before_read") server.running_.store(false);
  else if (name == "stopped_during_send") io.stopAfterBody = 1;
  else if (name == "stat_failure") { io.failStat = true; invalid = true; }
  else if (name == "not_regular") { io.regular = false; invalid = true; }
  else if (name == "negative_size") { io.size = -1; invalid = true; }
  else if (name == "empty_file") { io.size = 0; expected = true; }
  else throw std::runtime_error("unknown archive test: " + name);

  CHECK(server.RespondFile(kSocketFd, kFileFd) == expected);
  CHECK(io.stats == 1 && io.closes == 1 && io.closed);
  CHECK(io.maxRead <= kChunk && io.maxSend <= kChunk);
  if (invalid) {
    CHECK(io.responseCode == 404 && io.headers == 0 && io.reads == 0 && io.bodies == 0);
    return;
  }
  CHECK(io.responseCode == 0 && io.headers == 1);
  CHECK(io.header == "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: " +
        std::to_string(io.size) + "\r\nConnection: close\r\n\r\n");
  if (expected) {
    CHECK(io.sent == static_cast<uint64_t>(io.size) && io.position == io.sent && io.pending == 0);
  }
  if (name == "large_archive") {
    CHECK(io.reads == (static_cast<uint64_t>(io.size) + kChunk - 1) / kChunk);
    CHECK(io.bodies == io.reads && io.maxRead == kChunk && io.maxSend == kChunk);
    CHECK(io.lastRead == 123);
  } else if (name == "eintr_short_reads") {
    CHECK(io.interrupts == 3 && io.reads == io.bodies + 3);
    CHECK(io.bodies == (static_cast<uint64_t>(io.size) + 7000) / 7001);
    CHECK(io.maxSend == 7001);
  } else if (name == "early_eof") {
    CHECK(io.reads == 3 && io.bodies == 2 && io.sent == kChunk + 9);
  } else if (name == "read_error") {
    CHECK(io.reads == 2 && io.bodies == 1 && io.sent == kChunk);
  } else if (name == "body_send_failure") {
    CHECK(io.reads == 2 && io.bodies == 2 && io.sent == kChunk && io.position == 2 * kChunk);
  } else if (name == "stopped_during_send") {
    CHECK(io.reads == 1 && io.bodies == 1 && io.sent == kChunk);
  } else {
    CHECK(io.reads == 0 && io.bodies == 0 && io.sent == 0);
  }
  std::cout << "bytes=" << io.sent << " reads=" << io.reads
            << " max_read=" << io.maxRead << " closes=" << io.closes << '\n';
}
int main(int argc, char** argv) {
  try {
    CHECK(argc == 2);
    ArchiveCase(argv[1]);
    std::cout << "PASS " << argv[1] << " (SDK mocked; file/socket I/O mocked)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL (SDK mocked; file/socket I/O mocked): " << error.what() << '\n';
    return 1;
  }
}
'''


def find_vcvars():
    """Use the existing developer environment or the usual VS bootstrap."""
    if os.environ.get("VCVARS64"):
        path = Path(os.environ["VCVARS64"])
        if not path.is_file():
            raise RuntimeError("VCVARS64 does not name an existing vcvars64.bat")
        return path
    if shutil.which("cl.exe"):
        return None
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / (
        "Microsoft Visual Studio/Installer/vswhere.exe"
    )
    if vswhere.is_file():
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
            capture_output=True, text=True, errors="replace", timeout=30, check=True,
        )
        if result.stdout.strip():
            path = Path(result.stdout.strip()) / "VC/Auxiliary/Build/vcvars64.bat"
            if path.is_file():
                return path
    for root in ("ProgramFiles", "ProgramFiles(x86)"):
        base = Path(os.environ.get(root, "C:/Program Files")) / "Microsoft Visual Studio"
        for path in sorted(base.glob("*/*/VC/Auxiliary/Build/vcvars64.bat"), reverse=True):
            if path.is_file():
                return path
    raise RuntimeError("MSVC C++17 is required; install VS C++ tools or set VCVARS64")


def extract_respond_file(source):
    """Keep the exact definition, failing loudly if its signature/layout changes."""
    matches = list(re.finditer(
        r"^bool HttpServer::RespondFile\(int fd, int fileFd\) \{\n.*?^\}",
        source, flags=re.MULTILINE | re.DOTALL,
    ))
    if len(matches) != 1:
        raise RuntimeError("Expected exactly one opened-fd HttpServer::RespondFile definition")
    match = matches[0]
    return match.group(), source[:match.start()].count("\n") + 1


class RecorderArchiveHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name != "nt":
            raise unittest.SkipTest("Windows/MSVC host required (native SDK mocked)")
        vcvars = find_vcvars()
        cls.temp = tempfile.TemporaryDirectory(prefix="ipcamera-recorder-archive-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        native = Path(__file__).resolve().parents[1] / "entry/src/main/cpp"
        includes = cls.work / "include"
        media = includes / "multimedia/player_framework"
        media.mkdir(parents=True)
        (includes / "hilog").mkdir()
        (includes / "hilog/log.h").write_text(
            "#pragma once\n#define LOG_APP 0\n#define LOG_INFO 0\n#define LOG_WARN 0\n"
            "#define LOG_ERROR 0\n#define OH_LOG_Print(...) ((void)0)\n", encoding="utf-8",
        )
        (includes / "unistd.h").write_text(r'''
#pragma once
#include <io.h>
#include <chrono>
#include <ctime>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
inline int clock_gettime(int, timespec* out) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  out->tv_sec = ns / 1000000000; out->tv_nsec = static_cast<long>(ns % 1000000000);
  return 0;
}
#endif
''', encoding="utf-8")
        # MSVC recorder tests mock BSD flock; cloud_archive_pin_test.cpp exercises real fd locks.
        (includes / 'sys').mkdir()
        (includes / 'sys/file.h').write_text('#pragma once\n#define LOCK_EX 2\n#define LOCK_NB 4\ninline int lockedWriterFd=-1; inline int flock(int fd,int op){lockedWriterFd=fd; return op==6?0:-1;}\n', encoding='utf-8')
        (media / "stub.h").write_text(SDK_STUB, encoding="utf-8")
        for name in ("native_avbuffer.h", "native_avcodec_base.h", "native_avformat.h",
                     "native_avmuxer.h", "native_avmemory.h"):
            (media / name).write_text('#pragma once\n#include "stub.h"\n', encoding="utf-8")
        (cls.work / "recorder_test.cpp").write_text(COMMON_CPP + RECORDER_CPP, encoding="utf-8")
        http = native / "http_server.cpp"
        definition, line = extract_respond_file(http.read_text(encoding="utf-8"))
        (cls.work / "archive_test.cpp").write_text(
            COMMON_CPP + ARCHIVE_PREFIX + f'\n#line {line} "{http.as_posix()}"\n' +
            definition + '\n#line 1 "archive_assertions.cpp"\n' + ARCHIVE_SUFFIX, encoding="utf-8",
        )
        commands = ["@echo off"]
        if vcvars:
            commands += [f'call "{vcvars}" >nul', "if errorlevel 1 exit /b %errorlevel%"]
        for name in ("recorder", "archive"):
            command = ["cl.exe", "/nologo", "/std:c++17", "/EHsc", "/O2", "/utf-8",
                       "/D_CRT_SECURE_NO_WARNINGS", "/D_CRT_NONSTDC_NO_DEPRECATE",
                       f"/I{includes}", f"/I{native}", str(cls.work / f"{name}_test.cpp"),
                       f"/Fo:{cls.work / (name + '.obj')}", f"/Fe:{cls.work / (name + '.exe')}"]
            commands += [subprocess.list2cmdline(command), "if errorlevel 1 exit /b %errorlevel%"]
        commands.append("exit /b 0")
        batch = cls.work / "build.cmd"
        batch.write_text("\n".join(commands) + "\n", encoding="utf-8")
        result = subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=cls.work,
                                capture_output=True, text=True, errors="replace", timeout=120)
        if result.returncode:
            raise RuntimeError("MSVC host harness build failed:\n" + result.stdout + result.stderr)

    def run_case(self, harness, name):
        case = self.work / name
        case.mkdir()
        result = subprocess.run([str(self.work / (harness + ".exe")), name], cwd=case,
                                capture_output=True, text=True, errors="replace", timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + name, result.stdout)
        self.assertIn("SDK mocked", result.stdout)

    def test_completed_only_after_successful_close(self):
        for mode in ('ok', 'archive', 'invalid_path', 'no_key', 'create', 'write', 'stop', 'destroy', 'stat', 'replaced', 'empty', 'close'):
            with self.subTest(mode=mode):
                self.run_case('recorder', 'completion_' + mode)

    def test_failed_rotation(self):
        for case in ('rotation_stop_error', 'rotation_collision'):
            with self.subTest(case=case):
                self.run_case('recorder', case)

    def test_recorder_startup_h264(self):
        """SDK mocked: drop startup P/audio; IDR at PTS 0, then normal P/audio."""
        self.run_case("recorder", "startup_h264")

    def test_recorder_timed_rotation(self):
        """SDK mocked: expired time keeps P/audio on old muxer until next IDR."""
        self.run_case("recorder", "timed_rotation")

    def test_recorder_size_rotation(self):
        """SDK mocked: simulated 4 GiB threshold rotates before IDR, with new track IDs."""
        self.run_case("recorder", "size_rotation")

    def test_recorder_cancel_before_idr(self):
        """SDK mocked: Stop before any IDR writes no predictive or audio sample."""
        self.run_case("recorder", "cancel_before_idr")

    def test_recorder_h265_idr19(self):
        """SDK mocked: H.265 IDR type 19 starts video at sync PTS 0."""
        self.run_case("recorder", "startup_h265_19")

    def test_recorder_h265_idr20(self):
        """SDK mocked: H.265 IDR type 20 starts video at sync PTS 0."""
        self.run_case("recorder", "startup_h265_20")

    def test_recorder_h265_cra21_startup(self):
        """SDK mocked: CRA21 without IDR starts at PTS 0; drops RASL/RADL, accepts TRAIL_R."""
        self.run_case("recorder", "startup_h265_cra21")

    def test_recorder_h265_bla16_startup(self):
        """SDK mocked: BLA16 starts at sync PTS 0; drops leading pictures, accepts TRAIL_R."""
        self.run_case("recorder", "startup_h265_bla16")

    def test_recorder_h265_cra_timed_rotation(self):
        """SDK mocked: timed CRA21 rollover resets PTS and drops types 8/9/6/7, not 1."""
        self.run_case("recorder", "cra_timed_rotation")

    def test_recorder_h265_cra_size_rotation(self):
        """SDK mocked: size CRA21 rollover re-arms leading-picture filtering on the new muxer."""
        self.run_case("recorder", "cra_size_rotation")

    def test_archive_large_bounded_stream(self):
        """SDK/I/O mocked: 314 MiB logical archive, exact length/progress, <=64 KiB chunks."""
        self.run_case("archive", "large_archive")

    def test_archive_eintr_short_reads(self):
        """SDK/I/O mocked: retry EINTR and short reads without losing progress."""
        self.run_case("archive", "eintr_short_reads")

    def test_archive_early_eof(self):
        """SDK/I/O mocked: early EOF fails and closes the owned file descriptor."""
        self.run_case("archive", "early_eof")

    def test_archive_read_error(self):
        """SDK/I/O mocked: non-EINTR read error fails without a retry or fd leak."""
        self.run_case("archive", "read_error")

    def test_archive_header_send_failure(self):
        """SDK/I/O mocked: failed header send closes the fd without reading."""
        self.run_case("archive", "header_send_failure")

    def test_archive_body_send_failure(self):
        """SDK/I/O mocked: failed body send stops reading and closes the fd."""
        self.run_case("archive", "body_send_failure")

    def test_archive_stopped_before_read(self):
        """SDK/I/O mocked: running=false prevents reads and still closes the fd."""
        self.run_case("archive", "stopped_before_read")

    def test_archive_stopped_during_send(self):
        """SDK/I/O mocked: shutdown after one chunk stops progress and closes the fd."""
        self.run_case("archive", "stopped_during_send")

    def test_archive_stat_failure(self):
        """SDK/I/O mocked: failed fstat returns 404 and closes the fd."""
        self.run_case("archive", "stat_failure")

    def test_archive_not_regular(self):
        """SDK/I/O mocked: non-regular file returns 404 and closes the fd."""
        self.run_case("archive", "not_regular")

    def test_archive_negative_size(self):
        """SDK/I/O mocked: negative file size returns 404 and closes the fd."""
        self.run_case("archive", "negative_size")

    def test_archive_empty_file(self):
        """SDK/I/O mocked: empty file succeeds with length zero and no reads."""
        self.run_case("archive", "empty_file")


class ArchiveNapiHostTest(unittest.TestCase):
    def test_separate_encoder_start_failure_and_stop(self):
        """Actual holder startup/teardown, including an early worker error before writer.Start."""
        from motion_state_test import definition, compile_native, run
        native = Path(__file__).resolve().parents[1] / 'entry/src/main/cpp'
        code = (native / 'napi_init.cpp').read_text(encoding='utf-8')
        harness = r'''
#include "record_video_encoder.h"
#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>
using Bytes=std::vector<uint8_t>;
namespace ipcam {
enum class RtspEvent { kServerError };
std::string steps;
bool codecOK=true, writerOK=true, failEarly=false;
RecordVideoEncoder::Sinks sinks;
struct RecordVideoEncoder::Impl {};
RecordVideoEncoder::RecordVideoEncoder()=default;
RecordVideoEncoder::~RecordVideoEncoder()=default;
bool RecordVideoEncoder::Configure(const Options&,const Source& p,Sinks s,std::string& e) {
  assert(p.width==640 && p.height==480 && p.sps==Bytes{7});
  steps+='C'; sinks=std::move(s); if(!codecOK) e="unsupported";
  if(failEarly) {std::thread t([]{sinks.fatal("early");});t.join();} return codecOK;
}
void RecordVideoEncoder::Stop(){steps+='E';sinks={};}
struct StreamRecorder {
  struct Options{bool includeAudio=true;}; struct CompletedFile{std::string path;};
  int params=0,video=0; std::function<void(const CompletedFile&)> closed;
  void SetOptions(const Options&){}
  void SetVideoParams(bool,const Bytes&,const Bytes& s,const Bytes&,int,int){params=s.at(0);}
  bool Start(const std::string&,int,std::function<void(const std::string&)>,decltype(closed) cb){
    steps+='W'; if(writerOK)closed=cb; return writerOK;
  }
  void Stop(){steps+='S';if(closed)closed({"local"});closed={};}
  void FeedVideo(const uint8_t*,size_t,int64_t){++video;} void FeedAudio(const uint8_t*,size_t,int64_t){}
};
}
struct Holder;
void PostEvent(Holder*,ipcam::RtspEvent,int,const std::string&){}
struct Holder {
  std::mutex mediaControlMu,cameraMu,outputsMu; struct Camera{int Orientation(){return 90;}}* camera=nullptr;
  struct Server{struct VP{bool ready=true,h265=false;Bytes vps,sps{7},pps{8};} vp;
    VP GetVideoParams(){return vp;} int ClientCount(){return 0;}} server;
  struct Size{int width,height;};std::atomic<Size> encodedSize{Size{640,480}};
  struct Archive{int calls=0;bool Post(const std::string& p){assert(p=="local");++calls;return true;}} archives;
  std::shared_ptr<ipcam::StreamRecorder> recorder;std::shared_ptr<ipcam::RecordVideoEncoder> recordEncoder;
''' + definition(code, 'bool StartRecording(') + '\n' + definition(code, 'void StopRecorderOutput(') + r'''
};
int main(){
  using namespace ipcam; Holder h;RecordVideoEncoder::Options o{true,4000000,2000};
  h.server.vp.ready=false;assert(!h.StartRecording("p",{},&o) && steps.empty());
  h.server.vp.ready=true;codecOK=false;assert(!h.StartRecording("p",{},&o) && steps=="CE" && !h.recorder);
  steps.clear();codecOK=true;writerOK=false;assert(!h.StartRecording("p",{},&o) && steps=="CWES" && !h.recordEncoder);
  steps.clear();writerOK=true;failEarly=true;assert(!h.StartRecording("p",{},&o) && steps=="CSES" && !h.archives.calls);
  steps.clear();failEarly=false;assert(h.StartRecording("p",{false},&o) && !sinks.audio && h.recorder->params==0);
  sinks.params({true,640,480,{1},{9},{2}});sinks.video(nullptr,0,0);assert(h.recorder->params==9 && h.recorder->video==1);
  h.StopRecorderOutput();assert(steps=="CWES" && h.archives.calls==1);
  steps.clear();assert(h.StartRecording("p",{},&o) && sinks.audio);std::thread t([]{sinks.fatal("broken");});t.join();
  h.StopRecorderOutput();assert(steps=="CWSES" && h.archives.calls==1);
  steps.clear();assert(h.StartRecording("p",{},nullptr) && !h.recordEncoder && h.recorder->params==7);
  h.StopRecorderOutput();assert(steps=="WS" && h.archives.calls==2);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            shutil.copyfile(native / 'record_video_encoder.h', work / 'record_video_encoder.h')
            cpp = work / 'lifecycle.cpp'
            cpp.write_text(harness, encoding='utf-8')
            run([str(compile_native(cpp, work))], work, timeout=10)

    def test_real_tsfn_worker_to_js(self):
        """Real host Node-API/TSFN; no SDK, real recorder, network or npm install."""
        from motion_state_test import definition, run
        node = shutil.which('node')
        if os.name != 'nt' or not node:
            self.skipTest('Windows Node/MSVC host required')
        version = subprocess.check_output([node, '-p', 'process.versions.node'], text=True).strip()
        cache = Path(os.environ['LOCALAPPDATA']) / 'node-gyp/Cache' / version
        include, lib = cache / 'include/node', cache / 'x64/node.lib'
        if not (include / 'node_api.h').is_file() or not lib.is_file():
            self.skipTest('matching local x64 Node headers/import library required')
        native = Path(__file__).resolve().parents[1] / 'entry/src/main/cpp/napi_init.cpp'
        source = native.read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            addon = r'''
#include <node_api.h>
#include <atomic>
#include <deque>
#include <utility>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
''' + definition(source, 'struct ArchiveChannel {') + r''';
struct ServerHolder { ArchiveChannel archives; } h;
ServerHolder* HolderFromArg(napi_env, napi_value) { return &h; }
''' + '\n'.join(definition(source, marker) for marker in (
                'napi_value InvalidArgs(', 'napi_value MediaBool(', 'bool MediaString(',
                'napi_value RtspSetArchiveCallback(', 'napi_value RtspTakePendingArchives(', 'napi_value RtspClearArchiveCallback(')) + r'''
napi_value Worker(napi_env env, napi_callback_info info) {
  napi_value arg, undef; size_t n = 1;
  napi_get_cb_info(env, info, &n, &arg, nullptr, nullptr);
  std::string p;
  if (!n || !MediaString(env, arg, p)) return InvalidArgs(env);
  std::thread writer([&] { h.archives.Post(p); }); writer.join();
  napi_get_undefined(env, &undef); return undef;
}
NAPI_MODULE_INIT() {
  napi_property_descriptor props[] = {
    {"set", nullptr, RtspSetArchiveCallback}, {"clear", nullptr, RtspClearArchiveCallback},
    {"take", nullptr, RtspTakePendingArchives}, {"completed", nullptr, Worker}};
  napi_define_properties(env, exports, 4, props); return exports;
}
'''
            (work / 'addon.cpp').write_text(addon, encoding='utf-8')
            setup = find_vcvars()
            build = work / 'build.cmd'
            build.write_text((f'@call "{setup}" >nul\n@if errorlevel 1 exit /b %errorlevel%\n' if setup else '') +
                             f'@cl /nologo /std:c++17 /EHsc /utf-8 /LD /I"{include}" addon.cpp "{lib}" /link /out:bridge.node\n',
                             encoding='utf-8')
            run(['cmd.exe', '/d', '/c', str(build)], work, timeout=120)
            js = r'''
const a = require('./bridge.node'), assert = require('node:assert/strict');
const handle = {}, old = [], seen = [];
a.completed('unregistered');
assert.equal(a.set(handle, p => old.push(p)), true);
a.completed('superseded');
assert.equal(a.set(handle, p => old.push(p)), true);
a.completed('cleared');
a.clear(handle);
a.completed('unregistered again');
assert.deepEqual(a.take(handle), []);
assert.equal(a.set(handle, p => old.push(p)), true);
for(let i=0;i<260;i++) a.completed(`${i}`);
assert.deepEqual(a.take(handle), Array.from({length:256}, (_,i) => `${i}`));
assert.deepEqual(a.take(handle), []); // queued wakeup cannot replay drained paths
a.clear(handle);
const count = 32; // No queue-full loss even while JS is busy issuing stops.
const watchdog = setTimeout(() => { throw Error('missing archive notification'); }, 4000);
assert.equal(a.set(handle, p => {
  seen.push(p);
  if (seen.length === count) {
    a.clear(handle); // reentrant unsubscription on the JS thread
    assert.deepEqual(old, []);
    assert.deepEqual(seen, Array.from({length:count}, (_,i) => `本地/MD/${i}.mp4`));
    clearTimeout(watchdog);
  }
}), true);
a.completed('final segment'); assert.deepEqual(a.take(handle), ['final segment']);
for (let i=0; i<count; i++) a.completed(`本地/MD/${i}.mp4`);
'''
            (work / 'run.cjs').write_text(js, encoding='utf-8')
            run([node, str(work / 'run.cjs')], work, timeout=10)
            # A registration alone must not hold the env open. Forced env teardown
            # without page cleanup also detaches a still-queued native notification.
            run([node, '-e', "const a=require('./bridge.node'); a.set({},()=>{}); a.completed('shutdown');"],
                work, timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
