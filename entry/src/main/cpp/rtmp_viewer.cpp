#include "rtmp_viewer.h"
#include "rtmp_viewer_session.h"
#include "voice_player.h"
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>

namespace {
using namespace rtmpview;
class SurfaceSink {
  Session& s;
  uint64_t surface;
  OH_AVCodec* decoder = nullptr;
  OHNativeWindow* window = nullptr;
  bool checked = false, pictureSubmitted = false, haveAudio = false, audioFailed = false;
  std::unique_ptr<ipcam::VoicePlayer> player;
  void open() {
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface, &window) != 0 || !window) throw DecoderError;
    decoder = OH_VideoDecoder_CreateByMime("video/avc");
    if (!decoder) throw DecoderError;
    std::unique_ptr<OH_AVFormat, decltype(&OH_AVFormat_Destroy)> fmt(
        OH_AVFormat_CreateVideoFormat("video/avc", liveavc::kMaxWidth, liveavc::kMaxHeight),
        OH_AVFormat_Destroy);
    if (!fmt || !OH_AVFormat_SetIntValue(fmt.get(), OH_MD_KEY_ENABLE_SYNC_MODE, 1) ||
        OH_VideoDecoder_Configure(decoder, fmt.get()) != AV_ERR_OK ||
        OH_VideoDecoder_SetSurface(decoder, window) != AV_ERR_OK ||
        OH_VideoDecoder_Prepare(decoder) != AV_ERR_OK || OH_VideoDecoder_Start(decoder) != AV_ERR_OK)
      throw DecoderError;
  }
  void format() {
    std::unique_ptr<OH_AVFormat, decltype(&OH_AVFormat_Destroy)> fmt(
        OH_VideoDecoder_GetOutputDescription(decoder), OH_AVFormat_Destroy);
    int32_t w = 0, h = 0;
    if (!fmt || !OH_AVFormat_GetIntValue(fmt.get(), OH_MD_KEY_VIDEO_PIC_WIDTH, &w) ||
        !OH_AVFormat_GetIntValue(fmt.get(), OH_MD_KEY_VIDEO_PIC_HEIGHT, &h) || !liveavc::fitsPicture(w, h))
      throw VideoCodec;
    checked = true;
  }
  void drain(Clock::time_point& last) {
    for (unsigned i = 0; i < 32 && decoder; ++i) {
      if (s.stopped) throw Stopped;
      uint32_t index = 0;
      auto rc = OH_VideoDecoder_QueryOutputBuffer(decoder, &index, 0);
      if (rc == AV_ERR_TRY_AGAIN_LATER) return;
      if (rc == AV_ERR_STREAM_CHANGED) {
        format();
        continue;
      }
      if (rc != AV_ERR_OK) throw DecoderError;
      auto* buf = OH_VideoDecoder_GetOutputBuffer(decoder, index);
      OH_AVCodecBufferAttr attr{};
      if (!buf || OH_AVBuffer_GetBufferAttr(buf, &attr) != AV_ERR_OK) throw DecoderError;
      constexpr auto nonPicture = AVCODEC_BUFFER_FLAGS_EOS | AVCODEC_BUFFER_FLAGS_CODEC_DATA |
                                  AVCODEC_BUFFER_FLAGS_DISCARD | AVCODEC_BUFFER_FLAGS_INCOMPLETE_FRAME;
      if (!pictureSubmitted || (attr.flags & nonPicture)) {
        if (OH_VideoDecoder_FreeOutputBuffer(decoder, index) != AV_ERR_OK) throw DecoderError;
        continue;
      }
      if (!checked) format();
      if (OH_VideoDecoder_RenderOutputBuffer(decoder, index) != AV_ERR_OK) throw DecoderError;
      last = Clock::now();
      s.state = Playing;
    }
  }
  void updateAudio() {
    if (!s.listen || !haveAudio || audioFailed) {
      player.reset();
      if (haveAudio) s.audio = audioFailed ? -2 : 0;
    } else if (s.audio <= 0)
      s.audio = 1;
  }

 public:
  SurfaceSink(Session& session, uint64_t id) : s(session), surface(id) {}
  ~SurfaceSink() {
    if (decoder) OH_VideoDecoder_Destroy(decoder);
    if (window) OH_NativeWindow_DestroyNativeWindow(window);
  }
  void poll(Clock::time_point& last) {
    updateAudio();
    drain(last);
  }
  void video(const Bytes& au, int64_t pts, bool config, Clock::time_point& last) {
    if (!decoder) open();  // only after an admitted AVC configuration
    auto until = std::min(last + 12s, Clock::now() + 2s);
    uint32_t index = 0;
    for (;;) {
      check(s, until);
      drain(last);
      auto rc = OH_VideoDecoder_QueryInputBuffer(decoder, &index, 10000);
      if (rc == AV_ERR_OK) break;
      if (rc != AV_ERR_TRY_AGAIN_LATER) throw DecoderError;
    }
    auto* buf = OH_VideoDecoder_GetInputBuffer(decoder, index);
    int cap = buf ? OH_AVBuffer_GetCapacity(buf) : 0;
    auto* addr = buf ? OH_AVBuffer_GetAddr(buf) : nullptr;
    if (!addr || cap <= 0 || au.size() > size_t(cap)) throw DecoderError;
    std::memcpy(addr, au.data(), au.size());
    OH_AVCodecBufferAttr attr{};
    attr.size = int32_t(au.size());
    attr.pts = pts;
    attr.flags = config ? AVCODEC_BUFFER_FLAGS_CODEC_DATA : AVCODEC_BUFFER_FLAGS_NONE;
    if (OH_AVBuffer_SetBufferAttr(buf, &attr) != AV_ERR_OK ||
        OH_VideoDecoder_PushInputBuffer(decoder, index) != AV_ERR_OK)
      throw DecoderError;
    pictureSubmitted |= !config;
    drain(last);
  }
  void audioPacket(Aac::Result result, const Bytes& adts) {
    if (result == Aac::Unsupported) {
      haveAudio = false;
      player.reset();
      s.audio = -3;
      return;
    }
    haveAudio = true;
    updateAudio();
    if (result == Aac::Config || !s.listen || audioFailed) return;
    if (!player) player = std::make_unique<ipcam::VoicePlayer>();
    auto rc = player->Feed(adts.data(), adts.size());
    using R = ipcam::VoicePlayer::FeedResult;
    if (rc == R::Accepted)
      s.audio = 2;
    else if (rc != R::Busy) {
      audioFailed = true;
      updateAudio();
    }
  }
};

using Handle = std::shared_ptr<Session>;
constexpr napi_type_tag Tag{0x283313098cfb41a3ULL, 0x93de9c2e6ff48beaULL};
std::mutex registryMu;
std::map<uint32_t, Handle> registry;
uint32_t nextId = 0;
std::atomic<unsigned> workers{0};
void remove(uint32_t id) {
  std::lock_guard<std::mutex> lock(registryMu);
  auto it = registry.find(id);
  if (it != registry.end()) {
    it->second->stopped = true;
    registry.erase(it);
  }
}
void finalize(napi_env, void* data, void*) {
  std::unique_ptr<uint32_t> id(static_cast<uint32_t*>(data));
  remove(*id);
}
// Do not napi_wrap/create_external: other module exports would interpret the pointer as a server.
napi_value make(napi_env e, const Handle& s) {
  napi_value obj{}, value{};
  auto id = std::make_unique<uint32_t>();
  if (napi_create_object(e, &obj) != napi_ok || napi_type_tag_object(e, obj, &Tag) != napi_ok) throw Protocol;
  {
    std::lock_guard<std::mutex> lock(registryMu);
    if (registry.size() >= 16 || nextId == UINT32_MAX) throw Busy;
    *id = ++nextId;
    registry.emplace(*id, s);
  }
  napi_property_descriptor d{};
  d.utf8name = "viewerId";
  if (napi_create_uint32(e, *id, &value) == napi_ok) d.value = value;
  if (!d.value || napi_define_properties(e, obj, 1, &d) != napi_ok ||
      napi_add_finalizer(e, obj, id.get(), finalize, nullptr, nullptr) != napi_ok) {
    remove(*id);
    throw Protocol;
  }
  id.release();
  return obj;
}
std::string text(napi_env e, napi_value v, size_t max) {
  size_t n = 0;
  if (napi_get_value_string_utf8(e, v, nullptr, 0, &n) != napi_ok || n > max) throw Protocol;
  std::string out(n + 1, '\0');
  if (napi_get_value_string_utf8(e, v, out.data(), out.size(), &n) != napi_ok) throw Protocol;
  out.resize(n);
  return out;
}
napi_value start(napi_env e, napi_callback_info info) {
  size_t n = 3;
  napi_value a[3]{};
  try {
    if (napi_get_cb_info(e, info, &n, a, nullptr, nullptr) != napi_ok || n != 2) throw Protocol;
    auto url = text(e, a[0], 4096), surface = text(e, a[1], 20);
    uint64_t id = 0;
    auto r = std::from_chars(surface.data(), surface.data() + surface.size(), id);
    if (r.ec != std::errc() || r.ptr != surface.data() + surface.size() || !id) throw Protocol;
    auto h = std::make_shared<Session>();
    auto obj = make(e, h);
    try {
      auto ep = endpoint(url);
      if (workers.fetch_add(1) >= 2) {
        --workers;
        throw Busy;
      }
      try {
        std::thread([h, ep = std::move(ep), id] {
          ReleaseSlot slot{workers};
          try {
            SurfaceSink sink(*h, id);
            play(*h, ep, sink);
          } catch (Status st) {
            h->state = st;
          } catch (...) {
            h->state = Network;
          }
          h->audio = -1;
        }).detach();
      } catch (...) {
        --workers;
        throw;
      }
    } catch (Status st) {
      h->state = st;
    } catch (...) {
      h->state = Network;
    }
    return obj;
  } catch (Status st) {
    napi_throw_error(e, nullptr,
                     st == Busy ? "Too many RTMP viewer handles" : "Invalid RTMP viewer arguments");
  } catch (...) {
    napi_throw_error(e, nullptr, "Cannot create RTMP viewer");
  }
  return nullptr;
}
Handle find(napi_env e, napi_callback_info info, uint32_t& id, bool* on = nullptr) {
  size_t n = 3;
  napi_value a[3]{}, v{};
  bool tagged = false;
  napi_valuetype type{};
  if (napi_get_cb_info(e, info, &n, a, nullptr, nullptr) != napi_ok || n != (on ? 2u : 1u) ||
      napi_typeof(e, a[0], &type) != napi_ok || type != napi_object ||
      napi_check_object_type_tag(e, a[0], &Tag, &tagged) != napi_ok || !tagged ||
      (on && napi_get_value_bool(e, a[1], on) != napi_ok) ||
      napi_get_named_property(e, a[0], "viewerId", &v) != napi_ok ||
      napi_get_value_uint32(e, v, &id) != napi_ok)
    return {};
  std::lock_guard<std::mutex> lock(registryMu);
  auto it = registry.find(id);
  return it == registry.end() ? Handle{} : it->second;
}
napi_value status(napi_env e, napi_callback_info i) {
  uint32_t id = 0;
  auto h = find(e, i, id);
  napi_value v{};
  napi_create_int32(e, h ? h->state.load() : id ? Stopped : Protocol, &v);
  return v;
}
napi_value audioStatus(napi_env e, napi_callback_info i) {
  uint32_t id = 0;
  auto h = find(e, i, id);
  napi_value v{};
  napi_create_int32(e, h && h->state >= 0 ? h->audio.load() : -1, &v);
  return v;
}
napi_value setAudio(napi_env e, napi_callback_info i) {
  bool on = false;
  uint32_t id = 0;
  auto h = find(e, i, id, &on);
  if (h) h->listen = on;
  napi_value v{};
  napi_get_undefined(e, &v);
  return v;
}
napi_value stop(napi_env e, napi_callback_info i) {
  uint32_t id = 0;
  if (find(e, i, id)) remove(id);
  napi_value v{};
  napi_get_undefined(e, &v);
  return v;
}
}  // namespace

void RegisterRtmpViewer(napi_env e, napi_value exports) {
  napi_property_descriptor d[] = {
      {"rtmpViewerStart", nullptr, start, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtmpViewerStatus", nullptr, status, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtmpViewerAudioStatus", nullptr, audioStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtmpViewerSetAudio", nullptr, setAudio, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"rtmpViewerStop", nullptr, stop, nullptr, nullptr, nullptr, napi_default, nullptr}};
  if (napi_define_properties(e, exports, sizeof(d) / sizeof(*d), d) != napi_ok)
    napi_throw_error(e, nullptr, "Cannot register RTMP viewer");
}
