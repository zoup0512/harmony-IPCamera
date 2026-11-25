#pragma once
#include <napi/native_api.h>
#include "cloud_archive_pin.h"

namespace cloud_archive_napi {
struct Lease { std::string root; std::unique_ptr<ipcam::CloudArchivePin> pin; };
inline constexpr napi_type_tag kTag{0xb3236acc3b854403ULL,0x86ef498dc4f61603ULL};
enum class Op { Current, Remove, Close };
template<Op op> napi_value Use(napi_env env, napi_callback_info info) {
  napi_value self{}, out{};
  bool tagged = false, ok = false;
  Lease* l = nullptr;
  if (napi_get_cb_info(env,info,nullptr,nullptr,&self,nullptr) == napi_ok &&
      napi_check_object_type_tag(env,self,&kTag,&tagged) == napi_ok && tagged &&
      napi_unwrap(env,self,reinterpret_cast<void**>(&l)) == napi_ok && l && l->pin) {
    if (op != Op::Close) ok = l->pin->Check(ipcam::SafeArchiveDir::OpenAppRoot(l->root), op == Op::Remove);
    if (op != Op::Current || !ok) l->pin.reset();
  }
  napi_get_boolean(env,ok,&out);
  return out;
}
inline bool String(napi_env env, napi_value arg, std::string& out) {
  char buf[256]; size_t len = 0, copied = 0;
  if (napi_get_value_string_utf8(env,arg,nullptr,0,&len) != napi_ok || len == 0 || len >= sizeof(buf) ||
      napi_get_value_string_utf8(env,arg,buf,sizeof(buf),&copied) != napi_ok || copied != len) return false;
  out.assign(buf,len);
  return out.find('\0') == out.npos;
}
inline napi_value Pin(napi_env env, napi_callback_info info) {
  napi_value args[3]{}, out{}, absent{}; size_t argc = 3;
  napi_get_undefined(env,&absent);
  std::string dir,name; int32_t fd = -1;
  if (napi_get_cb_info(env,info,&argc,args,nullptr,nullptr) != napi_ok || argc != 3 ||
      !String(env,args[0],dir) || !String(env,args[1],name) || napi_get_value_int32(env,args[2],&fd) != napi_ok || fd < 0) return absent;
  auto pin = ipcam::CloudArchivePin::Pin(ipcam::SafeArchiveDir::OpenAppRoot(dir),name,fd);
  if (!pin) return absent;
  napi_property_descriptor methods[] = {
    {"current",nullptr,Use<Op::Current>,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"remove",nullptr,Use<Op::Remove>,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"close",nullptr,Use<Op::Close>,nullptr,nullptr,nullptr,napi_default,nullptr}
  };
  std::unique_ptr<Lease> l(new Lease{dir,std::move(pin)});
  if (napi_create_object(env,&out) != napi_ok || napi_type_tag_object(env,out,&kTag) != napi_ok ||
      napi_define_properties(env,out,3,methods) != napi_ok ||
      napi_wrap(env,out,l.get(),[](napi_env, void* ptr, void*) { delete static_cast<Lease*>(ptr); },nullptr,nullptr) != napi_ok) return absent;
  l.release();
  return out;
}
inline napi_status Register(napi_env env, napi_value exports) {
  napi_property_descriptor p{"pinCloudRecording",nullptr,Pin,nullptr,nullptr,nullptr,napi_default,nullptr};
  return napi_define_properties(env,exports,1,&p);
}
} // namespace cloud_archive_napi
