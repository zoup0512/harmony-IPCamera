#!/usr/bin/env python3
"""Host unit tests for pure protocol helpers (Python stdlib + MSVC only).

Run: python tests/protocol_units_test.py
MSVC is loaded from --vcvars, VCVARS64_BAT, or the VS 2022 Community default.
Covers production code compiled as-is:
  * RtmpChunkParser (rtmp_chunk_parser.h): fmt 0-3 headers, fmt-3 messages that
    reuse the previous header, extended timestamps on fmt-3 chunks,
    SetChunkSize, interleaved chunk streams, byte-by-byte delivery, and
    connect/publish rejection detection.
  * MergeParamSets (param_sets.h): per-NAL-type replacement, bounded size,
    VPS/SPS/PPS ordering, codec switches.
  * RtspPathMatches (extracted from rtsp_server.cpp on every run).
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from aac_framing_test import DEFAULT_VCVARS, extract_function  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CPP_DIR = ROOT / "entry/src/main/cpp"
RTSP = CPP_DIR / "rtsp_server.cpp"

TESTS = r'''
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "param_sets.h"
#include "rtmp_chunk_parser.h"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expr); } while (false)

using Bytes = std::vector<uint8_t>;
using ipcam::RtmpChunkParser;
using Status = RtmpChunkParser::Status;

namespace ipcam { bool RtspPathMatches(const std::string& url, const std::string& path); }

// ---- RTMP chunk builders --------------------------------------------------
void Put1(Bytes& v, uint32_t x) { v.push_back(static_cast<uint8_t>(x & 0xFF)); }
void Put3(Bytes& v, uint32_t x) { Put1(v, x >> 16); Put1(v, x >> 8); Put1(v, x); }
void Put4(Bytes& v, uint32_t x) { Put1(v, x >> 24); Put3(v, x); }
void Basic(Bytes& v, uint8_t fmt, uint32_t csid) {
  if (csid >= 64 + 256) {
    v.push_back(static_cast<uint8_t>(fmt << 6 | 1));
    Put1(v, csid - 64);
    Put1(v, (csid - 64) >> 8);
  } else if (csid >= 64) {
    v.push_back(static_cast<uint8_t>(fmt << 6));
    v.push_back(static_cast<uint8_t>(csid - 64));
  } else {
    v.push_back(static_cast<uint8_t>(fmt << 6 | csid));
  }
}
void Fmt0(Bytes& v, uint32_t csid, uint32_t ts, uint32_t len, uint8_t type) {
  Basic(v, 0, csid);
  Put3(v, ts >= 0xFFFFFF ? 0xFFFFFF : ts);
  Put3(v, len);
  v.push_back(type);
  for (int i = 0; i < 4; ++i) v.push_back(0);  // message stream id (LE)
  if (ts >= 0xFFFFFF) Put4(v, ts);
}
void Append(Bytes& v, const Bytes& payload, size_t from, size_t n) {
  v.insert(v.end(), payload.begin() + from, payload.begin() + from + n);
}
// A whole message split by `chunk`: fmt-0 (or `firstFmt`) then fmt-3 chunks.
// When `extTs` is set every chunk header is followed by the 4-byte timestamp.
Bytes Message(uint32_t csid, uint8_t type, const Bytes& payload, size_t chunk,
              uint32_t ts = 0, int firstFmt = 0) {
  Bytes v;
  if (firstFmt == 0) {
    Fmt0(v, csid, ts, static_cast<uint32_t>(payload.size()), type);
  } else {
    Basic(v, static_cast<uint8_t>(firstFmt), csid);
    if (firstFmt == 2) Put3(v, ts);
  }
  size_t off = 0;
  for (bool first = true; first || off < payload.size(); first = false) {
    if (!first) {
      Basic(v, 3, csid);
      if (ts >= 0xFFFFFF) Put4(v, ts);
    }
    const size_t n = std::min(chunk, payload.size() - off);
    Append(v, payload, off, n);
    off += n;
  }
  return v;
}
Bytes Text(const std::string& s, size_t pad = 0) {
  Bytes b(s.begin(), s.end());
  b.resize(b.size() + pad, 'x');
  return b;
}
Bytes ChunkSizeMsg(uint32_t size) {
  Bytes p;
  Put4(p, size);
  return Message(2, 1, p, 128);
}

// Feeds `stream` like SenderLoop does: append to a buffer, consume, erase.
struct Feed {
  RtmpChunkParser parser{4u * 1024u * 1024u};
  Bytes buf;
  Status status = Status::kOk;
  std::string detail;
  Status Push(const Bytes& stream, size_t step) {
    for (size_t off = 0; off < stream.size(); off += step) {
      const size_t n = std::min(step, stream.size() - off);
      buf.insert(buf.end(), stream.begin() + off, stream.begin() + off + n);
      const size_t used = parser.Consume(buf.data(), buf.size(), &status, &detail);
      CHECK(used <= buf.size());
      buf.erase(buf.begin(), buf.begin() + used);
      if (status != Status::kOk) return status;
    }
    return status;
  }
};

void TestRtmpParser() {
  for (size_t step : {size_t(1), size_t(7), size_t(100000)}) {
    // SetChunkSize, then a 300-byte command split at the new size.
    {
      Feed f;
      Bytes s = ChunkSizeMsg(256);
      const Bytes ok = Text("onStatus NetStream.Publish.Start", 268);
      Bytes m = Message(3, 20, ok, 256);
      s.insert(s.end(), m.begin(), m.end());
      CHECK(f.Push(s, step) == Status::kOk);
      CHECK(f.parser.chunkSize() == 256);
      CHECK(f.buf.empty());
    }
    // fmt-3 chunk that starts a NEW message reuses the declared length/type.
    {
      Feed f;
      const Bytes first = Text("onStatus NetStream.Publish.Start", 268);  // 300
      const Bytes second = Text("onStatus NetStream.Publish.BadName", 266);
      CHECK(first.size() == 300 && second.size() == 300);
      Bytes s = Message(3, 20, first, 128);
      Bytes again = Message(3, 20, second, 128, 0, 3);
      s.insert(s.end(), again.begin(), again.end());
      CHECK(f.Push(s, step) == Status::kRejected);
      CHECK(f.detail == "NetStream.Publish.BadName");
      CHECK(f.buf.empty());
    }
    // fmt-2 starts a new message inheriting length/type.
    {
      Feed f;
      Bytes s = Message(3, 20, Text("_result", 93), 128);
      Bytes next = Message(3, 20, Text("_error", 94), 128, 5, 2);
      s.insert(s.end(), next.begin(), next.end());
      CHECK(f.Push(s, step) == Status::kRejected);
      CHECK(f.detail == "_error");
    }
    // Extended timestamp: every fmt-3 continuation carries 4 extra bytes. If
    // they are not skipped the following SetChunkSize is misparsed.
    {
      Feed f;
      Bytes s = Message(4, 18, Text("onMetaData", 290), 128, 0x01000000);
      Bytes cs = ChunkSizeMsg(4000);
      s.insert(s.end(), cs.begin(), cs.end());
      CHECK(f.Push(s, step) == Status::kOk);
      CHECK(f.parser.chunkSize() == 4000);
      CHECK(f.buf.empty());
    }
    // Interleaved chunk streams (csid 3 command, csid 70 two-byte basic header).
    {
      Feed f;
      const Bytes a = Text("_result connect", 185);  // 200 bytes
      const Bytes b = Text("audio", 195);
      Bytes s;
      Fmt0(s, 3, 0, 200, 20);  Append(s, a, 0, 128);
      Fmt0(s, 70, 0, 200, 8);  Append(s, b, 0, 128);
      Basic(s, 3, 3);          Append(s, a, 128, 72);
      Basic(s, 3, 70);         Append(s, b, 128, 72);
      Bytes cs = ChunkSizeMsg(512);
      s.insert(s.end(), cs.begin(), cs.end());
      CHECK(f.Push(s, step) == Status::kOk);
      CHECK(f.parser.chunkSize() == 512);
    }
    // Three-byte basic header + zero-length message.
    {
      Feed f;
      Bytes s;
      Fmt0(s, 400, 0, 0, 4);
      Bytes cs = ChunkSizeMsg(300);
      s.insert(s.end(), cs.begin(), cs.end());
      CHECK(f.Push(s, step) == Status::kOk);
      CHECK(f.parser.chunkSize() == 300);
    }
    // Invalid chunk sizes.
    {
      Feed f;
      CHECK(f.Push(ChunkSizeMsg(0), step) == Status::kInvalidChunkSize);
      Feed g;
      CHECK(g.Push(ChunkSizeMsg(5u * 1024u * 1024u), step) == Status::kInvalidChunkSize);
    }
    // Rejection on connect.
    {
      Feed f;
      CHECK(f.Push(Message(3, 20, Text("_error NetConnection.Connect.Rejected", 10), 128),
                   step) == Status::kRejected);
    }
  }
  // Truncated input never over-consumes.
  {
    RtmpChunkParser p(1u << 20);
    Bytes s = Message(3, 20, Text("_result", 200), 128);
    Status st;
    std::string d;
    for (size_t n = 0; n < s.size(); ++n) {
      RtmpChunkParser fresh(1u << 20);
      const size_t used = fresh.Consume(s.data(), n, &st, &d);
      CHECK(used <= n);
      CHECK(st == Status::kOk);
    }
  }
  std::cout << "PASS RTMP chunk parser: fmt0-3, fmt-3 new message, extended ts, "
               "interleave, chunk size, rejection, partial delivery\n";
}

// ---- Parameter sets ----------------------------------------------------------
Bytes Nal(std::initializer_list<uint8_t> b, bool longStart = true) {
  Bytes v = longStart ? Bytes{0, 0, 0, 1} : Bytes{0, 0, 1};
  v.insert(v.end(), b);
  return v;
}
Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes v;
  for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
  return v;
}

void TestParamSets() {
  using ipcam::MergeParamSets;
  const Bytes sps1 = Nal({0x67, 0x42, 0x00, 0x1F});
  const Bytes sps2 = Nal({0x67, 0x64, 0x00, 0x28});
  const Bytes pps1 = Nal({0x68, 0xCE, 0x3C});
  const Bytes pps2 = Nal({0x68, 0xEE, 0x3C});

  Bytes cache = MergeParamSets({}, Cat({sps1, pps1}), false);
  CHECK(cache == Cat({sps1, pps1}));
  // PPS-only update keeps the SPS.
  cache = MergeParamSets(cache, pps2, false);
  CHECK(cache == Cat({sps1, pps2}));
  // SPS-only update keeps the PPS; order stays SPS, PPS.
  cache = MergeParamSets(cache, sps2, false);
  CHECK(cache == Cat({sps2, pps2}));
  // Repeated keyframes do not grow the cache.
  for (int i = 0; i < 100; ++i) cache = MergeParamSets(cache, Cat({sps2, pps2}), false);
  CHECK(cache == Cat({sps2, pps2}));
  // 3-byte start codes and trailing zero bytes normalise; slices are dropped.
  cache = MergeParamSets({}, Cat({Nal({0x68, 0xCE, 0x3C, 0x00}, false),
                                  Nal({0x67, 0x42, 0x00, 0x1F}, false),
                                  Nal({0x65, 0x88, 0x84})}), false);
  CHECK(cache == Cat({sps1, pps1}));

  const Bytes vps = Nal({0x40, 0x01, 0x0C});
  const Bytes hsps = Nal({0x42, 0x01, 0x01});
  const Bytes hpps = Nal({0x44, 0x01, 0xC1});
  const Bytes hpps2 = Nal({0x44, 0x01, 0xC2});
  Bytes h = MergeParamSets({}, Cat({hpps, hsps, vps}), true);
  CHECK(h == Cat({vps, hsps, hpps}));
  h = MergeParamSets(h, hpps2, true);
  CHECK(h == Cat({vps, hsps, hpps2}));
  // Codec switch: stale H.264 sets never leak into an H.265 cache and back.
  CHECK(MergeParamSets(Cat({sps1, pps1}), Cat({vps, hsps, hpps}), true) ==
        Cat({vps, hsps, hpps}));
  CHECK(MergeParamSets(Cat({vps, hsps, hpps}), Cat({sps1, pps1}), false) ==
        Cat({sps1, pps1}));
  // Empty fresh input keeps the cache.
  CHECK(MergeParamSets(Cat({sps1, pps1}), {}, false) == Cat({sps1, pps1}));
  std::cout << "PASS param sets: per-type replacement, bounded, ordered, codec switch\n";
}

// ---- RTSP path -------------------------------------------------------------
void TestRtspPath() {
  using ipcam::RtspPathMatches;
  const std::string host = "rtsp://192.168.1.2:8554";
  CHECK(RtspPathMatches(host + "/live", "live"));
  CHECK(RtspPathMatches(host + "/live/", "live"));
  CHECK(RtspPathMatches(host + "/live/trackID=0", "live"));
  CHECK(RtspPathMatches(host + "/live?token=1", "live"));
  CHECK(RtspPathMatches(host + "/live", "/live/"));
  CHECK(RtspPathMatches(host + "/a/b/trackID=1", "a/b"));
  CHECK(!RtspPathMatches(host + "/liveX", "live"));
  CHECK(!RtspPathMatches(host + "/other", "live"));
  CHECK(!RtspPathMatches(host, "live"));
  CHECK(!RtspPathMatches(host + "/", "live"));
  CHECK(!RtspPathMatches(host + "/LIVE", "live"));
  CHECK(RtspPathMatches("trackID=0", "live"));   // relative control URL
  CHECK(RtspPathMatches("*", "live"));           // OPTIONS/GET_PARAMETER *
  CHECK(RtspPathMatches("/live", "live"));
  CHECK(!RtspPathMatches("/x", "live"));
  CHECK(RtspPathMatches(host + "/anything", ""));
  CHECK(RtspPathMatches(host + "/anything", "/"));
  std::cout << "PASS RTSP path matching\n";
}

int main() {
  try {
    TestRtmpParser();
    TestParamSets();
    TestRtspPath();
    std::cout << "All protocol unit tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
'''


def build_source():
    source = RTSP.read_text(encoding="utf-8")
    body, line = extract_function(source, "RtspPathMatches")
    return "\n".join([
        TESTS,
        "#include <string>\nnamespace ipcam {",
        '#line %d "%s"' % (line, RTSP.as_posix()),
        body,
        "}",
    ])


def run(args):
    if os.name != "nt":
        raise RuntimeError("This host harness requires Windows with MSVC cl.exe.")
    vcvars = args.vcvars
    if vcvars is None and not shutil.which("cl.exe"):
        vcvars = Path(os.environ.get("VCVARS64_BAT", str(DEFAULT_VCVARS)))
    if vcvars is not None and not vcvars.is_file():
        raise RuntimeError("MSVC setup not found: %s (use --vcvars)" % vcvars)
    with tempfile.TemporaryDirectory(prefix="protocol_units_") as temp:
        build = Path(temp)
        (build / "protocol_units.cpp").write_text(build_source(), encoding="utf-8")
        commands = ["@echo off"]
        if vcvars is not None:
            commands.extend(['call "%s" >nul' % vcvars.resolve(), "if errorlevel 1 exit /b 1"])
        commands.extend([
            'cl.exe /nologo /std:c++17 /EHsc /W4 /utf-8 /I "%s" protocol_units.cpp '
            "/Fo:protocol_units.obj /Fe:protocol_units.exe" % CPP_DIR,
            "exit /b %errorlevel%",
        ])
        batch = build / "build.cmd"
        batch.write_text("\r\n".join(commands) + "\r\n", encoding="utf-8")
        compiled = subprocess.run([os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", str(batch)],
                                  cwd=build, capture_output=True, text=True, errors="replace",
                                  timeout=120)
        if compiled.stdout:
            print(compiled.stdout, end="")
        if compiled.stderr:
            print(compiled.stderr, end="", file=sys.stderr)
        if compiled.returncode:
            raise RuntimeError("MSVC compilation failed (exit %d)." % compiled.returncode)
        tested = subprocess.run([str(build / "protocol_units.exe")], cwd=build,
                                capture_output=True, text=True, errors="replace", timeout=30)
        if tested.stdout:
            print(tested.stdout, end="")
        if tested.stderr:
            print(tested.stderr, end="", file=sys.stderr)
        return tested.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcvars", type=Path, help="Path to vcvars64.bat (optional if cl is configured)")
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("ERROR: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
