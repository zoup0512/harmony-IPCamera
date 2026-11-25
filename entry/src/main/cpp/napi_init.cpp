#include <atomic>
#include <cmath>
#include <deque>
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
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>

#include "camera_capabilities.h"
#include "camera_streamer.h"
#include "rtsp_client.h"
#include "rtmp_viewer.h"
#include "rtmp_tls_napi.h"
#include "rtmp_tls_pipe.h"
#include "http_server.h"
#include "flv_stream.h"
#include "media_time.h"
#include "mic_streamer.h"
#include "onvif_client.h"
#include "opus_stream.h"
#include "osd_state.h"
#include "recorder.h"
#include "safe_archive_dir.h"
#include "cloud_archive_napi.h"
#include "record_video_encoder.h"
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

// Isolated from status/HTTP. Only the writer's successful-close sink may Post.
// A TSFN wakeup refers only to its registration, never to a server or napi_env.
struct ArchiveChannel {
  static constexpr size_t kCapacity = 256;
  struct Inbox {
    std::mutex mu;
    std::deque<std::string> paths;
    bool scheduled = false, overflowReported = false;
  };
  using Wake = std::weak_ptr<Inbox>;
  std::mutex mu;
  napi_threadsafe_function fn = nullptr;
  std::shared_ptr<Inbox> inbox;

  static void Deliver(napi_env env, napi_value cb, void*, void* data) {
    std::unique_ptr<Wake> wake(static_cast<Wake*>(data));
    if (!wake || !env || !cb) return;
    for (;;) {
      std::string path;
      {
        auto box = wake->lock();  // a callback may have cleared/replaced its own registration
        if (!box) return;
        std::lock_guard<std::mutex> lk(box->mu);
        if (box->paths.empty()) { box->scheduled = false; return; }
        path = std::move(box->paths.front());
        box->paths.pop_front();
        box->overflowReported = false;
      }
      napi_value value, self;
      if (napi_create_string_utf8(env, path.data(), path.size(), &value) != napi_ok ||
          napi_get_undefined(env, &self) != napi_ok || napi_call_function(env, self, cb, 1, &value, nullptr) != napi_ok) {
        // Keep remaining paths available to Take (or a later wakeup) on a throwing callback.
        if (auto box = wake->lock()) { std::lock_guard<std::mutex> lk(box->mu); box->scheduled = false; }
        return;
      }
    }
  }

  napi_env cleanupEnv = nullptr;  // used only on the owning JS thread
  void ResetLocked() {
    inbox.reset();  // all pending wakeups for the old generation become no-ops
    if (fn) napi_release_threadsafe_function(fn, napi_tsfn_abort);
    fn = nullptr;
  }
  static void EnvClosing(void* data) {
    auto* channel = static_cast<ArchiveChannel*>(data);
    std::lock_guard<std::mutex> lk(channel->mu);
    channel->cleanupEnv = nullptr;  // hook is already being removed
    channel->ResetLocked();        // writer cannot post to a torn-down env
  }
  bool Replace(napi_threadsafe_function next, napi_env env = nullptr) {
    std::lock_guard<std::mutex> lk(mu);
    if (cleanupEnv) napi_remove_env_cleanup_hook(cleanupEnv, EnvClosing, this);
    cleanupEnv = nullptr;
    ResetLocked();
    if (!next) return true;
    // Registered after the TSFN's own hook so we detach the native producer first.
    if (napi_add_env_cleanup_hook(env, EnvClosing, this) != napi_ok) {
      napi_release_threadsafe_function(next, napi_tsfn_abort);
      return false;
    }
    cleanupEnv = env;
    fn = next;
    inbox = std::make_shared<Inbox>();
    napi_unref_threadsafe_function(env, fn);  // subscription alone is not an app keep-alive
    return true;
  }

  std::deque<std::string> Take() {
    std::deque<std::string> out;
    std::lock_guard<std::mutex> lk(mu);
    if (inbox) {
      std::lock_guard<std::mutex> queue(inbox->mu);
      out.swap(inbox->paths);
      inbox->overflowReported = false;
    }
    return out;
  }

  bool Post(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu);
    if (!fn) return true;  // default: no registration, no retained paths
    std::lock_guard<std::mutex> queue(inbox->mu);
    if (inbox->paths.size() == kCapacity) return std::exchange(inbox->overflowReported, true);
    inbox->paths.push_back(path);
    if (!inbox->scheduled) {
      auto* wake = new Wake(inbox);
      if (napi_call_threadsafe_function(fn, wake, napi_tsfn_nonblocking) != napi_ok) {
        delete wake;
        return false;  // Take still owns the accepted path
      }
      inbox->scheduled = true;
    }
    return true;
  }
};

struct EncodedSize { int width, height; };

struct ServerHolder {
  ipcam::RtspServer server;
  std::shared_ptr<ipcam::FlvStream> flv = std::make_shared<ipcam::FlvStream>();
  std::unique_ptr<ipcam::TestPatternSource> pattern;
  std::unique_ptr<ipcam::CameraStreamer> camera;
  std::mutex cameraMu;
  // Serialize remote reconfiguration with starting consumers. The encoder's
  // callbacks must never take this lock (stopping the encoder drains them).
  std::mutex mediaControlMu;
  uint64_t remoteSizeGeneration = 0;  // guarded by cameraMu
  std::unique_ptr<ipcam::MicStreamer> mic;
  std::unique_ptr<ipcam::OpusStream> opus = std::make_unique<ipcam::OpusStream>();
  std::shared_ptr<ipcam::RtmpPublisher> rtmp;
  std::shared_ptr<ipcam::RtmpPublisher> pendingRtmp;
  std::shared_ptr<ipcam::StreamRecorder> recorder;
  std::shared_ptr<ipcam::RecordVideoEncoder> recordEncoder;  // published/removed with recorder
  ArchiveChannel archives;
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
  std::atomic<EncodedSize> encodedSize{EncodedSize{1280, 720}};
  std::atomic<bool> videoIsH265{false};  // last camera mime, used as RTMP hint
  std::atomic<bool> webParamsInjected{false};
  bool httpWebStreamActive = false;

  void ResetVideoFormat(int width, int height, bool hevc) {
    server.ResetVideoParams(hevc);
    flv->ResetVideoFormat(hevc);
    webStream->ResetVideoInput();
    snapshot->ResetVideoInput();
    encodedSize.store({width, height});
    videoIsH265.store(hevc);
    webParamsInjected.store(false);
  }

  ipcam::HttpServer::SizeResult RemoteSizeList(ipcam::HttpServer::SizeList& out) {
    using Result = ipcam::HttpServer::SizeResult;
    std::lock_guard<std::mutex> lk(cameraMu);
    if (!camera || !camera->IsRunning()) return Result::CameraClosed;
    if (camera->DualLayout() != 0) return Result::Busy;
    out.sizes.clear();
    out.currentIndex = -1;
    for (const auto& size : camera->AvailableResolutions()) {
      if (size.first == camera->CaptureWidth() && size.second == camera->CaptureHeight()) {
        out.currentIndex = static_cast<int>(out.sizes.size());
      }
      out.sizes.push_back({size.first, size.second});
    }
    return out.sizes.empty() ? Result::CameraClosed : Result::Ok;
  }

  // Caller holds mediaControlMu (take cameraMu second when needed). Reserve admission, not just
  // current client counts: an RTSP/FLV client may arrive during a restart.
  struct VideoChangeLease {
    ServerHolder& h;
    bool rtspHeld = false, flvHeld = false;
    bool Acquire() {
      if (h.webStream->MotionEnabled() || h.snapshot->IsBusy() || h.MediaOutputInUse()) return false;
      return ReserveSubscribers();
    }
    bool ReserveSubscribers() {
      rtspHeld = h.server.TryBeginVideoChange();
      if (rtspHeld) flvHeld = h.flv->TryBeginVideoChange();
      return flvHeld;
    }
    ~VideoChangeLease() {
      if (flvHeld) h.flv->EndVideoChange();
      if (rtspHeld) h.server.EndVideoChange();
    }
  };

  ipcam::HttpServer::SizeResult SelectRemoteSize(int index) {
    using Result = ipcam::HttpServer::SizeResult;
    using Resize = ipcam::CameraStreamer::ResizeResult;
    std::unique_lock<std::mutex> control(mediaControlMu, std::try_to_lock);
    if (!control.owns_lock()) return Result::Busy;
    std::lock_guard<std::mutex> lk(cameraMu);
    if (!camera || !camera->IsRunning()) return Result::CameraClosed;
    if (camera->DualLayout() != 0) return Result::Busy;
    const auto sizes = camera->AvailableResolutions();
    if (index < 0 || static_cast<size_t>(index) >= sizes.size()) return Result::NotFound;
    const auto [w, h] = sizes[index];
    if (camera->CaptureWidth() == w && camera->CaptureHeight() == h) return Result::Ok;
    VideoChangeLease lease{*this};
    if (!lease.Acquire()) return Result::Busy;
    const auto result = camera->ChangeResolution(w, h);
    if (result == Resize::Rejected) return Result::NotFound;
    if (result == Resize::Unchanged) return Result::Ok;
    if (result == Resize::Stopped) ResetVideoFormat(0, 0, videoIsH265.load());
    ++remoteSizeGeneration;
    PostEvent(this, ipcam::RtspEvent::kCameraSizeChanged, server.ClientCount(),
              result == Resize::Changed ? "resolution changed" :
              result == Resize::Restored ? "previous resolution restored" : "camera stopped");
    return result == Resize::Changed ? Result::Ok : Result::RestartFailed;
  }

  int ChangeDualLayout(int layout, const ipcam::DualCameraPlan* plan) {
    using Resize = ipcam::CameraStreamer::ResizeResult;
    std::unique_lock<std::mutex> control(mediaControlMu, std::try_to_lock);
    if (!control.owns_lock()) return 1;
    std::lock_guard<std::mutex> lk(cameraMu);
    if (!camera || !camera->IsRunning()) return 3;
    if (layout == 0 && camera->DualLayout() == 0) return 0;
    VideoChangeLease lease{*this};
    if (!lease.Acquire()) return 1;
    switch (camera->SetDualLayout(layout, plan)) {
      case Resize::Changed: case Resize::Unchanged: return 0;
      case Resize::Rejected: return 1;
      case Resize::Restored: return 2;
      case Resize::Stopped: ResetVideoFormat(0, 0, videoIsH265.load()); return 3;
    }
    return 1;
  }

  void PushVideo(const uint8_t* data, size_t size, ipcam::TimestampUs tsUs) {
    if (tsUs == ipcam::kNoTimestampUs) tsUs = ipcam::NowMonotonicUs();
    if (tsUs < 0) return;
    server.PushH264(data, size, tsUs);
    flv->FeedVideo(data, size, tsUs);
    // ResetVideoFormat clears both the latch and old parameter sets before the
    // new encoder can deliver buffers, including on rollback to the old size.
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
        const auto size = encodedSize.load();
        webStream->SetParams(vp.h265, annexB, size.width, size.height);
        webParamsInjected.store(true);
      }
    }
    webStream->FeedVideo(data, size);
    snapshot->FeedVideo(data, size);

    std::shared_ptr<ipcam::RtmpPublisher> rtmpSnapshot;
    std::shared_ptr<ipcam::StreamRecorder> recorderSnapshot;
    std::shared_ptr<ipcam::RecordVideoEncoder> encoderSnapshot;
    {
      std::lock_guard<std::mutex> lk(outputsMu);
      rtmpSnapshot = rtmp;
      recorderSnapshot = recorder;
      encoderSnapshot = recordEncoder;
    }
    if (rtmpSnapshot) rtmpSnapshot->SendH264Packet(data, size, tsUs);
    if (encoderSnapshot) encoderSnapshot->FeedVideo(data, size, tsUs);
    else if (recorderSnapshot) recorderSnapshot->FeedVideo(data, size, tsUs);
  }

  void PushAudio(const uint8_t* data, size_t size, ipcam::TimestampUs tsUs) {
    if (tsUs == ipcam::kNoTimestampUs) tsUs = ipcam::NowMonotonicUs();
    if (tsUs < 0) return;
    server.PushAdts(data, size, tsUs);
    flv->FeedAudio(data, size, tsUs);

    std::shared_ptr<ipcam::RtmpPublisher> rtmpSnapshot;
    std::shared_ptr<ipcam::StreamRecorder> recorderSnapshot;
    std::shared_ptr<ipcam::RecordVideoEncoder> encoderSnapshot;
    {
      std::lock_guard<std::mutex> lk(outputsMu);
      rtmpSnapshot = rtmp;
      recorderSnapshot = recorder;
      encoderSnapshot = recordEncoder;
    }
    if (rtmpSnapshot) rtmpSnapshot->SendAdtsPacket(data, size, tsUs);
    if (encoderSnapshot) encoderSnapshot->FeedAudio(data, size, tsUs);
    else if (recorderSnapshot) recorderSnapshot->FeedAudio(data, size, tsUs);
  }

  bool MediaOutputInUse() {
    std::lock_guard<std::mutex> lk(outputsMu);
    return recorder || rtmp || pendingRtmp;
  }

  // Call under mediaControlMu or after sources have been detached at finalization.
  void StopMicOutput() {
    opus->SetSourceActive(false);
    if (mic) {
      mic->Stop();
      mic.reset();
    }
    server.ResetAudioParams();  // no old-format callbacks remain after Stop
  }

  void StopCameraOutput() {
    std::unique_ptr<ipcam::CameraStreamer> current;
    {
      std::lock_guard<std::mutex> lk(cameraMu);
      current = std::move(camera);
      if (current && torchOn.exchange(false)) current->SetTorch(false);
    }
    if (current) current->Stop();
  }

  bool CameraIsRunning() {
    std::lock_guard<std::mutex> lk(cameraMu);
    return camera && camera->IsRunning();
  }

  // Encoder rebuilds share mediaControlMu with resize, dual layout and camera start.
  // A busy holder is itself rebuilding the encoder, which already emits a fresh IDR.
  bool RestartCamera() {
    std::unique_lock<std::mutex> control(mediaControlMu, std::try_to_lock);
    if (!control.owns_lock()) return false;
    std::lock_guard<std::mutex> lk(cameraMu);
    if (!camera || !camera->IsRunning() || MediaOutputInUse()) return false;
    const bool ok = camera->Restart();
    if (!camera->IsRunning()) ResetVideoFormat(0, 0, videoIsH265.load());  // rollback also failed
    return ok;
  }

  bool AttachCameraPreview(uint64_t surfaceId) {
    std::lock_guard<std::mutex> lk(cameraMu);
    return camera && camera->AttachPreview(surfaceId);
  }

  bool HandleCameraCommand(const std::string& command) {
    if (command == "light") {
      std::lock_guard<std::mutex> lk(cameraMu);
      if (!camera || !camera->IsRunning()) return false;
      bool nextTorch = !torchOn.load();
      if (!camera->SetTorch(nextTorch)) return false;
      torchOn.store(nextTorch);
      return true;
    }
    if (command != "camswitch") return false;
    // Same serialization as other encoder rebuilds (mediaControlMu, then cameraMu).
    std::unique_lock<std::mutex> control(mediaControlMu, std::try_to_lock);
    if (!control.owns_lock()) return false;
    std::lock_guard<std::mutex> lk(cameraMu);
    if (!camera || !camera->IsRunning() || MediaOutputInUse()) return false;
    const bool ok = camera->SwitchFacing();
    if (!camera->IsRunning()) ResetVideoFormat(0, 0, videoIsH265.load());
    return ok;
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

  bool StartRecording(const std::string& path, const ipcam::StreamRecorder::Options& options,
                      const ipcam::RecordVideoEncoder::Options* transcode) {
    std::lock_guard<std::mutex> control(mediaControlMu);
    std::lock_guard<std::mutex> cameraLock(cameraMu);  // snapshot a single encoder epoch, including preview restarts
    auto vp = server.GetVideoParams();
    const auto size = encodedSize.load();
    const auto report = [this](const std::string& error) {
      PostEvent(this, ipcam::RtspEvent::kServerError, server.ClientCount(), "record: " + error);
    };
    if (transcode && (!vp.ready || vp.sps.empty() || vp.pps.empty() || (vp.h265 && vp.vps.empty()) ||
                      size.width <= 0 || size.height <= 0)) {
      report("source codec parameters not ready");
      return false;
    }
    StopRecorderOutput();
    auto rec = std::make_shared<ipcam::StreamRecorder>();
    rec->SetOptions(options);
    if (!transcode && vp.ready) rec->SetVideoParams(vp.h265, vp.vps, vp.sps, vp.pps, size.width, size.height);
    auto failed = std::make_shared<std::atomic<bool>>(false);
    auto lifecycle = std::make_shared<std::mutex>();  // serialize early codec failure with writer startup
    std::shared_ptr<ipcam::RecordVideoEncoder> encoder;
    if (transcode) {
      encoder = std::make_shared<ipcam::RecordVideoEncoder>();
      ipcam::RecordVideoEncoder::Sinks sinks;
      sinks.params = [rec](const ipcam::RecordVideoEncoder::Source& p) {
        rec->SetVideoParams(p.h265, p.vps, p.sps, p.pps, p.width, p.height);
      };
      sinks.video = [rec](const uint8_t* d, size_t n, int64_t ts) { rec->FeedVideo(d, n, ts); };
      if (options.includeAudio) sinks.audio = [rec](const uint8_t* d, size_t n, int64_t ts) { rec->FeedAudio(d, n, ts); };
      sinks.fatal = [rec, report, failed, lifecycle](const std::string& error) {
        std::lock_guard<std::mutex> lk(*lifecycle);
        failed->store(true);
        report(error);
        rec->Stop();  // encoder worker, not recorder writer; control joins this worker first
      };
      ipcam::RecordVideoEncoder::Source input{vp.h265, size.width, size.height, vp.vps, vp.sps, vp.pps};
      std::string error;
      if (!encoder->Configure(*transcode, input, std::move(sinks), error)) {
        failed->store(true);
        encoder->Stop();
        if (!error.empty()) report(error);
        return false;  // resource/capability failure: never opened a recording file
      }
    }
    {
      std::lock_guard<std::mutex> startup(*lifecycle);
      if (!failed->load() && rec->Start(path, camera ? camera->Orientation() : 0, report,
          [this, failed](const ipcam::StreamRecorder::CompletedFile& file) {
            if (!failed->load() && !archives.Post(file.path)) {
              PostEvent(this, ipcam::RtspEvent::kServerError, server.ClientCount(), "local archive notification queue unavailable");
            }
          })) {
        std::lock_guard<std::mutex> lk(outputsMu);
        recordEncoder = std::move(encoder);
        recorder = std::move(rec);
        return true;
      }
      failed->store(true);
    }
    if (encoder) encoder->Stop();  // no locks needed by fatal/JS held
    rec->Stop();
    return false;
  }

  void StopRecorderOutput() {
    std::shared_ptr<ipcam::StreamRecorder> current;
    std::shared_ptr<ipcam::RecordVideoEncoder> encoder;
    {
      std::lock_guard<std::mutex> lk(outputsMu);
      current = recorder;  // keep IsWritingPath visible during finalization
      encoder = recordEncoder;
    }
    // Stop accepts no new input, drains its worker (including any fatal sink)
    // before touching the writer. No lock held, and no passthrough window.
    if (encoder) encoder->Stop();
    if (current) {
      current->Stop();
      std::lock_guard<std::mutex> lk(outputsMu);
      if (recorder == current) { recordEncoder.reset(); recorder.reset(); }
    }
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
  holder->webStream->SetMotionEnabled(false);
  holder->webStream->SetMotionSink(nullptr);
  holder->StopCameraOutput();
  holder->StopMicOutput();
  holder->StopRtmpOutputs();
  holder->archives.Replace(nullptr);
  holder->StopRecorderOutput();
  holder->snapshot->Stop();
  holder->voice.Stop();
  holder->opus->Shutdown();
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
    holder->webStream->SetMotionEnabled(false);
    holder->server.Stop();
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value ServiceBoundV4Port(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], result;
  bool http = false;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc != 2 || napi_get_value_bool(env, argv[1], &http) != napi_ok) return InvalidArgs(env);
  auto* h = HolderFromArg(env, argv[0]);
  int port = h ? (http ? (h->http ? h->http->BoundV4Port() : 0) : h->server.BoundV4Port()) : 0;
  napi_create_int32(env, port, &result);
  return result;
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

napi_value ServiceViewerCounts(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  if (napi_create_object(env, &result) != napi_ok) return nullptr;
  auto setCount = [&](const char* key, uint32_t count) {
    napi_value value;
    return napi_create_uint32(env, count, &value) == napi_ok &&
           napi_set_named_property(env, result, key, value) == napi_ok;
  };
  if (!setCount("rtsp", holder->server.ClientCount()) ||
      !setCount("mjpeg", holder->http ? holder->http->MjpegViewerCount() : 0) ||
      !setCount("flv", static_cast<uint32_t>(holder->flv->ViewerCount())) ||
      !setCount("opus", holder->opus->ViewerCount())) return nullptr;
  return result;
}

napi_value CameraToggleTorch(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  return napi_get_boolean(env, holder->HandleCameraCommand("light"), &result) == napi_ok ? result : nullptr;
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
  if (holder->CameraIsRunning()) {
    napi_value result;
    napi_get_boolean(env, false, &result);
    return result;
  }
  holder->patternClock.Reset();
  holder->ResetVideoFormat(static_cast<int>(width), static_cast<int>(height), false);
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
  } else {
    holder->ResetVideoFormat(0, 0, false);
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

bool PositiveInt(napi_env env, napi_value value, int32_t& out) {
  double n = 0;
  if (napi_get_value_double(env, value, &n) != napi_ok || !std::isfinite(n) ||
      n < 1 || n > INT32_MAX || n != std::floor(n)) return false;
  out = static_cast<int32_t>(n);
  return true;
}

// Do not silently truncate explicit device/codec names or a file path into another selection.
bool MediaString(napi_env env, napi_value arg, std::string& out) {
  size_t bytes = 0, copied = 0;
  if (napi_get_value_string_utf8(env, arg, nullptr, 0, &bytes) != napi_ok || bytes > 4095) return false;
  std::string buf(bytes + 1, '\0');
  if (napi_get_value_string_utf8(env, arg, buf.data(), buf.size(), &copied) != napi_ok || copied != bytes) return false;
  buf.resize(bytes);
  if (buf.find('\0') != std::string::npos) return false;
  out = std::move(buf);
  return true;
}

bool MediaArgPresent(napi_env env, napi_value arg) {
  napi_valuetype type;
  return arg && napi_typeof(env, arg, &type) == napi_ok && type != napi_undefined;
}

bool MediaInt(napi_env env, napi_value arg, int low, int high, int& out) {
  double n;
  if (napi_get_value_double(env, arg, &n) != napi_ok || !std::isfinite(n) ||
      n < low || n > high || n != std::floor(n)) return false;
  out = static_cast<int>(n);
  return true;
}

napi_value MediaJson(napi_env env, const std::string& json) {
  napi_value result;
  return napi_create_string_utf8(env, json.c_str(), json.size(), &result) == napi_ok ? result : nullptr;
}

napi_value MediaBool(napi_env env, bool value) {
  napi_value result;
  return napi_get_boolean(env, value, &result) == napi_ok ? result : nullptr;
}

napi_value CameraDevices(napi_env env, napi_callback_info) {
  return MediaJson(env, ipcam::CameraDevicesJson());
}

napi_value EncoderOptions(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string mime;
  if (!argc || !MediaString(env, argv[0], mime)) return InvalidArgs(env);
  return MediaJson(env, ipcam::EncoderOptionsJson(mime));
}

napi_value CameraResolutions(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  bool front;
  std::string deviceId;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || napi_get_value_bool(env, argv[0], &front) != napi_ok ||
      (argc > 1 && MediaArgPresent(env, argv[1]) && !MediaString(env, argv[1], deviceId))) return InvalidArgs(env);
  return MediaJson(env, ipcam::CameraResolutionsJson(front, deviceId));
}

napi_value CameraRemoteSizeState(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc == 1 ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  std::string state;
  {
    std::lock_guard<std::mutex> lk(holder->cameraMu);
    const bool running = holder->camera && holder->camera->IsRunning();
    state = "{\"generation\":" + std::to_string(holder->remoteSizeGeneration) +
        ",\"running\":" + (running ? "true" : "false") +
        ",\"width\":" + std::to_string(running ? holder->camera->CaptureWidth() : 0) +
        ",\"height\":" + std::to_string(running ? holder->camera->CaptureHeight() : 0) + "}";
  }
  napi_create_string_utf8(env, state.c_str(), state.size(), &result);
  return result;
}

napi_value CameraCaptureFps(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc == 1 ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  std::string json;
  {
    std::lock_guard<std::mutex> lk(holder->cameraMu);
    json = holder->camera ? holder->camera->CaptureFpsJson() : "{\"ranges\":[]}";
  }
  napi_create_string_utf8(env, json.c_str(), json.size(), &result);
  return result;
}

napi_value CameraSetCaptureFps(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t min, max;
  if (argc < 3 || !PositiveInt(env, argv[1], min) || !PositiveInt(env, argv[2], max)) return InvalidArgs(env);
  auto* holder = HolderFromArg(env, argv[0]);
  if (!holder) return InvalidArgs(env);
  bool ok;
  {
    std::lock_guard<std::mutex> lk(holder->cameraMu);
    ok = holder->camera && holder->camera->SetCaptureFps(min, max);
  }
  napi_get_boolean(env, ok, &result);
  return result;
}

bool ReadDualPlan(napi_env env, napi_value obj, ipcam::DualCameraPlan& p) {
  napi_valuetype type;
  if (napi_typeof(env, obj, &type) != napi_ok || type != napi_object) return false;
  napi_value v;
  auto id = [&](const char* key, std::string& s) {
    return napi_get_named_property(env, obj, key, &v) == napi_ok && MediaString(env, v, s) && !s.empty();
  };
  auto number = [&](const char* key, int low, int high, int& n) {
    return napi_get_named_property(env, obj, key, &v) == napi_ok && MediaInt(env, v, low, high, n);
  };
  return id("primaryId", p.primaryId) && id("secondaryId", p.secondaryId) && p.primaryId != p.secondaryId &&
      number("primaryType", 0, 1, p.primaryType) && number("secondaryType", 0, 1, p.secondaryType) &&
      number("width", 4, 4096, p.width) && number("height", 4, 4096, p.height) &&
      number("secondaryWidth", 4, 4096, p.secondaryWidth) && number("secondaryHeight", 4, 4096, p.secondaryHeight) &&
      !((p.width | p.height | p.secondaryWidth | p.secondaryHeight) & 1) &&
      p.width * p.height <= 4096 * 2160 && p.secondaryWidth * p.secondaryHeight <= 4096 * 2160;
}

napi_value CameraSetDualLayout(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc >= 2 ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  ipcam::DualCameraPlan plan;
  int layout = 0, code = 1;
  if (MediaInt(env, argv[1], 0, 3, layout) && (layout == 0 || (argc >= 3 && ReadDualPlan(env, argv[2], plan)))) {
    code = holder->ChangeDualLayout(layout, layout == 0 ? nullptr : &plan);
  }
  return napi_create_int32(env, code, &result) == napi_ok ? result : nullptr;
}

napi_value CameraDualLayout(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  std::lock_guard<std::mutex> lk(holder->cameraMu);
  return napi_create_int32(env, holder->camera ? holder->camera->DualLayout() : 0, &result) == napi_ok ? result : nullptr;
}

napi_value CameraFocusInfo(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  std::lock_guard<std::mutex> lk(holder->cameraMu);
  return MediaJson(env, holder->camera ? holder->camera->FocusInfoJson() : "{\"modes\":[]}");
}

napi_value CameraSetFocusMode(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int mode;
  auto* holder = argc >= 2 ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder || !MediaInt(env, argv[1], 0, INT32_MAX, mode)) return InvalidArgs(env);
  std::lock_guard<std::mutex> lk(holder->cameraMu);
  return MediaBool(env, holder->camera && holder->camera->SetFocusMode(mode));
}

napi_value CameraFocusAt(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  double x, y;
  auto* holder = argc >= 3 ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder || napi_get_value_double(env, argv[1], &x) != napi_ok ||
      napi_get_value_double(env, argv[2], &y) != napi_ok) return InvalidArgs(env);
  std::lock_guard<std::mutex> lk(holder->cameraMu);
  return MediaBool(env, x >= 0 && x <= 1 && y >= 0 && y <= 1 && holder->camera && holder->camera->FocusAt(x, y));
}

napi_value RtspStartCamera(napi_env env, napi_callback_info info) {
  size_t argc = 12;
  napi_value argv[12];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 7) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  int32_t width, height, bitrate, iFrameMs;
  char mime[32]{};
  size_t mimeLen = 0;
  bool front = false;
  if (!PositiveInt(env, argv[1], width) || !PositiveInt(env, argv[2], height) ||
      !PositiveInt(env, argv[3], bitrate) || !PositiveInt(env, argv[6], iFrameMs) ||
      napi_get_value_bool(env, argv[5], &front) != napi_ok ||
      napi_get_value_string_utf8(env, argv[4], mime, sizeof(mime), &mimeLen) != napi_ok ||
      (std::string(mime, mimeLen) != "video/avc" && std::string(mime, mimeLen) != "video/hevc")) {
    return InvalidArgs(env);
  }

  std::string deviceId, encoderName;
  int avcProfile = -1, degrees = -1;
  ipcam::CaptureOrientation orientation{};
  if ((argc >= 11 && MediaArgPresent(env, argv[10]) && !MediaInt(env, argv[10], -1, 270, degrees)) ||
      (argc >= 12 && MediaArgPresent(env, argv[11]) && napi_get_value_bool(env, argv[11], &orientation.mirror) != napi_ok)) return InvalidArgs(env);
  orientation.rotation = static_cast<ipcam::CaptureRotation>(degrees);
  if (!orientation.Valid()) return InvalidArgs(env);
  if ((argc >= 8 && MediaArgPresent(env, argv[7]) && !MediaString(env, argv[7], deviceId)) ||
      (argc >= 9 && MediaArgPresent(env, argv[8]) && !MediaString(env, argv[8], encoderName)) ||
      (argc >= 10 && MediaArgPresent(env, argv[9]) && !MediaInt(env, argv[9], -1, INT32_MAX, avcProfile))) return InvalidArgs(env);
  std::lock_guard<std::mutex> control(holder->mediaControlMu);
  ServerHolder::VideoChangeLease lease{*holder};
  if (holder->MediaOutputInUse() || ((holder->CameraIsRunning() || holder->pattern) && !lease.Acquire())) return MediaBool(env, false);
  if (holder->pattern) {
    holder->pattern->Stop();
    holder->pattern.reset();
  }
  holder->StopCameraOutput();
  auto camera = std::make_unique<ipcam::CameraStreamer>();
  camera->SetVideoFormatCallback([holder, hevc = std::string(mime) == "video/hevc"](int w, int h) {
    holder->ResetVideoFormat(w, h, hevc);
  });
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
      }, deviceId, encoderName, avcProfile, orientation);
  if (ok) {
    {
      std::lock_guard<std::mutex> lk(holder->cameraMu);
      holder->camera = std::move(camera);
    }
    std::lock_guard<std::mutex> lk(g_holderMu);
    if (holder == g_holder) holder->AttachCameraPreview(g_pendingPreviewId.load());
  } else {
    holder->ResetVideoFormat(0, 0, std::string(mime) == "video/hevc");
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
  if (holder != nullptr) {
    std::lock_guard<std::mutex> control(holder->mediaControlMu);
    holder->StopCameraOutput();
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspStartMic(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  int rate = 48000, channels = 1;
  if (!holder || (argc >= 2 && MediaArgPresent(env, argv[1]) && !MediaInt(env, argv[1], 0, 192000, rate)) ||
      (argc >= 3 && MediaArgPresent(env, argv[2]) && !MediaInt(env, argv[2], 1, 2, channels))) return InvalidArgs(env);
  std::lock_guard<std::mutex> control(holder->mediaControlMu);
  ServerHolder::VideoChangeLease lease{*holder};
  if (holder->MediaOutputInUse() || !lease.ReserveSubscribers()) return MediaBool(env, false);
  holder->StopMicOutput();
  holder->audioClock.Reset();
  auto mic = std::make_unique<ipcam::MicStreamer>();
  mic->SetPcmTap([holder](const uint8_t* pcm, size_t bytes, int rate, int ch) {
    holder->opus->FeedPcm(pcm, bytes, rate, ch);
  });
  bool ok = mic->Start(
      rate, channels, 64000,
      [holder](const uint8_t* data, size_t size, ipcam::TimestampUs ptsUs) {
        holder->PushAudio(data, size, holder->audioClock.Map(ptsUs));
      },
      [holder](const std::string& err) {
        PostEvent(holder, ipcam::RtspEvent::kServerError, holder->server.ClientCount(), err);
      });
  if (ok) {
    holder->opus->SetSourceActive(true, mic->SampleRate(), mic->Channels());
    holder->mic = std::move(mic);
  }
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

napi_value RtspMicFormat(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], result, hz, ch;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  std::lock_guard<std::mutex> control(holder->mediaControlMu);
  bool active = holder->mic && holder->mic->IsRunning();
  if (napi_create_object(env, &result) != napi_ok ||
      napi_create_int32(env, active ? holder->mic->SampleRate() : 0, &hz) != napi_ok ||
      napi_create_int32(env, active ? holder->mic->Channels() : 0, &ch) != napi_ok ||
      napi_set_named_property(env, result, "sampleRate", hz) != napi_ok ||
      napi_set_named_property(env, result, "channels", ch) != napi_ok) return nullptr;
  return result;
}

napi_value RtspStopMic(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder) {
    std::lock_guard<std::mutex> control(holder->mediaControlMu);
    holder->StopMicOutput();
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
  uint32_t tlsToken = 0;
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
      }, ctx->tlsToken);
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

napi_value RtmpFailure(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc == 1 ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  std::shared_ptr<ipcam::RtmpPublisher> active;
  {
    std::lock_guard<std::mutex> lk(holder->outputsMu);
    active = holder->rtmp;
  }
  const auto error = active ? active->LastError() : std::string();
  napi_create_string_utf8(env, error.c_str(), error.size(), &result);
  return result;
}

napi_value RtmpStart(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  size_t len = 0, used = 0;
  if (napi_get_value_string_utf8(env, argv[1], nullptr, 0, &len) != napi_ok || !len || len > 65535) {
    return InvalidArgs(env);
  }
  std::string url(len + 1, char(0));
  if (napi_get_value_string_utf8(env, argv[1], url.data(), url.size(), &used) != napi_ok || used != len) {
    return InvalidArgs(env);
  }
  url.resize(used);
  if (url.find(char(0)) != std::string::npos) return InvalidArgs(env);

  std::lock_guard<std::mutex> control(holder->mediaControlMu);
  auto ctx = std::make_unique<RtmpStartContext>();
  ctx->holder = holder;
  ctx->publisher = std::make_shared<ipcam::RtmpPublisher>();
  if (argc >= 3) {
    double token = 0;
    if (napi_get_value_double(env, argv[2], &token) != napi_ok || !std::isfinite(token) ||
        token < 1 || token > UINT32_MAX || token != std::floor(token)) return InvalidArgs(env);
    ctx->tlsToken = static_cast<uint32_t>(token);
    if (!ipcam::RtmpTlsPipe::Instance().IsOwned(ctx->tlsToken, env)) return InvalidArgs(env);
  }
  ctx->url = std::move(url);
  const auto size = holder->encodedSize.load();
  ctx->width = size.width;
  ctx->height = size.height;
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
  if (holder) {
    std::lock_guard<std::mutex> control(holder->mediaControlMu);
    holder->StopRtmpOutputs();
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspSetArchiveCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], name;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc >= 2 ? HolderFromArg(env, argv[0]) : nullptr;
  napi_valuetype type;
  if (!holder || napi_typeof(env, argv[1], &type) != napi_ok || type != napi_function) return InvalidArgs(env);
  napi_threadsafe_function fn = nullptr;
  // One outstanding nonblocking wakeup for this registration's bounded inbox.
  if (napi_create_string_utf8(env, "local_archive_closed", NAPI_AUTO_LENGTH, &name) != napi_ok ||
      napi_create_threadsafe_function(env, argv[1], nullptr, name, 1, 1, nullptr, nullptr, nullptr,
                                      ArchiveChannel::Deliver, &fn) != napi_ok) return MediaBool(env, false);
  return MediaBool(env, holder->archives.Replace(fn, env));
}

napi_value RtspTakePendingArchives(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], array;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  auto pending = holder->archives.Take();
  if (napi_create_array_with_length(env, pending.size(), &array) != napi_ok) return nullptr;
  uint32_t index = 0;
  for (const auto& path : pending) {
    napi_value value;
    if (napi_create_string_utf8(env, path.data(), path.size(), &value) != napi_ok ||
        napi_set_element(env, array, index++, value) != napi_ok) return nullptr;
  }
  return array;
}

napi_value RtspClearArchiveCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], undef;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* holder = argc ? HolderFromArg(env, argv[0]) : nullptr;
  if (!holder) return InvalidArgs(env);
  holder->archives.Replace(nullptr);
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value RtspStartRecord(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  std::string path;
  if (!MediaString(env, argv[1], path) || path.empty()) return InvalidArgs(env);
  ipcam::StreamRecorder::Options recordOptions;  // four_gb_limit / segment defaults
  std::string videoMime;
  ipcam::RecordVideoEncoder::Options videoOptions{false, 4000000, 2000};
  if (argc >= 3 && argv[2] != nullptr) {
    napi_valuetype optType = napi_undefined;
    napi_typeof(env, argv[2], &optType);
    if (optType == napi_object) {
      napi_value v = nullptr;
      if (napi_get_named_property(env, argv[2], "fourGbLimit", &v) == napi_ok &&
          v != nullptr) {
        napi_get_value_bool(env, v, &recordOptions.fourGbLimit);
      }
      if (napi_get_named_property(env, argv[2], "segmentMinutes", &v) != napi_ok ||
          (MediaArgPresent(env, v) && !MediaInt(env, v, 0, INT32_MAX, recordOptions.segmentMinutes))) return InvalidArgs(env);
      if (napi_get_named_property(env, argv[2], "includeAudio", &v) != napi_ok ||
          (MediaArgPresent(env, v) && napi_get_value_bool(env, v, &recordOptions.includeAudio) != napi_ok)) return InvalidArgs(env);
      if (napi_get_named_property(env, argv[2], "videoMime", &v) != napi_ok ||
          (MediaArgPresent(env, v) && !MediaString(env, v, videoMime))) return InvalidArgs(env);
      if (!videoMime.empty()) {
        if (videoMime != "video/avc" && videoMime != "video/hevc") return MediaBool(env, false);
        videoOptions.h265 = videoMime == "video/hevc";
        if (napi_get_named_property(env, argv[2], "videoBitrate", &v) != napi_ok ||
            (MediaArgPresent(env, v) && !MediaInt(env, v, 250000, 20000000, videoOptions.bitrate)) ||
            napi_get_named_property(env, argv[2], "gopMs", &v) != napi_ok ||
            (MediaArgPresent(env, v) && !MediaInt(env, v, 500, 10000, videoOptions.gopMs))) return MediaBool(env, false);
      }
    }
  }
  return MediaBool(env, holder->StartRecording(path, recordOptions, videoMime.empty() ? nullptr : &videoOptions));
}

napi_value RecordingFileBusy(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], result;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string path;
  if (!argc || !MediaString(env, argv[0], path)) return InvalidArgs(env);
  bool busy = false;
  {
    std::lock_guard<std::mutex> lk(g_holderMu);
    if (g_holder) {
      std::lock_guard<std::mutex> output(g_holder->outputsMu);
      busy = g_holder->recorder && g_holder->recorder->IsWritingPath(path);
    }
  }
  napi_get_boolean(env, busy, &result);
  return result;
}

napi_value RtspIsRecording(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  bool recording;
  {
    std::lock_guard<std::mutex> lk(holder->outputsMu);
    recording = holder->recorder && holder->recorder->IsRecording();
  }
  napi_value result;
  napi_get_boolean(env, recording, &result);
  return result;
}

napi_value RtspStopRecord(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder) {
    std::lock_guard<std::mutex> control(holder->mediaControlMu);
    holder->StopRecorderOutput();
  }
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
  std::lock_guard<std::mutex> control(holder->mediaControlMu);
  const auto size = holder->encodedSize.load();
  int w = size.width, h = size.height;
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
  bool allowLegacyVideoPassword = false;
  if (napi_get_named_property(env, opts, "allowLegacyVideoPassword", &v) == napi_ok && v) {
    napi_get_value_bool(env, v, &allowLegacyVideoPassword);
  }
  auto weakHolder = holder;
  holder->http->SetFreshJpegSource([weakHolder](const std::atomic<bool>& running) {
    return weakHolder->webStream->WaitForFreshJpeg(running);
  });
  holder->http->SetJpegSource([weakHolder]() {
    return weakHolder->webStream->LatestJpeg();
  });
  holder->http->SetInfoSource([weakHolder]() {
    if (weakHolder->webStream == nullptr) return std::string("no stream");
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - weakHolder->startWall)
                    .count();
    std::string s = "uptime_s=" + std::to_string(secs);
    const auto size = weakHolder->encodedSize.load();
    s += "\nresolution=" + std::to_string(size.width) + "x" + std::to_string(size.height);
    s += "\ncodec=" +
         std::string(weakHolder->videoIsH265.load() ? "h265" : "h264");
    s += "\nrtsp_clients=" + std::to_string(weakHolder->server.ClientCount());
    s += "\nmotion=" + std::string(weakHolder->webStream->MotionActive() ? "active" : "idle");
    s += "\ntorch=" + std::string(weakHolder->torchOn.load() ? "on" : "off");
    s += "\nosd=" + std::string(ipcam::OsdEnabled() ? "on" : "off");
    s += "\nencoder=rear";
    return s;
  });
  holder->http->SetSizeCallbacks(
      [weakHolder](ipcam::HttpServer::SizeList& list) { return weakHolder->RemoteSizeList(list); },
      [weakHolder](int index) { return weakHolder->SelectRemoteSize(index); });
  holder->http->SetCommandSink([weakHolder](const std::string& cmd) {
    return weakHolder->HandleCameraCommand(cmd);
  });
  holder->http->SetVoiceSink([weakHolder](const std::vector<uint8_t>& body) {
    switch (weakHolder->voice.FeedUpload(body.data(), body.size())) {
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
  holder->http->SetOpusStream(holder->opus.get());
  holder->http->SetFlvStream(holder->flv);
  const auto archives = std::make_shared<ipcam::SafeArchiveDir>(holder->filesDir);
  holder->http->SetArchiveSource([archives]() { return archives->List(); });
  holder->http->SetArchiveFile([archives](const std::string& key) { return archives->OpenFile(key); });
  bool ok = holder->http->Start(static_cast<int>(port), userStr, passStr,
                                holder->filesDir, allowLegacyVideoPassword);
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
  std::lock_guard<std::mutex> control(holder->mediaControlMu);
  bool enabled = false;
  napi_get_value_bool(env, argv[1], &enabled);
  if (argc >= 3) {
    double timeoutSec = 15;  // Android motion_timeout default; clamped natively
    napi_get_value_double(env, argv[2], &timeoutSec);
    holder->webStream->SetMotionTimeout(static_cast<int>(timeoutSec));
  }
  holder->webStream->SetMotionSink([holder](bool motion) {
    // Status events are advisory; ArkTS polls the authoritative motion state.
    PostEvent(holder, ipcam::RtspEvent::kClientPlaying, holder->server.ClientCount(),
              motion ? "MOTION detected" : "MOTION cleared");
  });
  holder->webStream->SetMotionEnabled(enabled);
  napi_value result;
  napi_get_boolean(env, enabled, &result);
  return result;
}

napi_value RtspMotionActive(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  napi_value result;
  napi_get_boolean(env, holder->server.IsRunning() && holder->webStream->MotionActive(), &result);
  return result;
}

napi_value RtspSetMotionTimeout(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return InvalidArgs(env);
  ServerHolder* holder = HolderFromArg(env, argv[0]);
  if (holder == nullptr) return InvalidArgs(env);
  int32_t seconds = 15;
  if (napi_get_value_int32(env, argv[1], &seconds) != napi_ok) return InvalidArgs(env);
  holder->webStream->SetMotionTimeout(seconds);
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

bool IsPrivateFontImport(const std::string& path) {
  if (path.empty()) return true;  // explicit reset / legacy configuration
  const auto split = path.rfind('/');
  if (split == std::string::npos) return false;
  const auto dir = path.substr(0, split), name = path.substr(split + 1);
  // UIAbilityContext.filesDir in the entry module's default sandbox. Do not
  // use caller-supplied HTTP filesDir, prefix matching, or arbitrary picker URIs.
  if (dir != "/data/storage/el2/base/haps/entry/files" && dir != "/data/storage/el2/base/files") return false;
  if (name.size() != 9 + 36 + 5 || name.compare(0, 9, "osd-font-") || name.compare(45, 5, ".font")) return false;
  for (size_t i = 0; i < 36; ++i) {
    const char c = name[9 + i];
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
    else if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) return false;
  }
  struct stat st{};
  return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 12 && st.st_size <= 32 * 1024 * 1024;
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
    int out = def;
    if (napi_get_named_property(env, argv[1], key, &v) == napi_ok && v != nullptr) {
      MediaInt(env, v, INT32_MIN, INT32_MAX, out);
    }
    return out;
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

  const auto fontPath = getStr("fontPath");
  if (!IsPrivateFontImport(fontPath)) return MediaBool(env, false);
  ipcam::SetOsdFontConfig(getInt("fontSize", 0), getStr("fontFamily"), fontPath);
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
  int32_t alphaType = 0, pixelFormat = 0;
  bool ok = false;
  if (OH_PixelmapImageInfo_Create(&info2) == IMAGE_SUCCESS &&
      OH_PixelmapNative_GetImageInfo(pm, info2) == IMAGE_SUCCESS &&
      OH_PixelmapImageInfo_GetWidth(info2, &w) == IMAGE_SUCCESS &&
      OH_PixelmapImageInfo_GetHeight(info2, &h) == IMAGE_SUCCESS && w && h &&
      static_cast<uint64_t>(w) * h <= 16 * 1024 * 1024 &&
      OH_PixelmapImageInfo_GetPixelFormat(info2, &pixelFormat) == IMAGE_SUCCESS && pixelFormat == PIXEL_FORMAT_RGBA_8888 &&
      OH_PixelmapImageInfo_GetAlphaType(info2, &alphaType) == IMAGE_SUCCESS) {
    size_t bytes = static_cast<size_t>(w) * h * 4;
    std::vector<uint8_t> buf(bytes);
    size_t got = bytes;
    if (OH_PixelmapNative_ReadPixels(pm, buf.data(), &got) == IMAGE_SUCCESS &&
        got == bytes) {
      ipcam::SetOsdWatermarkPixels(buf.data(), static_cast<int>(w), static_cast<int>(h),
                                   alphaType == PIXELMAP_ALPHA_TYPE_PREMULTIPLIED);
      ok = true;
    }
  }
  if (info2 != nullptr) OH_PixelmapImageInfo_Release(info2);
  OH_PixelmapNative_Release(pm);
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

// ---- ONVIF discovery (global module state, no server handle needed) ----

struct OnvifScanContext {
  napi_deferred deferred = nullptr;
  napi_async_work work = nullptr;
  int timeoutMs = 4000;
  std::string resultJson;  // array of device objects, or error text in `error`
  bool ok = true;
};

std::string JsonEscapeStr(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

void ExecuteOnvifScan(napi_env /*env*/, void* data) {
  auto* ctx = static_cast<OnvifScanContext*>(data);
  std::vector<ipcam::OnvifClient::Device> devices =
      ipcam::OnvifClient::Discover(ctx->timeoutMs, 1000);
  std::string json = "[";
  bool first = true;
  for (auto& dev : devices) {
    ipcam::OnvifClient::ResolveMedia(&dev);
    if (!first) json += ",";
    first = false;
    json += "{\"xaddrs\":\"" + JsonEscapeStr(dev.xaddrs) + "\"" +
            ",\"model\":\"" + JsonEscapeStr(dev.model) + "\"" +
            ",\"profile\":\"" + JsonEscapeStr(dev.profile) + "\"" +
            ",\"streamUri\":\"" + JsonEscapeStr(dev.streamUri) + "\"" +
            ",\"error\":\"" + JsonEscapeStr(dev.lastError) + "\"}";
  }
  json += "]";
  ctx->resultJson = std::move(json);
  ctx->ok = true;
}

void CompleteOnvifScan(napi_env env, napi_status status, void* data) {
  auto* ctx = static_cast<OnvifScanContext*>(data);
  if (status == napi_ok && ctx->ok) {
    napi_value text = nullptr;
    napi_create_string_utf8(env, ctx->resultJson.c_str(), ctx->resultJson.size(), &text);
    napi_resolve_deferred(env, ctx->deferred, text);
  } else {
    napi_value text = nullptr;
    napi_value error = nullptr;
    napi_create_string_utf8(env, "onvif scan failed", NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, nullptr, text, &error);
    napi_reject_deferred(env, ctx->deferred, error);
  }
  if (ctx->work != nullptr) napi_delete_async_work(env, ctx->work);
  delete ctx;
}

napi_value OnvifScan(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* ctx = new OnvifScanContext();
  if (argc >= 1 && argv[0] != nullptr) {
    double t = 4000;
    napi_get_value_double(env, argv[0], &t);
    ctx->timeoutMs = static_cast<int>(t);
  }
  napi_value promise = nullptr;
  if (napi_create_promise(env, &ctx->deferred, &promise) != napi_ok) {
    delete ctx;
    napi_throw_error(env, nullptr, "create promise failed");
    return nullptr;
  }
  napi_value resourceName = nullptr;
  napi_create_string_utf8(env, "onvif_scan", NAPI_AUTO_LENGTH, &resourceName);
  napi_status st = napi_create_async_work(env, nullptr, resourceName,
                                          ExecuteOnvifScan, CompleteOnvifScan,
                                          ctx, &ctx->work);
  if (st == napi_ok) st = napi_queue_async_work(env, ctx->work);
  if (st != napi_ok) {
    if (ctx->work != nullptr) napi_delete_async_work(env, ctx->work);
    napi_value text = nullptr;
    napi_value error = nullptr;
    napi_create_string_utf8(env, "queue onvif work failed", NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, nullptr, text, &error);
    napi_reject_deferred(env, ctx->deferred, error);
    delete ctx;
    return promise;
  }
  return promise;
}

napi_value Init(napi_env env, napi_value exports) {
  RegisterRtspClient(env, exports);
  RegisterRtmpViewer(env, exports);
  if (RegisterRtmpTls(env, exports) != napi_ok) return nullptr;
  if (cloud_archive_napi::Register(env, exports) != napi_ok) return nullptr;
  napi_property_descriptor props[] = {
      {"serviceBoundV4Port", nullptr, ServiceBoundV4Port, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"serviceViewerCounts", nullptr, ServiceViewerCounts, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraToggleTorch", nullptr, CameraToggleTorch, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraDevices", nullptr, CameraDevices, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"encoderOptions", nullptr, EncoderOptions, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraFocusInfo", nullptr, CameraFocusInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraSetFocusMode", nullptr, CameraSetFocusMode, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraFocusAt", nullptr, CameraFocusAt, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraSetDualLayout", nullptr, CameraSetDualLayout, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraDualLayout", nullptr, CameraDualLayout, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtspMicFormat", nullptr, RtspMicFormat, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtspSetArchiveCallback", nullptr, RtspSetArchiveCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtspTakePendingArchives", nullptr, RtspTakePendingArchives, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtspClearArchiveCallback", nullptr, RtspClearArchiveCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraResolutions", nullptr, CameraResolutions, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraRemoteSizeState", nullptr, CameraRemoteSizeState, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraCaptureFps", nullptr, CameraCaptureFps, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cameraSetCaptureFps", nullptr, CameraSetCaptureFps, nullptr, nullptr, nullptr, napi_default, nullptr},
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
      {"recordingFileBusy", nullptr, RecordingFileBusy, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtspIsRecording", nullptr, RtspIsRecording, nullptr, nullptr, nullptr,
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
      {"rtspMotionActive", nullptr, RtspMotionActive, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtspSetMotionTimeout", nullptr, RtspSetMotionTimeout, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"rtmpFailure", nullptr, RtmpFailure, nullptr, nullptr, nullptr, napi_default, nullptr},
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
      {"onvifScan", nullptr, OnvifScan, nullptr, nullptr, nullptr, napi_default,
       nullptr},
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
      char id[OH_XCOMPONENT_ID_LEN_MAX + 1]{};
      uint64_t len = sizeof(id);
      if (OH_NativeXComponent_GetXComponentId(xc, id, &len) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS ||
          std::string(id) != "cameraPreview") return exports;
      static OH_NativeXComponent_Callback cb{};
      cb.OnSurfaceCreated = [](OH_NativeXComponent*, void* window) {
        uint64_t sid = 0;
        if (window != nullptr &&
            OH_NativeWindow_GetSurfaceId(reinterpret_cast<OHNativeWindow*>(window), &sid) ==
                0) {
          std::lock_guard<std::mutex> lk(g_holderMu);
          g_pendingPreviewId.store(sid);
          if (g_holder != nullptr) g_holder->AttachCameraPreview(sid);
        }
      };
      cb.OnSurfaceDestroyed = [](OH_NativeXComponent*, void* window) {
        uint64_t sid = 0;
        if (!window || OH_NativeWindow_GetSurfaceId(static_cast<OHNativeWindow*>(window), &sid) != 0) return;
        std::lock_guard<std::mutex> lk(g_holderMu);
        if (sid != 0 && g_pendingPreviewId.compare_exchange_strong(sid, 0) && g_holder) {
          g_holder->AttachCameraPreview(0);
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
