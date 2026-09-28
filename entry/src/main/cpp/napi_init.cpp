#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <multimedia/image_framework/image/pixelmap_native.h>
#include <native_window/external_window.h>
#include <napi/native_api.h>

#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>

#include "camera_streamer.h"
#include "http_server.h"
#include "media_time.h"
#include "mic_streamer.h"
#include "osd_state.h"
#include "recorder.h"
#include "rtmp_publisher.h"
#include "rtsp_server.h"
#include "snapshot.h"
#include "test_source.h"
#include "voice_player.h"
#include "web_stream.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xC010
#define LOG_TAG "StreamingNapi"

namespace {

constexpr int kTsfnQueueSize = 16;

struct StatusEventData {
  int event = 0;
  int clients = 0;
  std::string detail;
  std::shared_ptr<std::atomic<uint64_t>> generation;
  uint64_t expectedGeneration = 0;
};

struct ServerHolder;
void PostEvent(ServerHolder* holder, ipcam::RtspEvent event, int clients,
               const std::string& detail);
ServerHolder* g_holder = nullptr;  // single-server app; used by XComponent callbacks
std::mutex g_holderMu;
// XComponent surfaces are created at page load, usually before the camera
// starts; remember the id and attach it when (or after) the camera starts.
std::atomic<uint64_t> g_pendingPreviewId{0};

struct ServerHolder {
  ipcam::RtspServer server;
  std::unique_ptr<ipcam::TestPatternSource> pattern;
  std::unique_ptr<ipcam::CameraStreamer> camera;
  std::mutex cameraMu;
  std::unique_ptr<ipcam::MicStreamer> mic;
  std::shared_ptr<ipcam::RtmpPublisher> rtmp;
  std::shared_ptr<ipcam::RtmpPublisher> pendingRtmp;
  std::shared_ptr<ipcam::StreamRecorder> recorder;
  std::mutex outputsMu;
  std::shared_ptr<std::atomic<uint64_t>> rtmpGeneration =
      std::make_shared<std::atomic<uint64_t>>(0);
  ipcam::RelativeTimestampMapper patternClock;
  ipcam::RelativeTimestampMapper audioClock;
  std::unique_ptr<ipcam::Snapshotter> snapshot = std::make_unique<ipcam::Snapshotter>();
  std::unique_ptr<ipcam::WebStream> webStream = std::make_unique<ipcam::WebStream>();
  std::unique_ptr<ipcam::HttpServer> http;
  ipcam::VoicePlayer voice;
  std::string filesDir;
  std::chrono::steady_clock::time_point startWall = std::chrono::steady_clock::now();
  std::atomic<bool> torchOn{false};
  napi_threadsafe_function tsfn = nullptr;
  std::mutex tsfnMu;
  std::atomic<int> streamWidth{1280};
  std::atomic<int> streamHeight{720};
  std::atomic<bool> videoIsH265{false};  // last camera mime, used as RTMP hint
  std::atomic<bool> webParamsInjected{false};
  bool httpWebStreamActive = false;

  void PushVideo(const uint8_t* data, size_t size, ipcam::TimestampUs tsUs) {
    if (tsUs == ipcam::kNoTimestampUs) tsUs = ipcam::NowMonotonicUs();
    if (tsUs < 0) return;
    server.PushH264(data, size, tsUs);
    // The RTSP server caches codec params from the first frame; inject them
    // into the web decoder lazily once they are available.
    if (!webParamsInjected.load()) {
      auto vp = server.GetVideoParams();
      if (vp.ready) {
        std::vector<uint8_t> annexB;
        const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
        auto append = [&annexB, &sc](const std::vector<uint8_t>& nal) {
          if (nal.empty()) return;
          annexB.insert(annexB.end(), sc, sc + 4);
          annexB.insert(annexB.end(), nal.begin(), nal.end());
        };
        if (vp.h265) append(vp.vps);
        append(vp.sps);
        append(vp.pps);
        webStream->SetParams(vp.h265, annexB, streamWidth.load(), streamHeight.load());
        webParamsInjected.store(true);
      }
    }
    webStream->FeedVideo(data, size);
    snapshot->FeedVideo(data, size);

    std::shared_ptr<ipcam::RtmpPublisher> rtmpSnapshot;
    std::shared_ptr<ipcam::StreamRecorder> recorderSnapshot;
    {
      std::lock_guard<std::mutex> lk(outputsMu);
      rtmpSnapshot = rtmp;
      recorderSnapshot = recorder;
    }
    if (rtmpSnapshot) rtmpSnapshot->SendH264Packet(data, size, tsUs);
    if (recorderSnapshot) recorderSnapshot->FeedVideo(data, size, tsUs);
  }

  void PushAudio(const uint8_t* data, size_t size, ipcam::TimestampUs tsUs) {
    if (tsUs == ipcam::kNoTimestampUs) tsUs = ipcam::NowMonotonicUs();
    if (tsUs < 0) return;
    server.PushAdts(data, size, tsUs);

    std::shared_ptr<ipcam::RtmpPublisher> rtmpSnapshot;
    std::shared_ptr<ipcam::StreamRecorder> recorderSnapshot;
    {
      std::lock_guard<std::mutex> lk(outputsMu);
      rtmpSnapshot = rtmp;
      recorderSnapshot = recorder;
    }
    if (rtmpSnapshot) rtmpSnapshot->SendAdtsPacket(data, size, tsUs);
    if (recorderSnapshot) recorderSnapshot->FeedAudio(data, size, tsUs);
  }

  void StopCameraOutput() {
    std::unique_ptr<ipcam::CameraStreamer> current;
    {
      std::lock_guard<std::mutex> lk(cameraMu);
      current = std::move(camera);
    }
    if (current) current->Stop();
  }

  int CameraOrientation() {
    std::lock_guard<std::mutex> lk(cameraMu);
    return camera ? camera->Orientation() : 0;
  }

  bool CameraIsRunning() {
    std::lock_guard<std::mutex> lk(cameraMu);
    return camera && camera->IsRunning();
  }

  bool RestartCamera() {
    std::lock_guard<std::mutex> lk(cameraMu);
    return camera && camera->Restart();
  }

  bool AttachCameraPreview(uint64_t surfaceId) {
    std::lock_guard<std::mutex> lk(cameraMu);
    return camera && camera->AttachPreview(surfaceId);
  }

  void HandleCameraCommand(const std::string& command) {
    std::lock_guard<std::mutex> lk(cameraMu);
    if (!camera) return;
    if (command == "light") {
      bool nextTorch = !torchOn.load();
      torchOn.store(nextTorch);
      camera->SetTorch(nextTorch);
      PostEvent(this, ipcam::RtspEvent::kClientPlaying, 0,
                std::string("torch ") + (nextTorch ? "on" : "off"));
    } else if (command == "camswitch") {
      camera->SwitchFacing();
      PostEvent(this, ipcam::RtspEvent::kClientPlaying, 0, "camera switched");
    }
  }

  void StopRtmpOutputs() {
    std::shared_ptr<ipcam::RtmpPublisher> active;
    std::shared_ptr<ipcam::RtmpPublisher> pending;
    {
      std::lock_guard<std::mutex> lk(outputsMu);
      rtmpGeneration->fetch_add(1);
      active = std::move(rtmp);
      pending = std::move(pendingRtmp);
    }
    if (pending && pending != active) pending->Cancel();
    if (active) active->Stop();
  }

  void StopRecorderOutput() {
    std::shared_ptr<ipcam::StreamRecorder> current;
    {
      std::lock_guard<std::mutex> lk(outputsMu);
      current = std::move(recorder);
    }
    if (current) current->Stop();
  }
};

void PostEvent(ServerHolder* holder, ipcam::RtspEvent event, int clients,
               const std::string& detail) {
  auto* ev = new StatusEventData{static_cast<int>(event), clients, detail, nullptr, 0};
  std::lock_guard<std::mutex> lk(holder->tsfnMu);
  if (holder->tsfn == nullptr ||
      napi_call_threadsafe_function(holder->tsfn, ev, napi_tsfn_nonblocking) != napi_ok) {
    delete ev;
  }
}

void PostRtmpEvent(ServerHolder* holder, uint64_t generation,
                   const std::string& detail) {
  auto* ev = new StatusEventData{static_cast<int>(ipcam::RtspEvent::kServerError),
                                 holder->server.ClientCount(), detail,
                                 holder->rtmpGeneration, generation};
  std::lock_guard<std::mutex> lk(holder->tsfnMu);
  if (holder->tsfn == nullptr ||
      napi_call_threadsafe_function(holder->tsfn, ev, napi_tsfn_nonblocking) != napi_ok) {
    delete ev;
  }
}

void CallJsStatus(napi_env env, napi_value jsCb, void* /*context*/, void* data) {
  auto* ev = static_cast<StatusEventData*>(data);
  if (ev == nullptr) return;
  if (ev->generation != nullptr &&
      ev->generation->load() != ev->expectedGeneration) {
    delete ev;
    return;
  }
  if (env == nullptr || jsCb == nullptr) {
    delete ev;
    return;
  }
  napi_value args[3];
  napi_create_int32(env, ev->event, &args[0]);
  napi_create_int32(env, ev->clients, &args[1]);
  napi_create_string_utf8(env, ev->detail.c_str(), ev->detail.size(), &args[2]);
  napi_value undef;
  napi_get_undefined(env, &undef);
  napi_value result;
  napi_call_function(env, undef, jsCb, 3, args, &result);
  delete ev;
}

void ServerFinalize(napi_env /*env*/, void* data, void* /*hint*/) {
  auto* holder = static_cast<ServerHolder*>(data);
  if (holder == nullptr) return;
  {
    std::lock_guard<std::mutex> lk(g_holderMu);
    if (g_holder == holder) g_holder = nullptr;
  }
  if (holder->http) {
    holder->http->Stop();
    holder->http.reset();
  }
  if (holder->httpWebStreamActive) {
    holder->webStream->DecClients();
    holder->httpWebStreamActive = false;
  }
  if (holder->pattern) {
    holder->pattern->Stop();
    holder->pattern.reset();
  }
  holder->StopCameraOutput();
  if (holder->mic) {
    holder->mic->Stop();
    holder->mic.reset();
  }
  holder->StopRtmpOutputs();
  holder->StopRecorderOutput();
  holder->snapshot->Stop();
  holder->voice.Stop();
  holder->server.Stop();
  {
    std::lock_guard<std::mutex> lk(holder->tsfnMu);
    if (holder->tsfn != nullptr) {
      napi_release_threadsafe_function(holder->tsfn, napi_tsfn_release);
      holder->tsfn = nullptr;
    }
  }
  delete holder;
}

// All rtsp* functions take the opaque server handle as argv[0] (free function
// call style from ArkTS). On the ArkTS runtime an object made with
// napi_create_external surfaces as napi_external and napi_unwrap on it THROWS
// (observed: napi_pending_exception) — read the pointer with
// napi_get_value_external; napi_unwrap stays as fallback only for plain
// objects wrapped with napi_wrap.
ServerHolder* HolderFromArg(napi_env env, napi_value arg) {
  if (arg == nullptr) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "HolderFromArg: arg is null");
    return nullptr;
  }
  napi_valuetype type = napi_undefined;
  napi_typeof(env, arg, &type);
  if (type == napi_external) {
    void* data = nullptr;
    napi_status st = napi_get_value_external(env, arg, &data);
    if (st == napi_ok && data != nullptr) {
      return static_cast<ServerHolder*>(data);
    }
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                 "HolderFromArg: get_value_external=%{public}d", static_cast<int>(st));
    return nullptr;
  }
  ServerHolder* holder = nullptr;
  napi_status st = napi_unwrap(env, arg, reinterpret_cast<void**>(&holder));
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "HolderFromArg: type=%{public}d unwrap=%{public}d holder=%{public}p",
               static_cast<int>(type), static_cast<int>(st), holder);
  return holder;
}

napi_value InvalidArgs(napi_env env) {
  napi_throw_error(env, nullptr, "invalid args");
  return nullptr;
}

napi_value CreateServer(napi_env env, napi_callback_info info) {
  (void)info;
  auto* holder = new ServerHolder();
  {
    std::lock_guard<std::mutex> lk(g_holderMu);
    g_holder = holder;
  }
  napi_value out = nullptr;
  napi_status st = napi_create_external(env, holder, ServerFinalize, nullptr, &out);
  if (st != napi_ok) {
    delete holder;
    napi_throw_error(env, nullptr, "create external failed");
    return nullptr;
  }
  return out;
}

napi_value RtspStart(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);

  ipcam::RtspConfig cfg;
  napi_value v = nullptr;
  double port = 8554;
  if (napi_get_named_property(env, argv[1], "port", &v) == napi_ok && v != nullptr) {
    napi_get_value_double(env, v, &port);
  }
  cfg.port = static_cast<int>(port);
  char buf[256];
  size_t len = 0;
  if (napi_get_named_property(env, argv[1], "path", &v) == napi_ok && v != nullptr) {
    if (napi_get_value_string_utf8(env, v, buf, sizeof(buf), &len) == napi_ok) {
      cfg.path.assign(buf, len);
    }
  }
  if (napi_get_named_property(env, argv[1], "user", &v) == napi_ok && v != nullptr) {
    if (napi_get_value_string_utf8(env, v, buf, sizeof(buf), &len) == napi_ok) {
      cfg.user.assign(buf, len);
    }
  }
  if (napi_get_named_property(env, argv[1], "password", &v) == napi_ok && v != nullptr) {
    if (napi_get_value_string_utf8(env, v, buf, sizeof(buf), &len) == napi_ok) {
      cfg.password.assign(buf, len);
    }
  }

  bool ok = holder->server.Start(cfg, [holder](ipcam::RtspEvent event, int clients,
                                               const std::string& detail) {
    PostEvent(holder, event, clients, detail);
  });
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

napi_value RtspStop(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder != nullptr) {
    holder->server.Stop();
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspIsRunning(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  napi_value result;
  napi_get_boolean(env, holder != nullptr && holder->server.IsRunning(), &result);
  return result;
}

napi_value RtspClientCount(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  napi_value result;
  napi_create_int32(env, holder != nullptr ? holder->server.ClientCount() : 0, &result);
  return result;
}

bool GetBufferArg(napi_env env, napi_value value, const uint8_t** data, size_t* size) {
  bool isTyped = false;
  if (napi_is_typedarray(env, value, &isTyped) == napi_ok && isTyped) {
    napi_typedarray_type tt = napi_uint8_array;
    size_t offset = 0;
    napi_value unused = nullptr;
    void* ptr = nullptr;
    if (napi_get_typedarray_info(env, value, &tt, size, &ptr, &unused, &offset) == napi_ok) {
      *data = static_cast<const uint8_t*>(ptr);
      return true;
    }
    return false;
  }
  bool isArrayBuffer = false;
  if (napi_is_arraybuffer(env, value, &isArrayBuffer) == napi_ok && isArrayBuffer) {
    void* ptr = nullptr;
    if (napi_get_arraybuffer_info(env, value, &ptr, size) == napi_ok) {
      *data = static_cast<const uint8_t*>(ptr);
      return true;
    }
  }
  return false;
}

napi_value RtspSendH264(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 3) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  const uint8_t* ptr = nullptr;
  size_t size = 0;
  if (!GetBufferArg(env, argv[1], &ptr, &size)) {
    napi_throw_type_error(env, nullptr, "expect ArrayBuffer or Uint8Array");
    return nullptr;
  }
  double tsUs = 0;
  napi_get_value_double(env, argv[2], &tsUs);
  if (!std::isfinite(tsUs) || tsUs < 0) {
    napi_throw_range_error(env, nullptr, "timestamp must be a finite non-negative number");
    return nullptr;
  }
  ipcam::TimestampUs timestamp = tsUs == 0 ? ipcam::NowMonotonicUs()
                                             : static_cast<ipcam::TimestampUs>(tsUs);
  holder->PushVideo(ptr, size, timestamp);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspSendAdts(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 3) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  const uint8_t* ptr = nullptr;
  size_t size = 0;
  if (!GetBufferArg(env, argv[1], &ptr, &size)) {
    napi_throw_type_error(env, nullptr, "expect ArrayBuffer or Uint8Array");
    return nullptr;
  }
  double tsUs = 0;
  napi_get_value_double(env, argv[2], &tsUs);
  if (!std::isfinite(tsUs) || tsUs < 0) {
    napi_throw_range_error(env, nullptr, "timestamp must be a finite non-negative number");
    return nullptr;
  }
  ipcam::TimestampUs timestamp = tsUs == 0 ? ipcam::NowMonotonicUs()
                                             : static_cast<ipcam::TimestampUs>(tsUs);
  holder->PushAudio(ptr, size, timestamp);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspSetStatusCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
               "setStatusCallback: argc=%{public}zu", argc);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);

  napi_value name = nullptr;
  napi_create_string_utf8(env, "rtsp_status", NAPI_AUTO_LENGTH, &name);
  napi_threadsafe_function tsfn = nullptr;
  napi_status st = napi_create_threadsafe_function(env, argv[1], nullptr, name,
                                                   kTsfnQueueSize, 1, nullptr, nullptr,
                                                   nullptr, CallJsStatus, &tsfn);
  if (st != napi_ok) {
    napi_throw_error(env, nullptr, "create threadsafe function failed");
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(holder->tsfnMu);
    if (holder->tsfn != nullptr) {
      napi_release_threadsafe_function(holder->tsfn, napi_tsfn_release);
    }
    holder->tsfn = tsfn;
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspClearStatusCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  {
    std::lock_guard<std::mutex> lk(holder->tsfnMu);
    if (holder->tsfn != nullptr) {
      napi_release_threadsafe_function(holder->tsfn, napi_tsfn_abort);
      holder->tsfn = nullptr;
    }
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspStartTestPattern(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 5) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  double width = 640, height = 480, fps = 15, bitrate = 1500000;
  napi_get_value_double(env, argv[1], &width);
  napi_get_value_double(env, argv[2], &height);
  napi_get_value_double(env, argv[3], &fps);
  napi_get_value_double(env, argv[4], &bitrate);

  if (holder->pattern) {
    holder->pattern->Stop();
    holder->pattern.reset();
  }
  holder->patternClock.Reset();
  auto pattern = std::make_unique<ipcam::TestPatternSource>();
  bool ok = pattern->Start(
      static_cast<int>(width), static_cast<int>(height), static_cast<int>(fps),
      static_cast<int>(bitrate),
      [holder](const uint8_t* data, size_t size, ipcam::TimestampUs ptsUs) {
        holder->PushVideo(data, size, holder->patternClock.Map(ptsUs));
      },
      [holder](const std::string& err) {
        PostEvent(holder, ipcam::RtspEvent::kServerError, holder->server.ClientCount(), err);
      });
  if (ok) {
    holder->pattern = std::move(pattern);
  }
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

napi_value RtspStopTestPattern(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder != nullptr && holder->pattern) {
    holder->pattern->Stop();
    holder->pattern.reset();
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspStartCamera(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 7) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  double width = 1280, height = 720, bitrate = 2000000;
  napi_get_value_double(env, argv[1], &width);
  napi_get_value_double(env, argv[2], &height);
  napi_get_value_double(env, argv[3], &bitrate);
  char mime[32] = "video/avc";
  size_t mimeLen = 0;
  napi_get_value_string_utf8(env, argv[4], mime, sizeof(mime), &mimeLen);
  bool front = false;
  napi_get_value_bool(env, argv[5], &front);
  double iFrameMs = 2000;
  napi_get_value_double(env, argv[6], &iFrameMs);

  if (holder->pattern) {
    holder->pattern->Stop();
    holder->pattern.reset();
  }
  holder->StopCameraOutput();
  holder->streamWidth.store(static_cast<int>(width));
  holder->streamHeight.store(static_cast<int>(height));
  holder->videoIsH265.store(std::string(mime) == "video/hevc");
  holder->webParamsInjected.store(false);
  auto camera = std::make_unique<ipcam::CameraStreamer>();
  bool ok = camera->Start(
      static_cast<int>(width), static_cast<int>(height), static_cast<int>(bitrate),
      mime, front, static_cast<int>(iFrameMs), g_pendingPreviewId.load(),
      ipcam::OsdEnabled(),
      [holder](const uint8_t* data, size_t size, ipcam::TimestampUs /*ptsUs*/) {
        // Surface-mode encoder PTS is device-dependent; sample the shared clock
        // once so every downstream consumer receives the same timestamp.
        holder->PushVideo(data, size, ipcam::NowMonotonicUs());
      },
      [holder](const std::string& err) {
        PostEvent(holder, ipcam::RtspEvent::kServerError, holder->server.ClientCount(), err);
      });
  if (ok) {
    {
      std::lock_guard<std::mutex> lk(holder->cameraMu);
      holder->camera = std::move(camera);
    }
    auto vp = holder->server.GetVideoParams();
    if (vp.ready) {
      std::vector<uint8_t> annexB;
      const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
      auto append = [&annexB, &sc](const std::vector<uint8_t>& nal) {
        if (nal.empty()) return;
        annexB.insert(annexB.end(), sc, sc + 4);
        annexB.insert(annexB.end(), nal.begin(), nal.end());
      };
      if (vp.h265) append(vp.vps);
      append(vp.sps);
      append(vp.pps);
      holder->webStream->SetParams(vp.h265, annexB, holder->streamWidth.load(),
                                   holder->streamHeight.load());
    }
    uint64_t previewId = g_pendingPreviewId.load();
    if (previewId != 0) holder->AttachCameraPreview(previewId);
  }
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

napi_value RtspStopCamera(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder != nullptr) holder->StopCameraOutput();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspStartMic(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);

  if (holder->mic) {
    holder->mic->Stop();
    holder->mic.reset();
  }
  holder->audioClock.Reset();
  auto mic = std::make_unique<ipcam::MicStreamer>();
  bool ok = mic->Start(
      48000, 1, 64000,
      [holder](const uint8_t* data, size_t size, ipcam::TimestampUs ptsUs) {
        holder->PushAudio(data, size, holder->audioClock.Map(ptsUs));
      },
      [holder](const std::string& err) {
        PostEvent(holder, ipcam::RtspEvent::kServerError, holder->server.ClientCount(), err);
      });
  if (ok) {
    holder->mic = std::move(mic);
  }
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

napi_value RtspStopMic(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder != nullptr && holder->mic) {
    holder->mic->Stop();
    holder->mic.reset();
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

struct RtmpStartContext {
  ServerHolder* holder = nullptr;
  napi_deferred deferred = nullptr;
  napi_async_work work = nullptr;
  napi_ref holderRef = nullptr;
  std::shared_ptr<ipcam::RtmpPublisher> publisher;
  std::string url;
  uint64_t generation = 0;
  int width = 0;
  int height = 0;
  bool h265 = false;
  bool ok = false;
};

void ExecuteRtmpStart(napi_env /*env*/, void* data) {
  auto* ctx = static_cast<RtmpStartContext*>(data);
  if (ctx == nullptr || ctx->publisher == nullptr) return;
  const std::weak_ptr<ipcam::RtmpPublisher> weakPublisher = ctx->publisher;
  ctx->ok = ctx->publisher->Start(
      ctx->url, ctx->width, ctx->height, ctx->h265,
      [holder = ctx->holder, generation = ctx->generation,
       weakPublisher](const std::string& err) {
        auto publisher = weakPublisher.lock();
        if (!publisher) return;
        {
          std::lock_guard<std::mutex> lk(holder->outputsMu);
          if (holder->rtmpGeneration->load() != generation ||
              (holder->rtmp != publisher && holder->pendingRtmp != publisher)) {
            return;
          }
        }
        PostRtmpEvent(holder, generation, "rtmp: " + err);
      });
}

void CompleteRtmpStart(napi_env env, napi_status status, void* data) {
  std::unique_ptr<RtmpStartContext> ctx(static_cast<RtmpStartContext*>(data));
  if (ctx == nullptr) return;

  bool installed = false;
  bool superseded = false;
  if (ctx->holder != nullptr) {
    std::lock_guard<std::mutex> lk(ctx->holder->outputsMu);
    bool current = ctx->holder->rtmpGeneration->load() == ctx->generation &&
                   ctx->holder->pendingRtmp == ctx->publisher;
    if (current) {
      ctx->holder->pendingRtmp.reset();
      if (status == napi_ok && ctx->ok && ctx->publisher->IsRunning()) {
        ctx->holder->rtmp = ctx->publisher;
        installed = true;
      }
    } else {
      superseded = true;
    }
  }

  if (!installed && ctx->publisher != nullptr) ctx->publisher->Stop();

  napi_value result = nullptr;
  if (installed || superseded || status == napi_cancelled) {
    napi_get_boolean(env, installed, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
  } else {
    std::string message = ctx->publisher != nullptr ? ctx->publisher->LastError() : std::string();
    if (message.empty()) message = "RTMP start failed";
    napi_value text = nullptr;
    napi_value error = nullptr;
    napi_create_string_utf8(env, message.c_str(), message.size(), &text);
    napi_create_error(env, nullptr, text, &error);
    napi_reject_deferred(env, ctx->deferred, error);
  }

  if (ctx->holderRef != nullptr) napi_delete_reference(env, ctx->holderRef);
  if (ctx->work != nullptr) napi_delete_async_work(env, ctx->work);
}

napi_value RtmpStart(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  char url[1024];
  size_t len = 0;
  if (napi_get_value_string_utf8(env, argv[1], url, sizeof(url), &len) != napi_ok) {
    napi_throw_type_error(env, nullptr, "expect url string");
    return nullptr;
  }

  auto ctx = std::make_unique<RtmpStartContext>();
  ctx->holder = holder;
  ctx->publisher = std::make_shared<ipcam::RtmpPublisher>();
  ctx->url.assign(url, len);
  ctx->width = holder->streamWidth.load();
  ctx->height = holder->streamHeight.load();
  ctx->h265 = holder->videoIsH265.load();
  auto params = holder->server.GetVideoParams();
  if (params.ready) {
    ctx->publisher->SetVideoParams(params.h265, params.vps, params.sps, params.pps);
  }

  std::shared_ptr<ipcam::RtmpPublisher> oldActive;
  std::shared_ptr<ipcam::RtmpPublisher> oldPending;
  {
    std::lock_guard<std::mutex> lk(holder->outputsMu);
    ctx->generation = holder->rtmpGeneration->fetch_add(1) + 1;
    oldActive = std::move(holder->rtmp);
    oldPending = std::move(holder->pendingRtmp);
    holder->pendingRtmp = ctx->publisher;
  }
  if (oldPending && oldPending != oldActive) oldPending->Cancel();
  if (oldActive) oldActive->Stop();

  napi_value promise = nullptr;
  if (napi_create_reference(env, argv[0], 1, &ctx->holderRef) != napi_ok) {
    ctx->publisher->Stop();
    {
      std::lock_guard<std::mutex> lk(holder->outputsMu);
      if (holder->pendingRtmp == ctx->publisher) holder->pendingRtmp.reset();
    }
    napi_throw_error(env, nullptr, "retain RTMP server handle failed");
    return nullptr;
  }
  if (napi_create_promise(env, &ctx->deferred, &promise) != napi_ok) {
    ctx->publisher->Stop();
    {
      std::lock_guard<std::mutex> lk(holder->outputsMu);
      if (holder->pendingRtmp == ctx->publisher) holder->pendingRtmp.reset();
    }
    napi_delete_reference(env, ctx->holderRef);
    napi_throw_error(env, nullptr, "create RTMP promise failed");
    return nullptr;
  }
  napi_value resourceName = nullptr;
  napi_create_string_utf8(env, "rtmp_start", NAPI_AUTO_LENGTH, &resourceName);
  napi_status workStatus = napi_create_async_work(
      env, nullptr, resourceName, ExecuteRtmpStart, CompleteRtmpStart,
      ctx.get(), &ctx->work);
  if (workStatus == napi_ok) workStatus = napi_queue_async_work(env, ctx->work);
  if (workStatus != napi_ok) {
    if (ctx->work != nullptr) napi_delete_async_work(env, ctx->work);
    ctx->publisher->Stop();
    {
      std::lock_guard<std::mutex> lk(holder->outputsMu);
      if (holder->pendingRtmp == ctx->publisher) holder->pendingRtmp.reset();
    }
    napi_delete_reference(env, ctx->holderRef);
    napi_value text = nullptr;
    napi_value error = nullptr;
    napi_create_string_utf8(env, "queue RTMP work failed", NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, nullptr, text, &error);
    napi_reject_deferred(env, ctx->deferred, error);
    return promise;
  }
  ctx.release();
  return promise;
}

napi_value RtmpStop(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder != nullptr) holder->StopRtmpOutputs();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspStartRecord(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  char path[512];
  size_t len = 0;
  if (napi_get_value_string_utf8(env, argv[1], path, sizeof(path), &len) != napi_ok) {
    napi_throw_type_error(env, nullptr, "expect path string");
    return nullptr;
  }
  holder->StopRecorderOutput();
  auto rec = std::make_shared<ipcam::StreamRecorder>();
  int rotation = holder->CameraOrientation();
  auto vp = holder->server.GetVideoParams();
  if (vp.ready) {
    rec->SetVideoParams(vp.h265, vp.vps, vp.sps, vp.pps, holder->streamWidth.load(),
                        holder->streamHeight.load());
  }
  bool ok = rec->Start(std::string(path, len), rotation,
                       [holder](const std::string& err) {
                         PostEvent(holder, ipcam::RtspEvent::kServerError,
                                   holder->server.ClientCount(), "record: " + err);
                       });
  if (ok) {
    std::lock_guard<std::mutex> lk(holder->outputsMu);
    holder->recorder = std::move(rec);
  }
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

napi_value RtspStopRecord(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder != nullptr) holder->StopRecorderOutput();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspTakeSnapshot(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  char path[512];
  size_t len = 0;
  if (napi_get_value_string_utf8(env, argv[1], path, sizeof(path), &len) != napi_ok) {
    napi_throw_type_error(env, nullptr, "expect path string");
    return nullptr;
  }
  int w = holder->streamWidth.load();
  int h = holder->streamHeight.load();
  auto vp = holder->server.GetVideoParams();
  if (vp.ready) {
    std::vector<uint8_t> annexB;
    const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
    auto append = [&annexB, &sc](const std::vector<uint8_t>& nal) {
      if (nal.empty()) return;
      annexB.insert(annexB.end(), sc, sc + 4);
      annexB.insert(annexB.end(), nal.begin(), nal.end());
    };
    if (vp.h265) {
      append(vp.vps);
    }
    append(vp.sps);
    append(vp.pps);
    holder->snapshot->SetParams(vp.h265, annexB);
  }
  holder->snapshot->CaptureAsync(
      std::string(path, len), w, h,
      [holder](bool ok, const std::string& detail) {
        PostEvent(holder,
                  ok ? ipcam::RtspEvent::kClientPlaying : ipcam::RtspEvent::kServerError,
                  holder->server.ClientCount(),
                  ok ? "snapshot saved " + detail : "snapshot failed: " + detail);
      });
  napi_value result;
  napi_get_boolean(env, true, &result);
  return result;
}

napi_value RtspStartHttp(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  napi_value opts = argv[1];
  double port = 8081;
  napi_value v = nullptr;
  if (napi_get_named_property(env, opts, "port", &v) == napi_ok && v != nullptr) {
    napi_get_value_double(env, v, &port);
  }
  char user[64] = "admin";
  char pass[64] = "admin";
  char dir[512] = "/data/storage/el2/base/haps/entry/files";
  size_t len = 0;
  if (napi_get_named_property(env, opts, "user", &v) == napi_ok && v != nullptr) {
    napi_get_value_string_utf8(env, v, user, sizeof(user), &len);
  }
  if (napi_get_named_property(env, opts, "password", &v) == napi_ok && v != nullptr) {
    napi_get_value_string_utf8(env, v, pass, sizeof(pass), &len);
  }
  if (napi_get_named_property(env, opts, "filesDir", &v) == napi_ok && v != nullptr) {
    napi_get_value_string_utf8(env, v, dir, sizeof(dir), &len);
  }
  if (holder->http) holder->http->Stop();
  if (holder->httpWebStreamActive) {
    holder->webStream->DecClients();
    holder->httpWebStreamActive = false;
  }
  holder->filesDir = dir;
  if (!holder->http) holder->http = std::make_unique<ipcam::HttpServer>();
  std::string userStr = user;
  std::string passStr = pass;
  auto weakHolder = holder;
  holder->http->SetJpegSource([weakHolder]() {
    if (weakHolder->webStream == nullptr) return std::vector<uint8_t>();
    // One-shot snapshot may arrive before the web decoder has produced a
    // frame: hold a client reference and wait up to 3 s for the first JPEG.
    // The web console holds a client reference for its whole lifetime, so
    // the decoder stays warm for snapshot/MJPEG.
    std::vector<uint8_t> jpg;
    for (int i = 0; i < 30; ++i) {
      jpg = weakHolder->webStream->LatestJpeg();
      if (!jpg.empty()) break;
      usleep(100 * 1000);
    }
    return jpg;
  });
  holder->http->SetInfoSource([weakHolder]() {
    if (weakHolder->webStream == nullptr) return std::string("no stream");
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - weakHolder->startWall)
                    .count();
    std::string s = "uptime_s=" + std::to_string(secs);
    s += "\nresolution=" + std::to_string(weakHolder->streamWidth.load()) + "x" +
         std::to_string(weakHolder->streamHeight.load());
    s += "\ncodec=" +
         std::string(weakHolder->videoIsH265.load() ? "h265" : "h264");
    s += "\nrtsp_clients=" + std::to_string(weakHolder->server.ClientCount());
    s += "\nmotion=" + std::string(weakHolder->webStream->MotionActive() ? "active" : "idle");
    s += "\ntorch=" + std::string(weakHolder->torchOn.load() ? "on" : "off");
    s += "\nosd=" + std::string(ipcam::OsdEnabled() ? "on" : "off");
    s += "\nencoder=rear";
    return s;
  });
  holder->http->SetCommandSink([weakHolder](const std::string& cmd) {
    weakHolder->HandleCameraCommand(cmd);
  });
  holder->http->SetVoiceSink([weakHolder](const std::vector<uint8_t>& body) {
    switch (weakHolder->voice.Feed(body.data(), body.size())) {
      case ipcam::VoicePlayer::FeedResult::Accepted:
        return ipcam::HttpServer::VoiceResult::Accepted;
      case ipcam::VoicePlayer::FeedResult::Invalid:
        return ipcam::HttpServer::VoiceResult::Invalid;
      case ipcam::VoicePlayer::FeedResult::Busy:
        return ipcam::HttpServer::VoiceResult::Busy;
      case ipcam::VoicePlayer::FeedResult::Unavailable:
        return ipcam::HttpServer::VoiceResult::Unavailable;
    }
    return ipcam::HttpServer::VoiceResult::Unavailable;
  });
  holder->http->SetArchiveSource([weakHolder]() {
    std::string out;
    DIR* d = opendir((weakHolder->filesDir).c_str());
    if (d == nullptr) return out;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
      std::string name = e->d_name;
      if (name.rfind("ipc_", 0) != 0 || name.find(".mp4") == std::string::npos) continue;
      struct stat st{};
      std::string full = weakHolder->filesDir + "/" + name;
      if (stat(full.c_str(), &st) == 0) {
        out += name + " " + std::to_string(st.st_size) + "\n";
      }
    }
    closedir(d);
    return out;
  });
  holder->http->SetArchiveFile([weakHolder](const std::string& name,
                                            std::vector<uint8_t>* out) {
    if (name.rfind("ipc_", 0) != 0 || name.find("..") != std::string::npos ||
        name.find('/') != std::string::npos) {
      return false;
    }
    std::string full = weakHolder->filesDir + "/" + name;
    FILE* f = fopen(full.c_str(), "rb");
    if (f == nullptr) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize(static_cast<size_t>(size));
    size_t rd = fread(out->data(), 1, static_cast<size_t>(size), f);
    fclose(f);
    out->resize(rd);
    return rd > 0;
  });
  bool ok = holder->http->Start(static_cast<int>(port), userStr, passStr,
                                holder->filesDir);
  if (ok) {
    holder->webStream->IncClients();  // keep the web decoder warm
    holder->httpWebStreamActive = true;
    if (holder->CameraIsRunning()) {
      // Surface-mode encoders emit their only IDR at stream start; restart so
      // the web decoder (just started) can begin decoding immediately.
      holder->RestartCamera();
    }
  }
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

napi_value RtspStopHttp(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder != nullptr && holder->http) {
    holder->http->Stop();
    if (holder->httpWebStreamActive) {
      holder->webStream->DecClients();
      holder->httpWebStreamActive = false;
    }
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspSetMotion(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  bool enabled = false;
  napi_get_value_bool(env, argv[1], &enabled);
  if (argc >= 3) {
    double timeoutSec = 5;
    napi_get_value_double(env, argv[2], &timeoutSec);
    holder->webStream->SetMotionTimeout(static_cast<int>(timeoutSec));
  }
  holder->webStream->SetMotionSink([holder](bool motion) {
    PostEvent(holder, ipcam::RtspEvent::kClientPlaying, holder->server.ClientCount(),
              motion ? "MOTION detected" : "");
  });
  holder->webStream->SetMotionEnabled(enabled);
  napi_value result;
  napi_get_boolean(env, enabled, &result);
  return result;
}

napi_value OsdSetConfig(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  (void)holder;  // config is process-global; the handle keeps call-style uniform
  napi_value v = nullptr;
  auto getBool = [&](const char* key, bool def) {
    bool out = def;
    if (napi_get_named_property(env, argv[1], key, &v) == napi_ok && v != nullptr) {
      napi_get_value_bool(env, v, &out);
    }
    return out;
  };
  auto getInt = [&](const char* key, int def) {
    double out = def;
    if (napi_get_named_property(env, argv[1], key, &v) == napi_ok && v != nullptr) {
      napi_get_value_double(env, v, &out);
    }
    return static_cast<int>(out);
  };
  auto getStr = [&](const char* key) {
    char buf[512] = {0};
    size_t len = 0;
    if (napi_get_named_property(env, argv[1], key, &v) == napi_ok && v != nullptr) {
      if (napi_get_value_string_utf8(env, v, buf, sizeof(buf), &len) == napi_ok) {
        return std::string(buf, len);
      }
    }
    return std::string();
  };
  auto getHex = [&](const char* key, uint32_t def) {
    std::string s = getStr(key);
    if (s.empty()) return def;
    if (s[0] == '#') s = s.substr(1);
    // #RRGGBBAA (string resource) -> 0xAARRGGBB (skia/ArkUI color)
    if (s.size() != 8) return def;
    auto nib = [](char c) -> uint32_t {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return 0;
    };
    uint32_t r = nib(s[0]) << 4 | nib(s[1]);
    uint32_t g = nib(s[2]) << 4 | nib(s[3]);
    uint32_t b = nib(s[4]) << 4 | nib(s[5]);
    uint32_t a = nib(s[6]) << 4 | nib(s[7]);
    return a << 24 | r << 16 | g << 8 | b;
  };

  ipcam::SetOsdTextConfig(
      getBool("enabled", false), getInt("padding", 1), getInt("position", 0),
      getInt("fontStyle", 0), getHex("color", 0xFFFFFFFF), getBool("showTimestamp", true),
      getBool("showDevName", true), getBool("showBattery", true), getBool("showGps", false),
      getBool("speedMph", false), getStr("customText"));
  ipcam::SetOsdWatermarkConfig(getBool("wmEnabled", false), getInt("wmPadding", 0),
                               getInt("wmMaxAreaPercent", 10), getInt("wmPosition", 0));
  napi_value result;
  napi_get_boolean(env, true, &result);
  return result;
}

napi_value OsdSetDeviceName(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  char buf[256] = {0};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, argv[1], buf, sizeof(buf), &len) == napi_ok) {
    ipcam::SetOsdDeviceName(std::string(buf, len));
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value OsdSetBattery(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 3) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  double level = -1;
  double state = 0;
  napi_get_value_double(env, argv[1], &level);
  napi_get_value_double(env, argv[2], &state);
  ipcam::SetOsdBattery(static_cast<int>(level), static_cast<int>(state));
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value OsdSetGps(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 6) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  double lat = 0, lng = 0, speed = 0;
  bool valid = false;
  napi_get_value_double(env, argv[1], &lat);
  napi_get_value_double(env, argv[2], &lng);
  napi_get_value_double(env, argv[3], &speed);
  napi_get_value_bool(env, argv[4], &valid);
  bool speedMph = false;
  napi_get_value_bool(env, argv[5], &speedMph);
  ipcam::SetOsdGps(valid, lat, lng, speed);
  // speed unit rides along so the renderer can label the right unit
  if (valid) {
    ipcam::OsdSnapshot snap = ipcam::GetOsd();
    if (snap.speedMph != speedMph) {
      ipcam::SetOsdTextConfig(snap.enabled, snap.padding, snap.position, snap.fontStyle,
                              snap.argb, snap.showTimestamp, snap.showDevName,
                              snap.showBattery, snap.showGps, speedMph, snap.customText);
    }
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value OsdSetWatermark(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  OH_PixelmapNative* pm = nullptr;
  Image_ErrorCode err =
      OH_PixelmapNative_ConvertPixelmapNativeFromNapi(env, argv[1], &pm);
  if (err != IMAGE_SUCCESS || pm == nullptr) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                 "osdSetWatermark: convert failed %{public}d", static_cast<int>(err));
    napi_throw_type_error(env, nullptr, "expect PixelMap");
    return nullptr;
  }
  OH_Pixelmap_ImageInfo* info2 = nullptr;
  uint32_t w = 0, h = 0;
  bool ok = false;
  if (OH_PixelmapImageInfo_Create(&info2) == IMAGE_SUCCESS &&
      OH_PixelmapNative_GetImageInfo(pm, info2) == IMAGE_SUCCESS &&
      OH_PixelmapImageInfo_GetWidth(info2, &w) == IMAGE_SUCCESS &&
      OH_PixelmapImageInfo_GetHeight(info2, &h) == IMAGE_SUCCESS && w > 0 && h > 0) {
    size_t bytes = static_cast<size_t>(w) * h * 4;
    std::vector<uint8_t> buf(bytes);
    size_t got = bytes;
    if (OH_PixelmapNative_ReadPixels(pm, buf.data(), &got) == IMAGE_SUCCESS &&
        got == bytes) {
      ipcam::SetOsdWatermarkPixels(buf.data(), static_cast<int>(w), static_cast<int>(h));
      ok = true;
    }
  }
  if (info2 != nullptr) OH_PixelmapImageInfo_Release(info2);
  OH_PixelmapNative_Release(pm);
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor props[] = {
      {"createRtspServer", nullptr, CreateServer, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStart", nullptr, RtspStart, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtspStop", nullptr, RtspStop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtspIsRunning", nullptr, RtspIsRunning, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"rtspClientCount", nullptr, RtspClientCount, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspSendH264", nullptr, RtspSendH264, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"rtspSendAdts", nullptr, RtspSendAdts, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"rtspSetStatusCallback", nullptr, RtspSetStatusCallback, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspClearStatusCallback", nullptr, RtspClearStatusCallback, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStartTestPattern", nullptr, RtspStartTestPattern, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStopTestPattern", nullptr, RtspStopTestPattern, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStartCamera", nullptr, RtspStartCamera, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStopCamera", nullptr, RtspStopCamera, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStartMic", nullptr, RtspStartMic, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtmpStart", nullptr, RtmpStart, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStartRecord", nullptr, RtspStartRecord, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStopRecord", nullptr, RtspStopRecord, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspTakeSnapshot", nullptr, RtspTakeSnapshot, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStartHttp", nullptr, RtspStartHttp, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStopHttp", nullptr, RtspStopHttp, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspSetMotion", nullptr, RtspSetMotion, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspSetMotionTimeout", nullptr, RtspSetMotion, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtmpStop", nullptr, RtmpStop, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspStopMic", nullptr, RtspStopMic, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"osdSetConfig", nullptr, OsdSetConfig, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"osdSetDeviceName", nullptr, OsdSetDeviceName, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"osdSetBattery", nullptr, OsdSetBattery, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"osdSetGps", nullptr, OsdSetGps, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"osdSetWatermark", nullptr, OsdSetWatermark, nullptr, nullptr, nullptr,
       napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);

  // XComponent surface hookup for local camera preview.
  napi_value exportInstance = nullptr;
  if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) ==
          napi_ok &&
      exportInstance != nullptr) {
    OH_NativeXComponent* xc = nullptr;
    if (napi_unwrap(env, exportInstance, reinterpret_cast<void**>(&xc)) == napi_ok &&
        xc != nullptr) {
      static OH_NativeXComponent_Callback cb{};
      cb.OnSurfaceCreated = [](OH_NativeXComponent* component, void* window) {
        uint64_t sid = 0;
        if (window != nullptr &&
            OH_NativeWindow_GetSurfaceId(reinterpret_cast<OHNativeWindow*>(window), &sid) ==
                0) {
          g_pendingPreviewId.store(sid);
          std::lock_guard<std::mutex> lk(g_holderMu);
          if (g_holder != nullptr) g_holder->AttachCameraPreview(sid);
        }
      };
      OH_NativeXComponent_RegisterCallback(xc, &cb);
    }
  }
  return exports;
}

napi_module g_streamingModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "streaming",
    .nm_priv = nullptr,
    .reserved = {0},
};

}  // namespace

extern "C" __attribute__((constructor)) void RegisterStreamingModule(void) {
  napi_module_register(&g_streamingModule);
}
