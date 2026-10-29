#!/usr/bin/env python3
"""Host regression test for the real MicStreamer AAC callbacks (Python stdlib only).

Run: python tests/aac_framing_test.py
MSVC is loaded from --vcvars, VCVARS64_BAT, or the VS 2022 Community default.
A preconfigured cl.exe environment also works. No SDK, device or network is used.
Only platform declarations and lifecycle are adapted: the class declaration,
OnReadData, OnNeedInputData, DrainPendingInputs, OnNewOutputData and ADTS helpers
come from production sources on every run. Build artifacts live in a temporary
folder and are removed afterward. --source can point at a candidate .cpp file.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "entry/src/main/cpp/mic_streamer.cpp"
HEADER = CPP.with_suffix(".h")
DEFAULT_VCVARS = Path(
    "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat"
)


def extract_function(source, name):
    """Extract the actual definition, counting braces outside comments/strings."""
    match = re.search(r"(?m)^[^\n;{}]*\b" + re.escape(name) + r"\s*\(", source)
    if not match:
        raise RuntimeError("Production function not found: " + name)
    opening = source.find("{", match.end())
    if opening < 0:
        raise RuntimeError("Missing function body: " + name)
    depth = 0
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():token.end()], source.count("\n", 0, match.start()) + 1
    raise RuntimeError("Unbalanced function body: " + name)


ADAPTER = r'''
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expr); } while (false)
#define OH_LOG_Print(...) ((void)0)
constexpr int AUDIOSTREAM_SUCCESS = 0;
constexpr int AV_ERR_OK = 0;
constexpr uint32_t AVCODEC_BUFFER_FLAGS_NONE = 0;
constexpr uint32_t AVCODEC_BUFFER_FLAGS_EOS = 1;
constexpr uint32_t AVCODEC_BUFFER_FLAGS_SYNC_FRAME = 2;
constexpr uint32_t AVCODEC_BUFFER_FLAGS_CODEC_DATA = 8;
struct OH_AudioCapturer {};
struct OH_AudioStreamBuilder {};
struct OH_AVFormat {};
struct OH_AVCodecBufferAttr {
  int64_t pts = 0;
  int32_t size = 0;
  int32_t offset = 0;
  uint32_t flags = 0;
};
struct OH_AVMemory {
  static constexpr size_t guard = 16;
  int32_t capacity;
  bool addressable = true;
  std::vector<uint8_t> bytes;
  explicit OH_AVMemory(int32_t cap)
      : capacity(cap), bytes(2 * guard + static_cast<size_t>(std::max(cap, 0)), 0xCD) {}
  uint8_t* addr() { return bytes.data() + guard; }
  void checkGuards() const {
    CHECK(std::all_of(bytes.begin(), bytes.begin() + guard,
                     [](uint8_t b) { return b == 0xCD; }));
    CHECK(std::all_of(bytes.end() - guard, bytes.end(),
                     [](uint8_t b) { return b == 0xCD; }));
  }
};
uint8_t* OH_AVMemory_GetAddr(OH_AVMemory* memory) {
  return memory != nullptr && memory->addressable ? memory->addr() : nullptr;
}
int32_t OH_AVMemory_GetSize(OH_AVMemory* memory) { return memory->capacity; }
struct Submission {
  uint32_t index;
  OH_AVCodecBufferAttr attr;
  std::vector<uint8_t> pcm;
};
struct OH_AVCodec {
  std::map<uint32_t, OH_AVMemory*> inputs;
  std::vector<Submission> submissions;
  std::vector<uint32_t> freed;
  std::function<void(uint32_t)> recycle;
};
int OH_AudioEncoder_PushInputData(OH_AVCodec* codec, uint32_t index,
                                OH_AVCodecBufferAttr attr) {
  CHECK(codec != nullptr);
  CHECK(attr.size > 0);  // A zero-size submission must fail, never spin.
  CHECK(attr.offset == 0);
  CHECK(attr.flags == AVCODEC_BUFFER_FLAGS_NONE);
  CHECK(codec->inputs.count(index) == 1);
  OH_AVMemory* memory = codec->inputs.at(index);
  CHECK(memory != nullptr && memory->addressable);
  CHECK(attr.size <= memory->capacity);
  memory->checkGuards();
  codec->submissions.push_back({index, attr,
      std::vector<uint8_t>(memory->addr(), memory->addr() + attr.size)});
  CHECK(codec->submissions.size() < 10000);  // Bound reentrant encoder callbacks.
  if (codec->recycle) codec->recycle(index);
  return AV_ERR_OK;
}
int OH_AudioEncoder_FreeOutputData(OH_AVCodec* codec, uint32_t index) {
  CHECK(codec != nullptr);
  codec->freed.push_back(index);
  return AV_ERR_OK;
}
namespace ipcam { using TimestampUs = int64_t; }
'''


TESTS = r'''
using ipcam::MicStreamer;

void initialize(MicStreamer& mic, OH_AVCodec& codec, int channels) {
  mic.encoder_ = &codec;
  mic.channels_ = channels;
  mic.sampleRate_ = 48000;
  mic.running_.store(true);
}

std::vector<uint8_t> samples(size_t start, size_t count, int channels) {
  std::vector<uint8_t> result;
  for (size_t sample = start; sample < start + count; ++sample) {
    for (int channel = 0; channel < channels; ++channel) {
      const uint16_t value = static_cast<uint16_t>(
          (sample * 73 + static_cast<size_t>(channel) * 1151) ^ 0x5A3C);
      result.push_back(static_cast<uint8_t>(value));
      result.push_back(static_cast<uint8_t>(value >> 8));
    }
  }
  return result;
}

void capture(MicStreamer& mic, size_t start, size_t count) {
  auto pcm = samples(start, count, mic.channels_);
  CHECK(MicStreamer::OnReadData(nullptr, &mic, pcm.data(),
                                static_cast<int32_t>(pcm.size())) == AUDIOSTREAM_SUCCESS);
}

void checkStream(const MicStreamer& mic, const OH_AVCodec& codec, size_t totalSamples) {
  const size_t expected = totalSamples / 1024;
  const size_t frameBytes = 1024 * 2 * static_cast<size_t>(mic.channels_);
  CHECK(codec.submissions.size() == expected);
  CHECK(mic.nextSampleFrame_ == expected * 1024);
  CHECK(mic.pcmQueue_ == samples(expected * 1024, totalSamples % 1024, mic.channels_));
  CHECK(!mic.drainActive_);
  for (size_t i = 0; i < expected; ++i) {
    const auto& pushed = codec.submissions[i];
    CHECK(static_cast<size_t>(pushed.attr.size) == frameBytes);
    CHECK(pushed.pcm == samples(i * 1024, 1024, mic.channels_));
    CHECK(pushed.attr.pts == static_cast<int64_t>(i * 1024 * 1000000ULL / 48000));
    if (i > 0) {
      const auto step = pushed.attr.pts - codec.submissions[i - 1].attr.pts;
      CHECK(step == 21333 || step == 21334);
    }
  }
}

void testCaptureCadence(int channels, int capacityMultiplier) {
  OH_AVCodec codec;
  MicStreamer mic;
  initialize(mic, codec, channels);
  const int frameBytes = 1024 * 2 * channels;
  OH_AVMemory input(frameBytes * capacityMultiplier);
  codec.inputs[7] = &input;
  codec.recycle = [&](uint32_t index) {
    MicStreamer::OnNeedInputData(&codec, index, &input, &mic);
  };
  MicStreamer::OnNeedInputData(&codec, 7, &input, &mic);
  CHECK(codec.submissions.empty());
  constexpr size_t callbacks = 137;
  for (size_t n = 0; n < callbacks; ++n) {
    capture(mic, n * 960, 960);
    checkStream(mic, codec, (n + 1) * 960);
    CHECK(mic.pendingInputs_.size() == 1);
    input.checkGuards();
    CHECK(std::all_of(input.addr() + frameBytes, input.addr() + input.capacity,
                     [](uint8_t byte) { return byte == 0xCD; }));
    if (n == 0) CHECK(codec.submissions.empty());  // 960 is not 1024.
  }
  CHECK(codec.submissions.size() == 128);
  CHECK(mic.pcmQueue_.size() == 448 * 2 * static_cast<size_t>(channels));
  const auto submittedBefore = codec.submissions.size();
  mic.DrainPendingInputs();
  CHECK(codec.submissions.size() == submittedBefore);  // Never pad the tail.
  capture(mic, callbacks * 960, 576);
  checkStream(mic, codec, callbacks * 960 + 576);
  CHECK(mic.pcmQueue_.empty());
  CHECK(codec.submissions.size() == 129);
  bool saw21333 = false;
  bool saw21334 = false;
  for (size_t i = 1; i < codec.submissions.size(); ++i) {
    const auto delta = codec.submissions[i].attr.pts - codec.submissions[i - 1].attr.pts;
    saw21333 |= delta == 21333;
    saw21334 |= delta == 21334;
  }
  CHECK(saw21333 && saw21334);
  std::cout << "PASS 137 x 960 samples, " << channels << "ch, capacity="
            << input.capacity << ": 128 frames + 448 samples; tail -> 129 frames\n";
}

void testDelayedInput() {
  OH_AVCodec codec;
  MicStreamer mic;
  initialize(mic, codec, 2);
  for (size_t i = 0; i < 5; ++i) capture(mic, i * 960, 960);
  CHECK(codec.submissions.empty());
  CHECK(mic.pcmQueue_ == samples(0, 4800, 2));
  OH_AVMemory input(4096);
  codec.inputs[3] = &input;
  codec.recycle = [&](uint32_t index) {
    MicStreamer::OnNeedInputData(&codec, index, &input, &mic);
  };
  MicStreamer::OnNeedInputData(&codec, 3, &input, &mic);
  checkStream(mic, codec, 4800);
  CHECK(codec.submissions.size() == 4);
  CHECK(mic.pendingInputs_.size() == 1);
  std::cout << "PASS delayed input availability and reentrant encoder callbacks\n";
}

void testInvalidInput(int channels, int capacity, bool addressable, bool nullMemory) {
  OH_AVCodec codec;
  MicStreamer mic;
  initialize(mic, codec, channels);
  capture(mic, 0, 1024);
  OH_AVMemory bad(capacity);
  bad.addressable = addressable;
  const auto untouched = bad.bytes;
  codec.inputs[1] = nullMemory ? nullptr : &bad;
  MicStreamer::OnNeedInputData(&codec, 1, codec.inputs[1], &mic);
  CHECK(codec.submissions.empty());
  CHECK(mic.pcmQueue_ == samples(0, 1024, channels));
  CHECK(mic.nextSampleFrame_ == 0);
  CHECK(!mic.drainActive_);
  CHECK(bad.bytes == untouched);
  bad.checkGuards();
  OH_AVMemory good(1024 * 2 * channels);
  codec.inputs[2] = &good;
  MicStreamer::OnNeedInputData(&codec, 2, &good, &mic);
  checkStream(mic, codec, 1024);
  CHECK(codec.submissions.size() == 1);
  CHECK(codec.submissions[0].index == 2);
}

void testOutput(int channels) {
  OH_AVCodec codec;
  MicStreamer mic;
  initialize(mic, codec, channels);
  std::vector<std::vector<uint8_t>> packets;
  std::vector<int64_t> timestamps;
  mic.sink_ = [&](const uint8_t* bytes, size_t size, int64_t pts) {
    packets.emplace_back(bytes, bytes + size);
    timestamps.push_back(pts);
  };
  OH_AVMemory output(8192);
  for (int i = 0; i < output.capacity; ++i) output.addr()[i] = static_cast<uint8_t>(i * 29 + 7);
  const auto original = output.bytes;
  OH_AVCodecBufferAttr attr{};
  attr.pts = 1234567;
  attr.offset = 5;
  attr.size = 9;
  uint32_t index = 10;
  auto deliver = [&](OH_AVMemory* memory, OH_AVCodecBufferAttr* attributes, bool emitted) {
    const auto before = packets.size();
    const auto freed = codec.freed.size();
    MicStreamer::OnNewOutputData(&codec, index, memory, attributes, &mic);
    CHECK(packets.size() == before + (emitted ? 1 : 0));
    CHECK(codec.freed.size() == freed + 1);
    CHECK(codec.freed.back() == index++);
    CHECK(output.bytes == original);
  };
  deliver(&output, &attr, true);
  CHECK(packets[0].size() == 16);
  const std::vector<uint8_t> expectedHeader{
      0xFF, 0xF1, 0x4C, static_cast<uint8_t>(channels << 6), 0x02, 0x1F, 0xFC};
  CHECK(std::equal(expectedHeader.begin(), expectedHeader.end(), packets[0].begin()));
  CHECK(std::equal(packets[0].begin() + 7, packets[0].end(), output.addr() + 5));
  CHECK(timestamps[0] == attr.pts);
  attr.flags = AVCODEC_BUFFER_FLAGS_CODEC_DATA;
  deliver(&output, &attr, false);
  attr.flags = AVCODEC_BUFFER_FLAGS_CODEC_DATA | AVCODEC_BUFFER_FLAGS_SYNC_FRAME;
  deliver(&output, &attr, false);
  attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
  deliver(&output, &attr, false);
  attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
  deliver(&output, nullptr, false);
  deliver(nullptr, &attr, false);
  output.addressable = false;
  deliver(&output, &attr, false);
  output.addressable = true;
  attr.offset = -1;
  deliver(&output, &attr, false);
  attr.offset = output.capacity + 1;
  deliver(&output, &attr, false);
  attr.offset = output.capacity - 2;
  deliver(&output, &attr, false);
  attr.offset = std::numeric_limits<int32_t>::max();
  deliver(&output, &attr, false);
  attr.offset = 0;
  attr.size = 0;
  deliver(&output, &attr, false);
  attr.size = -1;
  deliver(&output, &attr, false);
  attr.size = std::numeric_limits<int32_t>::max();
  deliver(&output, &attr, false);
  attr.size = 8191 - 7 + 1;
  deliver(&output, &attr, false);
  attr.size = 8191 - 7;
  deliver(&output, &attr, true);
  CHECK(packets.back().size() == 8191);
  const auto& largest = packets.back();
  CHECK((((largest[3] & 3) << 11) | (largest[4] << 3) | (largest[5] >> 5)) == 8191);
  CHECK(std::equal(largest.begin() + 7, largest.end(), output.addr()));
  mic.sampleRate_ = 12345;
  deliver(&output, &attr, false);
  mic.sampleRate_ = 48000;
  mic.sink_ = nullptr;
  deliver(&output, &attr, false);
  std::cout << "PASS " << channels << "ch output: CODEC_DATA/EOS ignored, offset/ADTS/PTS, "
               "bounds and exactly-once output release\n";
}

int main() {
  try {
    for (int channels : {1, 2}) {
      for (int capacityMultiplier : {1, 3}) testCaptureCadence(channels, capacityMultiplier);
      for (int capacity : {-1, 0, 1, 1024 * 2 * channels - 1}) {
        testInvalidInput(channels, capacity, true, false);
      }
      testInvalidInput(channels, 1024 * 2 * channels, false, false);
      testInvalidInput(channels, 1024 * 2 * channels, true, true);
      testOutput(channels);
    }
    std::cout << "PASS undersized/null/invalid input buffers: no write, no submission, "
                 "PCM preserved, recovery with a valid buffer\n";
    testDelayedInput();
    std::cout << "All AAC framing host tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
'''


def build_source(source_path):
    source = source_path.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    # Keep the actual class layout/declarations; replace only platform includes
    # and access control so the harness can inspect queues and drive callbacks.
    header = re.sub(r'^#include <(?:multimedia|ohaudio)/[^\n]+\n', '', header, flags=re.M)
    header = header.replace('#include "media_time.h"', '')
    header = re.sub(r'(?m)^([ \t]*)private:', r'\1public:', header)
    parts = [ADAPTER, '#line 1 "mic_streamer.h"\n', header, '\nnamespace ipcam {\n']
    constant = re.search(r'constexpr\s+size_t\s+kAacSamplesPerFrame\s*=\s*[^;]+;', source)
    if constant:
        parts.append(constant.group())
    for name in ("SfIndexForRate", "BuildAdts", "MicStreamer::OnReadData",
                 "MicStreamer::OnNeedInputData", "MicStreamer::DrainPendingInputs",
                 "MicStreamer::OnNewOutputData"):
        body, line = extract_function(source, name)
        parts.extend(['\n#line %d "%s"\n' % (line, source_path.as_posix()), body])
    parts.append('\nMicStreamer::~MicStreamer() = default;\n}\n#line 1 "aac_framing_harness.cpp"\n')
    parts.append(TESTS)
    return "\n".join(parts)


def run(args):
    if os.name != "nt":
        raise RuntimeError("This host harness requires Windows with MSVC cl.exe.")
    source_path = args.source.resolve()
    generated = build_source(source_path)
    vcvars = args.vcvars
    if vcvars is None and not shutil.which("cl.exe"):
        vcvars = Path(os.environ.get("VCVARS64_BAT", str(DEFAULT_VCVARS)))
    if vcvars is not None and not vcvars.is_file():
        raise RuntimeError("MSVC setup not found: %s (use --vcvars)" % vcvars)
    with tempfile.TemporaryDirectory(prefix="aac_framing_") as temp:
        build = Path(temp)
        (build / "aac_framing.cpp").write_text(generated, encoding="utf-8")
        commands = ["@echo off"]
        if vcvars is not None:
            commands.extend(['call "%s" >nul' % vcvars.resolve(), 'if errorlevel 1 exit /b 1'])
        commands.extend([
            'cl.exe /nologo /std:c++17 /EHsc /W4 /utf-8 aac_framing.cpp '
            '/Fo:aac_framing.obj /Fe:aac_framing.exe',
            'exit /b %errorlevel%',
        ])
        batch = build / "build.cmd"
        batch.write_text("\r\n".join(commands) + "\r\n", encoding="utf-8")
        print("Testing actual callbacks from %s" % source_path, flush=True)
        compiled = subprocess.run([os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", str(batch)],
                                  cwd=build, capture_output=True, text=True, errors="replace", timeout=120)
        if compiled.stdout:
            print(compiled.stdout, end="")
        if compiled.stderr:
            print(compiled.stderr, end="", file=sys.stderr)
        if compiled.returncode:
            raise RuntimeError("MSVC compilation failed (exit %d)." % compiled.returncode)
        tested = subprocess.run([str(build / "aac_framing.exe")], cwd=build,
                                capture_output=True, text=True, errors="replace", timeout=30)
        if tested.stdout:
            print(tested.stdout, end="")
        if tested.stderr:
            print(tested.stderr, end="", file=sys.stderr)
        return tested.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcvars", type=Path, help="Path to vcvars64.bat (optional if cl is configured)")
    parser.add_argument("--source", type=Path, default=CPP, help="MicStreamer .cpp candidate to exercise")
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("ERROR: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
