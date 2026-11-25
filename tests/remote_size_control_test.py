"""Host regression for ServerHolder remote size control. Run: python tests/remote_size_control_test.py -v

Compile the production control methods, HTTP callbacks, motion start, event posting
and authoritative N-API state query. Only camera/outputs, transport and SDK calls
are doubles. No device, sockets or Harmony SDK; all build products are temporary.
"""
import json
from pathlib import Path
import shutil
import tempfile
import unittest

from motion_state_test import compile_native, definition, run

CPP = Path(__file__).resolve().parents[1] / 'entry/src/main/cpp'


SDK = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "http_server.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <utility>
namespace ipcam {
// @EVENT_ENUM@
struct DualCameraPlan {};
struct CameraStreamer {
  int layout = 0;
  int DualLayout() const { return layout; }
  // @RESIZE_ENUM@
  ResizeResult SetDualLayout(int nextLayout, const DualCameraPlan* plan) {
    assert(!nextLayout || plan);
    if (next == ResizeResult::Changed) layout = nextLayout;
    if (next == ResizeResult::Stopped) running = false;
    return next;
  }
  ResizeResult next = ResizeResult::Changed;
  bool running = true;
  int width = 640, height = 480;
  std::vector<std::pair<int,int>> sizes{{1280,720},{640,480},{640,360}};
  std::vector<std::pair<int,int>> attempts;
  std::function<void()> duringResize;
  std::function<void(int,int)> format;
  bool IsRunning() const { return running; }
  int CaptureWidth() const { return running ? width : 0; }
  int CaptureHeight() const { return running ? height : 0; }
  auto AvailableResolutions() const { return sizes; }
  void SetVideoFormatCallback(std::function<void(int,int)> f) { format = f; }
  ResizeResult ChangeResolution(int w, int h) {
    attempts.emplace_back(w,h);
    if (duringResize) duringResize();
    if (next == ResizeResult::Changed) { width = w; height = h; }
    if (next == ResizeResult::Changed || next == ResizeResult::Restored) format(width,height);
    if (next == ResizeResult::Stopped) running = false;
    return next;
  }
};
struct RtspServer {
  bool occupied = false, reserved = false;
  int tries = 0, ends = 0, resets = 0;
  bool TryBeginVideoChange() {
    ++tries;
    if (occupied || reserved) return false;
    return reserved = true;
  }
  void EndVideoChange() { assert(reserved); reserved = false; ++ends; }
  int ClientCount() const { return occupied ? 1 : 0; }
  void ResetVideoParams(bool hevc) { assert(hevc); ++resets; }
};
struct FlvStream {
  bool occupied = false, reserved = false;
  bool TryBeginVideoChange() { return !occupied && !reserved && (reserved = true); }
  void EndVideoChange() { assert(reserved); reserved = false; }
  void ResetVideoFormat(bool) {}
};
struct WebStream {
  bool armed = false;
  int resets = 0;
  std::function<void()> beforeEnable;
  bool MotionEnabled() const { return armed; }
  bool MotionActive() const { return false; } // armed, but no hit
  void SetMotionEnabled(bool b) { if (beforeEnable) beforeEnable(); armed = b; }
  void SetMotionTimeout(int) {}
  void SetMotionSink(std::function<void(bool)>) {}
  void ResetVideoInput() { ++resets; }
};
struct Snapshot {
  bool busy = false;
  int resets = 0;
  bool IsBusy() const { return busy; }
  void ResetVideoInput() { ++resets; }
};
struct HttpCallbacks {
  HttpServer::SizeSource list;
  HttpServer::SizeSink select;
  void SetSizeCallbacks(HttpServer::SizeSource l, HttpServer::SizeSink s) { list = l; select = s; }
};
}

struct ServerHolder;
struct Env { ServerHolder* holder; std::string text; };
using napi_env = Env*;
using napi_value = void*;
using napi_callback_info = void*;
void napi_get_cb_info(napi_env e, napi_callback_info, size_t* argc, napi_value* argv, void*, void*) {
  *argc = std::min<size_t>(*argc, 2); argv[0] = e->holder;
  if (*argc == 2) argv[1] = e;  // true
}
ServerHolder* HolderFromArg(napi_env, napi_value v) { return static_cast<ServerHolder*>(v); }
napi_value InvalidArgs(napi_env) { std::abort(); }
void napi_get_value_bool(napi_env, napi_value v, bool* b) { *b = v != nullptr; }
void napi_get_value_double(napi_env, napi_value, double* d) { *d = 15; }
void napi_get_boolean(napi_env e, bool b, napi_value* v) { *v = b ? e : nullptr; }
void napi_create_string_utf8(napi_env e, const char* s, size_t n, napi_value* v) {
  e->text.assign(s,n); *v = e;
}
// @EVENT_DATA@
void PostEvent(ServerHolder*, ipcam::RtspEvent, int, const std::string&);
struct Notice { StatusEventData data; uint64_t generation; bool running; int width,height; };
struct ServerHolder {
  ipcam::RtspServer server;
  std::shared_ptr<ipcam::FlvStream> flv = std::make_shared<ipcam::FlvStream>();
  std::unique_ptr<ipcam::CameraStreamer> camera;
  std::unique_ptr<ipcam::Snapshot> snapshot = std::make_unique<ipcam::Snapshot>();
  std::unique_ptr<ipcam::WebStream> webStream = std::make_unique<ipcam::WebStream>();
  std::unique_ptr<ipcam::HttpCallbacks> http = std::make_unique<ipcam::HttpCallbacks>();
  std::mutex cameraMu, mediaControlMu, outputsMu, tsfnMu;
  uint64_t remoteSizeGeneration = 0;
  std::shared_ptr<int> rtmp, pendingRtmp, recorder;
  struct Size { int width, height; };
  std::atomic<Size> encodedSize{Size{640, 480}};
  std::atomic<bool> videoIsH265{true}, webParamsInjected{true};
  ServerHolder* tsfn = this;
  std::vector<Notice> notices;
  // @METHODS@
};
constexpr int napi_ok = 0, napi_tsfn_nonblocking = 1;
int napi_call_threadsafe_function(ServerHolder* h, void* raw, int mode) {
  assert(mode == napi_tsfn_nonblocking);
  std::unique_ptr<StatusEventData> e(static_cast<StatusEventData*>(raw));
  h->notices.push_back({*e,h->remoteSizeGeneration,h->camera->IsRunning(),
                        h->camera->CaptureWidth(),h->camera->CaptureHeight()});
  return napi_ok;
}
// @FUNCTIONS@
void wireHttp(ServerHolder* holder) {
  auto* weakHolder = holder;
  // @WIRE_HTTP@
}
void wireFormat(ServerHolder* holder) {
  auto& camera = holder->camera;
  const char* mime = "video/hevc";
  // @WIRE_FORMAT@
}
'''

CASES = r'''
using Result = ipcam::HttpServer::SizeResult;
using Resize = ipcam::CameraStreamer::ResizeResult;
int main() {
  ServerHolder h;
  Env e{&h,{}};
  wireHttp(&h);
  auto& select = h.http->select;
  ipcam::HttpServer::SizeList list;
  auto state = [&] {
    assert(CameraRemoteSizeState(&e,nullptr) == &e);
    std::cout << e.text << '\n'; // parsed as JSON, independent of key order/spacing
  };
  auto event = [&](size_t n, const char* detail, bool running, int w, int height) {
    assert(h.notices.size() == n && h.remoteSizeGeneration == n);
    const auto& notice = h.notices.back();
    assert(notice.data.event == 4 && notice.data.clients == h.server.ClientCount());
    assert(notice.data.detail == detail && notice.generation == n);
    assert(notice.running == running && notice.width == w && notice.height == height);
    assert(h.encodedSize.load().width == w && h.encodedSize.load().height == height && h.videoIsH265 && !h.webParamsInjected);
    assert(h.webStream->resets == int(n) && h.snapshot->resets == int(n) && h.server.resets == int(n));
    assert(!h.server.reserved);
    state();
  };
  assert(h.http->list(list) == Result::CameraClosed && select(0) == Result::CameraClosed);
  state();
  h.camera = std::make_unique<ipcam::CameraStreamer>();
  auto& cam = *h.camera;
  wireFormat(&h);
  cam.running = false;
  assert(h.http->list(list) == Result::CameraClosed && select(1) == Result::CameraClosed);
  cam.running = true;
  assert(h.http->list(list) == Result::Ok && list.sizes.size() == 3 && list.currentIndex == 1);
  for (size_t i = 0; i < cam.sizes.size(); ++i)
    assert(std::make_pair(list.sizes[i].width,list.sizes[i].height) == cam.sizes[i]);
  auto sizes = cam.sizes;
  cam.sizes = {{640,360}};
  assert(h.http->list(list) == Result::Ok && list.sizes.size() == 1 && list.currentIndex == -1);
  cam.sizes.clear();
  assert(h.http->list(list) == Result::CameraClosed && list.sizes.empty());
  cam.sizes = sizes;
  assert(select(-1) == Result::NotFound && select(3) == Result::NotFound);

  // Enter the real N-API motion-start path. While it is starting (not yet
  // armed), an HTTP selection on another thread must immediately return Busy.
  h.webStream->beforeEnable = [&] {
    assert(!h.webStream->MotionEnabled());
    std::thread request([&] { assert(select(0) == Result::Busy); });
    request.join();
  };
  assert(RtspSetMotion(&e,nullptr) == &e);
  assert(h.webStream->armed && !h.webStream->MotionActive());
  assert(select(1) == Result::Ok && select(0) == Result::Busy);
  h.webStream->armed = false;
  for (auto* slot : {&h.pendingRtmp,&h.rtmp,&h.recorder}) {
    *slot = std::make_shared<int>(1);
    assert(select(0) == Result::Busy && select(1) == Result::Ok);
    slot->reset();
  }
  h.snapshot->busy = true;
  assert(select(0) == Result::Busy && select(1) == Result::Ok);
  h.snapshot->busy = false;
  assert(h.server.tries == 0);
  h.server.occupied = true;
  assert(select(0) == Result::Busy && select(1) == Result::Ok);
  h.server.occupied = false;
  assert(h.server.tries == 1 && h.server.ends == 0 && cam.attempts.empty());
  assert(h.notices.empty() && h.remoteSizeGeneration == 0 && h.webParamsInjected);
  state();

  cam.duringResize = [&] {
    assert(h.server.reserved);
    // Do not recursively probe a mutex from its owning thread.
    std::thread probe([&] { assert(!h.mediaControlMu.try_lock()); });
    probe.join();
  };
  cam.next = Resize::Rejected;
  assert(select(0) == Result::NotFound && !h.server.reserved);
  cam.next = Resize::Unchanged;
  assert(select(0) == Result::Ok && !h.server.reserved);
  assert(h.notices.empty() && h.remoteSizeGeneration == 0 && h.webParamsInjected);
  cam.next = Resize::Changed;
  assert(select(0) == Result::Ok);
  event(1,"resolution changed",true,1280,720);
  assert(h.http->list(list) == Result::Ok && list.currentIndex == 0);
  assert(select(0) == Result::Ok && h.notices.size() == 1 && cam.attempts.size() == 3);
  cam.next = Resize::Restored;
  h.webParamsInjected = true;
  assert(select(2) == Result::RestartFailed);
  event(2,"previous resolution restored",true,1280,720);
  assert(h.http->list(list) == Result::Ok && list.currentIndex == 0);
  cam.next = Resize::Stopped;
  h.webParamsInjected = true;
  assert(select(1) == Result::RestartFailed);
  event(3,"camera stopped",false,0,0);
  assert(h.http->list(list) == Result::CameraClosed && select(1) == Result::CameraClosed);
  assert(h.remoteSizeGeneration == 3 && h.notices.size() == 3);
  const decltype(cam.attempts) wanted{sizes[0],sizes[0],sizes[0],sizes[2],sizes[1]};
  assert(cam.attempts == wanted && h.server.ends == 5 && h.server.tries == 6);

  ipcam::DualCameraPlan plan;
  assert(h.ChangeDualLayout(1, &plan) == 3); // stopped
  cam.running = true;
  cam.next = Resize::Changed;
  h.flv->occupied = true;
  assert(h.ChangeDualLayout(1, &plan) == 1 && !h.server.reserved);
  h.flv->occupied = false;
  h.webStream->armed = true;
  assert(h.ChangeDualLayout(1, &plan) == 1 && !cam.layout);
  h.webStream->armed = false;
  assert(h.ChangeDualLayout(1, &plan) == 0 && cam.layout == 1);
  assert(h.http->list(list) == Result::Busy && select(-1) == Result::Busy);
  cam.next = Resize::Rejected;
  assert(h.ChangeDualLayout(2, &plan) == 1);
  cam.next = Resize::Restored;
  assert(h.ChangeDualLayout(2, &plan) == 2 && cam.layout == 1);
  cam.next = Resize::Unchanged;
  assert(h.ChangeDualLayout(1, &plan) == 0);
  cam.next = Resize::Stopped;
  assert(h.ChangeDualLayout(0, nullptr) == 3 && h.encodedSize.load().width == 0);
  assert(!h.flv->reserved && !h.server.reserved);
}
'''


def callback_setup(source, marker):
    # The call ends on the last callback's closing line. Keep its capture list
    # and body verbatim so a disconnected/misrouted production hook fails here.
    assert source.count(marker) == 1, marker
    start = source.index(marker)
    return source[start:source.index('});', start) + 3]


class RemoteSizeControlTest(unittest.TestCase):
    def test_host_controls(self):
        napi = (CPP / 'napi_init.cpp').read_text(encoding='utf-8')
        source = SDK.replace('// @EVENT_ENUM@', definition(
            (CPP / 'rtsp_server.h').read_text(encoding='utf-8'), 'enum class RtspEvent') + ';')
        source = source.replace('// @RESIZE_ENUM@', definition(
            (CPP / 'camera_streamer.h').read_text(encoding='utf-8'), 'enum class ResizeResult') + ';')
        source = source.replace('// @EVENT_DATA@', definition(napi, 'struct StatusEventData {') + ';')
        source = source.replace('// @METHODS@', definition(napi, 'struct VideoChangeLease {') + ';\n' +
                                definition(napi, 'bool MediaOutputInUse(') + '\n' +
                                '\n'.join(definition(napi, marker) for marker in (
            'void ResetVideoFormat(', 'ipcam::HttpServer::SizeResult RemoteSizeList(',
            'ipcam::HttpServer::SizeResult SelectRemoteSize(', 'int ChangeDualLayout(')))
        # PostEvent also has a forward declaration before the holder definition.
        source = source.replace('// @FUNCTIONS@', '\n'.join((
            definition(napi[napi.index('struct ServerHolder {'):], 'void PostEvent('),
            definition(napi, 'napi_value RtspSetMotion('), definition(napi, 'napi_value CameraRemoteSizeState('))))
        source = source.replace('// @WIRE_HTTP@', callback_setup(napi, 'holder->http->SetSizeCallbacks('))
        source = source.replace('// @WIRE_FORMAT@', callback_setup(napi, 'camera->SetVideoFormatCallback('))
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            shutil.copyfile(CPP / 'http_server.h', directory / 'http_server.h')
            cpp = directory / 'controls.cpp'
            cpp.write_text(source + CASES, encoding='utf-8')
            states = run([str(compile_native(cpp, directory))], directory, timeout=15).splitlines()
        self.assertEqual([json.loads(line) for line in states], [
            dict(generation=g, running=r, width=w, height=h)
            for g, r, w, h in [(0, False, 0, 0), (0, True, 640, 480), (1, True, 1280, 720),
                               (2, True, 1280, 720), (3, False, 0, 0)]])


if __name__ == '__main__':
    unittest.main()
