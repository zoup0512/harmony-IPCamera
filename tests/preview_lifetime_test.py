"""Native OSD preview lifetime regression; Python stdlib and a C++17 compiler only.

Run from the repository root: python tests/preview_lifetime_test.py -v
The complete production .cpp/.h are compiled with host SDK stubs. All generated
files stay in a temporary directory; no Harmony SDK, device, or network is used.
Gates and non-owner try_lock probes check lock scope without scheduling sleeps.
Timeouts are deadlock watchdogs, not performance assertions.
"""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
CPP = REPO / "entry" / "src" / "main" / "cpp"
VCVARS = Path(
    "C:/Program Files/Microsoft Visual Studio/2022/Community/"
    "VC/Auxiliary/Build/vcvars64.bat"
)

SDK = r"""
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

inline void Check(bool ok, const char* message) {
  if (!ok) {
    std::cerr << "FAIL: " << message << std::endl;
    std::_Exit(1);  // Also terminates workers blocked by an intentionally bad lock.
  }
}

class Gate {
  std::mutex mu_;
  std::condition_variable cv_;
  bool armed_ = false, entered_ = false, open_ = false;
 public:
  void Arm() {
    std::lock_guard<std::mutex> lock(mu_);
    Check(!armed_, "gate rearmed before previous call returned");
    armed_ = true;
    entered_ = open_ = false;
  }
  void Hit() {
    std::unique_lock<std::mutex> lock(mu_);
    if (!armed_) return;
    Check(!entered_, "two native calls overlapped at one gate");
    entered_ = true;
    cv_.notify_all();
    Check(cv_.wait_for(lock, std::chrono::seconds(8), [&] { return open_; }),
          "native gate was never released");
    armed_ = false;
  }
  void Wait(const char* message) {
    std::unique_lock<std::mutex> lock(mu_);
    Check(cv_.wait_for(lock, std::chrono::seconds(8), [&] { return entered_; }),
          message);
  }
  void Open() {
    std::lock_guard<std::mutex> lock(mu_);
    open_ = true;
    cv_.notify_all();
  }
};

class Counter {
  std::mutex mu_;
  std::condition_variable cv_;
  int value_ = 0;
 public:
  void Signal() {
    std::lock_guard<std::mutex> lock(mu_);
    ++value_;
    cv_.notify_all();
  }
  void Wait(int target, const char* message) {
    std::unique_lock<std::mutex> lock(mu_);
    Check(cv_.wait_for(lock, std::chrono::seconds(8), [&] { return value_ >= target; }),
          message);
  }
};

constexpr uint64_t NATIVEBUFFER_USAGE_CPU_READ = 1, NATIVEBUFFER_USAGE_CPU_WRITE = 2;
constexpr int NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP = 1;
constexpr int NATIVEBUFFER_PIXEL_FMT_YCRCB_420_SP = 2;
constexpr int NATIVEBUFFER_PIXEL_FMT_YCBCR_420_P = 3;
constexpr int NATIVEBUFFER_ROTATE_NONE = 0;
enum WindowOption {
  GET_TRANSFORM, SET_TRANSFORM, SET_TIMEOUT, SET_USAGE, SET_BUFFER_GEOMETRY,
  SET_FORMAT, GET_BUFFER_GEOMETRY, GET_FORMAT, GET_USAGE
};
struct BufferHandle {
  int width = 4, height = 4, stride = 4, size = 24;
  int format = NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP;
};
struct OHNativeWindowBuffer;
struct OH_NativeBuffer {
  OHNativeWindowBuffer* owner;
  std::atomic<int> maps{0};
};
struct OHNativeWindowBuffer {
  BufferHandle handle;
  std::array<uint8_t, 24> pixels{};
  OH_NativeBuffer native{this};
};
struct OHNativeWindow {
  uint64_t id;
  OHNativeWindowBuffer buffer;
  int transform = 4;  // Pure flip: rotation stays zero; cleanup must restore it.
  uint64_t usage = 0;
  std::atomic<bool> destroyed{false};
  std::atomic<int> outstanding{0}, requests{0}, flushes{0}, aborts{0}, destroys{0};
  explicit OHNativeWindow(uint64_t surfaceId) : id(surfaceId) {}
};
struct Region { void* rects; int count; };
struct OH_OnFrameAvailableListener {
  void* context;
  void (*onFrameAvailable)(void*);
};
struct OH_NativeImage {
  OHNativeWindow camera{0};
  OH_OnFrameAvailableListener listener{};
  std::mutex mu;
  bool pending = false;
  Counter released;
};

enum RequestMode { Success, Error, NullBuffer, Unusable };
inline std::atomic<int> requestMode{Success}, outstanding{0};
inline std::atomic<bool> creating{false}, destroying{false};
inline Gate requestGate, flushGate, abortGate, createGate, destroyGate;
inline std::mutex windowsMu;
// Retain tombstones so a regression reports a lifetime violation, not host UAF.
inline std::vector<std::unique_ptr<OHNativeWindow>> windows;
inline size_t CreatedCount() {
  std::lock_guard<std::mutex> lock(windowsMu);
  return windows.size();
}
inline void Alive(OHNativeWindow* w) {
  Check(w && !w->destroyed.load(), "native operation used a destroyed window");
}
inline int OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t id,
                                                          OHNativeWindow** out) {
  Check(outstanding == 0 && !destroying, "replacement overlapped presentation/destruction");
  Check(!creating.exchange(true), "concurrent window creation");
  createGate.Hit();
  std::lock_guard<std::mutex> lock(windowsMu);
  windows.emplace_back(new OHNativeWindow(id));
  *out = windows.back().get();
  creating = false;
  return 0;
}
inline int OH_NativeWindow_NativeWindowHandleOpt(OHNativeWindow* w, int op, ...) {
  Alive(w);
  va_list args;
  va_start(args, op);
  switch (op) {
    case GET_TRANSFORM: *va_arg(args, int*) = w->transform; break;
    case SET_TRANSFORM: w->transform = va_arg(args, int); break;
    case SET_TIMEOUT: (void)va_arg(args, int); break;
    case SET_USAGE: w->usage = va_arg(args, uint64_t); break;
    case SET_FORMAT: w->buffer.handle.format = va_arg(args, int); break;
    case SET_BUFFER_GEOMETRY: {
      int width = va_arg(args, int), height = va_arg(args, int);
      Check(width == 4 && height == 4, "unexpected host fixture geometry");
      break;
    }
    case GET_BUFFER_GEOMETRY:
      *va_arg(args, int*) = w->buffer.handle.height;
      *va_arg(args, int*) = w->buffer.handle.width;
      break;
    case GET_FORMAT: *va_arg(args, int*) = w->buffer.handle.format; break;
    case GET_USAGE: *va_arg(args, uint64_t*) = w->usage; break;
    default: Check(false, "unexpected window option");
  }
  va_end(args);
  return 0;
}
inline void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow* w) {
  Alive(w);
  Check(outstanding == 0 && w->outstanding == 0,
        "window destroyed during RequestBuffer/FlushBuffer/AbortBuffer");
  Check(!creating && !destroying.exchange(true), "destruction overlapped replacement");
  destroyGate.Hit();
  Check(!w->destroyed.exchange(true), "window destroyed twice");
  ++w->destroys;
  destroying = false;
}
inline int OH_NativeWindow_NativeWindowRequestBuffer(OHNativeWindow* w,
                                                   OHNativeWindowBuffer** out,
                                                   int* fence) {
  Alive(w);
  Check(!creating && !destroying, "request overlapped a lifecycle operation");
  Check(w->outstanding.fetch_add(1) == 0, "overlapping requests on one window");
  Check(outstanding.fetch_add(1) == 0, "overlapping preview presentations");
  ++w->requests;
  requestGate.Hit();
  Alive(w);
  *fence = -1;
  *out = nullptr;
  int mode = requestMode.load();
  if (mode == Error || mode == NullBuffer) {
    --w->outstanding;
    --outstanding;
    return mode == Error ? -1 : 0;
  }
  w->buffer.handle.format = mode == Unusable ? -1 : NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP;
  *out = &w->buffer;
  return 0;
}
inline int ReturnBuffer(OHNativeWindow* w, OHNativeWindowBuffer* b, bool flush) {
  Alive(w);
  Check(b == &w->buffer && w->outstanding == 1, "returning the wrong buffer");
  (flush ? flushGate : abortGate).Hit();
  Alive(w);
  if (flush) ++w->flushes; else ++w->aborts;
  --w->outstanding;
  --outstanding;
  return 0;
}
inline int OH_NativeWindow_NativeWindowFlushBuffer(OHNativeWindow* w,
                                                  OHNativeWindowBuffer* b, int,
                                                  Region) {
  return ReturnBuffer(w, b, true);
}
inline int OH_NativeWindow_NativeWindowAbortBuffer(OHNativeWindow* w,
                                                  OHNativeWindowBuffer* b) {
  return ReturnBuffer(w, b, false);
}
inline BufferHandle* OH_NativeWindow_GetBufferHandleFromNative(OHNativeWindowBuffer* b) {
  return &b->handle;
}
inline int OH_NativeBuffer_FromNativeWindowBuffer(OHNativeWindowBuffer* b,
                                                 OH_NativeBuffer** out) {
  *out = &b->native;
  return 0;
}
inline int OH_NativeBuffer_Map(OH_NativeBuffer* b, void** out) {
  ++b->maps;
  *out = b->owner->pixels.data();
  return 0;
}
inline int OH_NativeBuffer_Unmap(OH_NativeBuffer*) {
  Check(false, "borrowed camera/preview buffer was unmapped");
  return -1;
}
inline int OH_NativeBuffer_Unreference(OH_NativeBuffer*) {
  Check(false, "borrowed camera/preview buffer was unreferenced");
  return -1;
}
inline OH_NativeImage* OH_NativeImage_Create(int, int) { return new OH_NativeImage; }
inline OHNativeWindow* OH_NativeImage_AcquireNativeWindow(OH_NativeImage* image) {
  return &image->camera;
}
inline int OH_NativeImage_GetSurfaceId(OH_NativeImage*, uint64_t* out) {
  *out = 100;
  return 0;
}
inline int OH_NativeImage_SetOnFrameAvailableListener(OH_NativeImage* image,
                                                      OH_OnFrameAvailableListener listener) {
  image->listener = listener;
  return 0;
}
inline int OH_NativeImage_AcquireNativeWindowBuffer(OH_NativeImage* image,
                                                   OHNativeWindowBuffer** out, int* fence) {
  std::lock_guard<std::mutex> lock(image->mu);
  if (!image->pending) return -1;
  image->pending = false;
  *out = &image->camera.buffer;
  *fence = -1;
  return 0;
}
inline int OH_NativeImage_ReleaseNativeWindowBuffer(OH_NativeImage* image,
                                                   OHNativeWindowBuffer* b, int) {
  Check(b == &image->camera.buffer, "unexpected camera release");
  image->released.Signal();
  return 0;
}
inline void OH_NativeImage_Destroy(OH_NativeImage** image) {
  delete *image;
  *image = nullptr;
}
"""

HARNESS = r"""
#include "host_sdk.h"
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include "osd_renderer.h"
// Expose state only; every pipeline method and member type remains production code.
#define private public
#include "osd_pipeline.h"
#undef private
#include "osd_pipeline.cpp"

namespace ipcam {
OsdSnapshot GetOsd() { return {}; }
bool OsdRenderer::RefreshIfNeeded(const OsdSnapshot&, int, int) { return false; }
OsdRenderer::Rect OsdRenderer::TextRect(const OsdSnapshot&, int, int) const { return {}; }
OsdRenderer::Rect OsdRenderer::WatermarkRect(const OsdSnapshot&, int, int) const { return {}; }
}
static_assert(std::is_same<decltype(ipcam::OsdPipeline::uiActive_),
                           std::atomic<bool>>::value, "uiActive_ must remain atomic");

void UiLocked(ipcam::OsdPipeline& p) {
  // Called by the controller, never by the worker holding the mutex.
  bool acquired = p.uiMu_.try_lock();
  if (acquired) p.uiMu_.unlock();
  Check(!acquired, "uiMu_ does not cover the gated production operation");
}

struct Fixture {
  ipcam::OsdPipeline p;
  std::atomic<int> encoded{0};
  int frames = 0;
  Fixture() {
    uint64_t cameraId = 0;
    Check(p.Start(4, 4, 1, &cameraId,
                  [this](const uint8_t*, size_t n) {
                    Check(n == 24, "encoder received the wrong frame size");
                    ++encoded;
                  }, [](const std::string&) { Check(false, "pipeline reported an error"); }),
          "pipeline did not start");
    Check(cameraId == 100 && p.uiActive_.load(), "initial preview not active");
  }
  ~Fixture() { p.Stop(); }
  void Feed(uint8_t value) {
    auto* image = p.image_;
    {
      std::lock_guard<std::mutex> lock(image->mu);
      Check(!image->pending, "camera fixture already has a pending frame");
      image->camera.buffer.pixels.fill(value);
      image->pending = true;
    }
    image->listener.onFrameAvailable(image->listener.context);
    image->released.Wait(++frames, "compositor stalled behind the preview window lock");
    Check(encoded == frames, "compositor did not deliver every frame to the encoder");
    Check(image->camera.buffer.native.maps == 1, "camera buffer was unnecessarily remapped");
  }
  void QuitPreview() {
    {
      std::lock_guard<std::mutex> lock(p.presentMu_);
      p.presentQuit_ = true;
      p.presentPending_ = false;
    }
    p.presentCv_.notify_all();
  }
  void JoinPreview() {
    if (p.previewThread_.joinable()) p.previewThread_.join();
  }
  void CheckInactive() {
    Check(!p.uiActive_.load(), "cleanup did not publish inactive preview state");
    Feed(44);  // Release occurs AFTER the actual CompositeOne active-state read.
    std::lock_guard<std::mutex> lock(p.presentMu_);
    Check(!p.presentPending_, "inactive preview received a queued frame");
  }
  void CheckCachesClearedOnlyForPreview() {
    std::lock_guard<std::mutex> lock(p.mapMu_);
    Check(p.previewMapCache_.empty(), "preview teardown retained stale mappings");
    Check(p.mapCache_.size() == 1, "preview teardown discarded camera mappings");
  }
};

void Lifetime(bool replace, bool abort) {
  Fixture f;
  auto& p = f.p;
  auto* old = p.uiWindow_;
  requestMode = abort ? Unusable : Success;
  requestGate.Arm();
  Gate& terminal = abort ? abortGate : flushGate;
  terminal.Arm();
  destroyGate.Arm();
  if (replace) createGate.Arm();

  f.Feed(11);
  requestGate.Wait("preview never entered RequestBuffer");
  UiLocked(p);
  f.Feed(22);
  f.Feed(33);
  {
    std::lock_guard<std::mutex> lock(p.presentMu_);
    Check(p.presentPending_ && p.presentFrame_ == std::vector<uint8_t>(24, 22),
          "blocked preview did not retain one pending frame and drop the next");
  }
  // No further preview iteration can own uiMu_ during the lifecycle probes.
  f.QuitPreview();
  Counter started, finished;
  std::thread lifecycle([&] {
    started.Signal();
    if (replace) Check(p.SetUiSurface(2), "replacement failed");
    else p.ClearUiSurface();
    finished.Signal();
  });
  started.Wait(1, "lifecycle worker did not start");
  Check(!old->destroyed && old->destroys == 0 && CreatedCount() == 1,
        "blocked RequestBuffer allowed window mutation");
  requestGate.Open();
  terminal.Wait("presentation never reached FlushBuffer/AbortBuffer");
  UiLocked(p);
  Check(!old->destroyed && old->destroys == 0 && CreatedCount() == 1,
        "window mutated before the borrowed buffer was returned");
  Check(old->buffer.native.maps == 1, "preview buffer did not use the mapping cache");
  terminal.Open();
  f.JoinPreview();

  if (replace) {
    createGate.Wait("replacement never reached native creation");
    UiLocked(p);  // Only the SetUiSurface worker can own uiMu_ now.
    Check(!old->destroyed && old->outstanding == 0, "replacement ran before presentation ended");
    createGate.Open();
  }
  destroyGate.Wait("clear/replace never reached native destruction");
  UiLocked(p);  // Preview is joined: catches a missing mutator lock deterministically.
  Check(old->outstanding == 0 && old->transform == 4, "cleanup did not restore the window");
  Check(old->flushes == (abort ? 0 : 1) && old->aborts == (abort ? 1 : 0),
        "presentation used the wrong buffer return path");
  f.CheckCachesClearedOnlyForPreview();
  f.CheckInactive();
  destroyGate.Open();
  finished.Wait(1, "lifecycle operation deadlocked");
  lifecycle.join();
  Check(old->destroyed && old->destroys == 1, "old window was not destroyed exactly once");
  Check(p.uiActive_.load() == replace, "incorrect active state after lifecycle operation");
  Check(p.uiSurfaceId_ == (replace ? 2 : 0), "incorrect surface after lifecycle operation");
  if (replace) Check(p.uiWindow_ != old && p.uiFailCount_ == 0 && !p.uiLogged_,
                     "replacement inherited stale preview state");
}

void ActiveFlag() {
  Fixture f;
  auto& p = f.p;
  auto* window = p.uiWindow_;
  f.QuitPreview();
  f.JoinPreview();
  std::lock_guard<std::mutex> uiLock(p.uiMu_);
  for (bool active : {false, true, false}) {
    p.uiActive_.store(active);
    f.Feed(active ? 22 : 11);
    std::lock_guard<std::mutex> lock(p.presentMu_);
    Check(p.presentPending_ == active,
          "compositor did not use the atomic activity flag independently of uiWindow_");
    if (active) Check(p.presentFrame_ == std::vector<uint8_t>(24, 22),
                      "active compositor did not queue the encoded frame");
    p.presentPending_ = false;
  }
  Check(p.uiWindow_ == window && window->requests == 0,
        "activity probe unexpectedly touched the native window");
}

void FailureCleanup() {
  Fixture f;
  auto& p = f.p;
  auto* old = p.uiWindow_;
  for (int failure = 1; failure <= 2; ++failure) {
    requestMode = failure == 1 ? Error : NullBuffer;
    requestGate.Arm();
    f.Feed(static_cast<uint8_t>(failure));
    requestGate.Wait("failed request did not enter native call");
    UiLocked(p);
    requestGate.Open();
    std::lock_guard<std::mutex> lock(p.uiMu_);
    Check(p.uiFailCount_ == failure && p.uiActive_.load() && !old->destroyed,
          "preview disabled before three consecutive request failures");
  }
  requestMode = Error;
  requestGate.Arm();
  destroyGate.Arm();
  f.Feed(3);
  requestGate.Wait("third request never started");
  UiLocked(p);
  requestGate.Open();
  destroyGate.Wait("third failure did not disable the preview");
  UiLocked(p);  // No setter exists yet: failure cleanup itself must hold uiMu_.
  Check(p.uiFailCount_ == 3 && p.uiWindow_ == nullptr && p.uiSurfaceId_ == 0,
        "third failure did not clear the failed surface");
  f.CheckCachesClearedOnlyForPreview();
  f.CheckInactive();
  f.QuitPreview();

  requestMode = Success;
  createGate.Arm();
  Counter started, finished;
  std::thread setter([&] {
    started.Signal();
    Check(p.SetUiSurface(2), "replacement after failure failed");
    finished.Signal();
  });
  started.Wait(1, "replacement worker did not start");
  Check(CreatedCount() == 1, "replacement entered during failure destruction");
  destroyGate.Open();
  f.JoinPreview();
  createGate.Wait("replacement remained blocked after failure cleanup");
  UiLocked(p);
  Check(old->destroyed && old->destroys == 1, "replacement preceded failure cleanup");
  createGate.Open();
  finished.Wait(1, "replacement after failure deadlocked");
  setter.join();
  auto* current = p.uiWindow_;
  Check(current && current != old && p.uiSurfaceId_ == 2 && p.uiActive_.load() &&
        p.uiFailCount_ == 0 && !p.uiLogged_, "failure cleanup clobbered the replacement");

  {
    std::lock_guard<std::mutex> lock(p.presentMu_);
    p.presentQuit_ = false;
  }
  requestGate.Arm();
  flushGate.Arm();
  p.previewThread_ = std::thread(&ipcam::OsdPipeline::PreviewLoop, &p);
  f.Feed(55);
  requestGate.Wait("replacement did not receive a frame");
  UiLocked(p);
  Check(current->requests == 1 && old->requests == 3, "frame used the failed window");
  requestGate.Open();
  flushGate.Wait("replacement did not flush its frame");
  UiLocked(p);
  f.QuitPreview();
  flushGate.Open();
  f.JoinPreview();
  Check(current->flushes == 1 && !current->destroyed && p.uiActive_.load(),
        "replacement did not survive the old window's failure");
}

int main(int argc, char** argv) {
  Check(argc == 2, "expected one test case name");
  std::string name = argv[1];
  if (name == "clear-flush") Lifetime(false, false);
  else if (name == "replace-flush") Lifetime(true, false);
  else if (name == "clear-abort") Lifetime(false, true);
  else if (name == "replace-abort") Lifetime(true, true);
  else if (name == "failure-cleanup") FailureCleanup();
  else if (name == "active-flag") ActiveFlag();
  else Check(false, "unknown test case");
  Check(outstanding == 0 && !creating && !destroying, "native operation leaked");
  for (const auto& window : windows)
    Check(window->destroyed && window->destroys == 1, "fixture leaked a native window");
  std::cout << "PASS " << name << std::endl;
}
"""


def build_harness(work, cpp_dir=CPP):
    """Compile unchanged production source; all scaffolding lives under work."""
    include = work / "include"
    include.mkdir()
    (include / "host_sdk.h").write_text(SDK, encoding="utf-8")
    for header in (
        "native_buffer/native_buffer.h",
        "native_image/native_image.h",
        "native_window/external_window.h",
    ):
        path = include / header
        path.parent.mkdir(exist_ok=True)
        path.write_text('#pragma once\n#include "host_sdk.h"\n', encoding="utf-8")
    log = include / "hilog" / "log.h"
    log.parent.mkdir()
    log.write_text("#pragma once\n#define OH_LOG_Print(...) ((void)0)\n", encoding="utf-8")
    if os.name == "nt":
        # Tests use fence=-1; accidentally exercising real descriptor I/O is an error.
        (include / "unistd.h").write_text(
            "#pragma once\n#include <cstdlib>\ninline int close(int) { std::abort(); }\n",
            encoding="utf-8",
        )
        (include / "poll.h").write_text(
            "#pragma once\n#include <cstdlib>\n"
            "struct pollfd { int fd; short events; short revents; };\n"
            "#define POLLIN 1\n"
            "inline int poll(pollfd*, unsigned long, int) { std::abort(); }\n",
            encoding="utf-8",
        )
    source = work / "preview_lifetime.cpp"
    source.write_text(HARNESS, encoding="utf-8")
    exe = work / ("preview_lifetime.exe" if os.name == "nt" else "preview_lifetime")
    cl = shutil.which("cl") if os.name == "nt" else None
    if cl or (os.name == "nt" and VCVARS.is_file()):
        command = [cl or "cl", "/nologo", "/std:c++17", "/EHsc", "/utf-8",
                   "/D_CRT_SECURE_NO_WARNINGS", f"/I{include}", f"/I{cpp_dir}",
                   str(source), f"/Fe:{exe}"]
        if not cl:
            batch = work / "build.cmd"
            batch.write_text(
                f'@echo off\ncall "{VCVARS}" >nul\nif errorlevel 1 exit /b %errorlevel%\n'
                + subprocess.list2cmdline(command) + "\nexit /b %errorlevel%\n",
                encoding="utf-8",
            )
            command = [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", str(batch)]
    else:
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("C++17 compiler required: install MSVC, clang++, or g++")
        command = [compiler, "-std=c++17", "-pthread", "-I", str(include), "-I",
                   str(cpp_dir), str(source), "-o", str(exe)]
    result = subprocess.run(command, cwd=work, capture_output=True, text=True,
                            errors="replace", timeout=120)
    if result.returncode:
        raise RuntimeError("Host harness compilation failed:\n" + result.stdout + result.stderr)
    return exe


class PreviewLifetimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ipcamera-preview-lifetime-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.exe = build_harness(cls.work)

    def run_case(self, name):
        try:
            result = subprocess.run([str(self.exe), name], cwd=self.work,
                                    capture_output=True, text=True, errors="replace", timeout=30)
        except subprocess.TimeoutExpired as exc:
            self.fail(f"{name}: native lock/deadlock watchdog expired: {exc}")
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("PASS " + name, output)

    def test_clear_waits_for_request_and_flush(self):
        self.run_case("clear-flush")

    def test_replace_waits_for_request_and_flush(self):
        self.run_case("replace-flush")

    def test_clear_waits_for_request_and_abort(self):
        self.run_case("clear-abort")

    def test_replace_waits_for_request_and_abort(self):
        self.run_case("replace-abort")

    def test_third_request_failure_serializes_cleanup_and_replacement(self):
        self.run_case("failure-cleanup")

    def test_compositor_uses_atomic_flag_without_ui_lock(self):
        self.run_case("active-flag")


if __name__ == "__main__":
    unittest.main()
