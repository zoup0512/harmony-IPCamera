#include "rtmp_tls_napi.h"
#include "rtmp_tls_pipe.h"
#include <cmath>
#include <cstring>

namespace {
using ipcam::RtmpTlsPipe;
napi_value Fail(napi_env e) { napi_throw_error(e, nullptr, "RTMPS transport closed or unavailable"); return nullptr; }
uint32_t Token(napi_env e, napi_value v) {
  double n = 0;
  if (!v || napi_get_value_double(e, v, &n) != napi_ok || !(n >= 1 && n <= UINT32_MAX) || n != std::floor(n)) return 0;
  return static_cast<uint32_t>(n);
}
napi_value Open(napi_env e, napi_callback_info i) {
  size_t argc = 1, size = 0, used = 0; napi_value arg{}, result{}; char buf[513];
  if (napi_get_cb_info(e, i, &argc, &arg, nullptr, nullptr) != napi_ok || argc != 1 ||
      napi_get_value_string_utf8(e, arg, nullptr, 0, &size) != napi_ok || !size || size >= sizeof(buf) ||
      napi_get_value_string_utf8(e, arg, buf, sizeof(buf), &used) != napi_ok || used != size || std::memchr(buf, 0, size)) return Fail(e);
  auto& p = RtmpTlsPipe::Instance(); const auto id = p.Open(std::string(buf, size), e);
  if (!id) return Fail(e);
  if (napi_create_uint32(e, id, &result) != napi_ok) { p.Close(id, e); return Fail(e); }
  return result;
}
napi_value Read(napi_env e, napi_callback_info i) {
  size_t argc = 1; napi_value arg{}, result{}; uint8_t buf[RtmpTlsPipe::kRead];
  if (napi_get_cb_info(e, i, &argc, &arg, nullptr, nullptr) != napi_ok || argc != 1) return Fail(e);
  ssize_t n = RtmpTlsPipe::Instance().Read(Token(e, arg), e, buf, sizeof(buf));
  if (n < 0) return Fail(e);
  if (!n) { napi_get_undefined(e, &result); return result; }
  void* dst = nullptr;
  if (napi_create_arraybuffer(e, n, &dst, &result) != napi_ok) return Fail(e);
  std::memcpy(dst, buf, n); return result;
}
napi_value Write(napi_env e, napi_callback_info i) {
  size_t argc = 2, n = 0; napi_value args[2]{}, result{}; void* data = nullptr;
  if (napi_get_cb_info(e, i, &argc, args, nullptr, nullptr) != napi_ok || argc != 2 ||
      napi_get_arraybuffer_info(e, args[1], &data, &n) != napi_ok || !data ||
      !RtmpTlsPipe::Instance().Write(Token(e, args[0]), e, data, n)) return Fail(e);
  napi_get_undefined(e, &result); return result;
}
napi_value Close(napi_env e, napi_callback_info i) {
  size_t argc = 1; napi_value arg{}, result{};
  if (napi_get_cb_info(e, i, &argc, &arg, nullptr, nullptr) == napi_ok && argc == 1) RtmpTlsPipe::Instance().Close(Token(e, arg), e);
  napi_get_undefined(e, &result); return result;
}
}
napi_status RegisterRtmpTls(napi_env env, napi_value exports) {
  napi_property_descriptor methods[] = {
    {"rtmpTlsOpen", nullptr, Open, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"rtmpTlsRead", nullptr, Read, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"rtmpTlsWrite", nullptr, Write, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"rtmpTlsClose", nullptr, Close, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_status st = napi_add_env_cleanup_hook(env, [](void* e) { RtmpTlsPipe::Instance().Cleanup(e); }, env);
  return st == napi_ok ? napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods) : st;
}
