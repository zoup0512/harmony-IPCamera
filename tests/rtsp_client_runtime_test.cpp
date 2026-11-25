// Standalone OHOS SDK executable. Link --wrap=getaddrinfo and the same N-API,
// window and video libraries as streaming. No application, camera or remote DNS needed.
#include "../entry/src/main/cpp/rtsp_client.cpp"
#include <cassert>
#include <condition_variable>
#include <iostream>

namespace {
std::mutex gate;
std::condition_variable changed;
int entered = 0;
bool releaseDns = false;
}
extern "C" int __real_getaddrinfo(const char*, const char*, const addrinfo*, addrinfo**);
extern "C" int __wrap_getaddrinfo(const char* host, const char* port, const addrinfo* hints, addrinfo** out) {
  if (hints->ai_flags & AI_NUMERICHOST) return __real_getaddrinfo(host, port, hints, out);
  std::unique_lock<std::mutex> lock(gate);
  ++entered;
  changed.notify_all();
  changed.wait(lock, [] { return releaseDns; });
  return EAI_AGAIN;
}

static void testResolver() {
  Session session;
  auto waiter = [&](Clock::time_point end) {
    try { resolve(session, "camera.test", "554", end); }
    catch (Status st) { return st; }
    return Playing;
  };
  auto waiting = std::async(std::launch::async, waiter, Clock::now() + 6s);
  {
    std::unique_lock<std::mutex> lock(gate);
    assert(changed.wait_for(lock, 2s, [] { return entered == 1; }));
  }
  session.stopped = true;
  assert(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == Network);
  session.stopped = false;
  assert(waiter(Clock::now() + 100ms) == Timeout);
  assert(waiter(Clock::now() + 2s) == Network); // full; never queues a third DNS job
  assert(resolve(session, "127.0.0.1", "554", Clock::now() + 1s));
  {
    std::lock_guard<std::mutex> lock(gate);
    releaseDns = true;
  }
  changed.notify_all();
  auto until = Clock::now() + 2s;
  while (resolvers.load() && Clock::now() < until) std::this_thread::yield();
  assert(!resolvers.load());
}

// Shell-only emulator images may not expose the Ark VM to standalone binaries.
// Compile with -DRTSP_DNS_ONLY -ffunction-sections -fdata-sections -Wl,--gc-sections
// to run the real resolver test without linking N-API or video libraries.
#ifndef RTSP_DNS_ONLY
static void testHandles() {
  napi_env env{};
  assert(napi_create_ark_runtime(&env) == napi_ok);
  napi_handle_scope scope{};
  assert(napi_open_handle_scope(env, &scope) == napi_ok);
  napi_value exports{}, statusFn{}, stopFn{}, unrelated{}, id{}, result{};
  assert(napi_create_object(env, &exports) == napi_ok);
  RegisterRtspClient(env, exports);
  assert(napi_get_named_property(env, exports, "liveAvcStatus", &statusFn) == napi_ok);
  assert(napi_get_named_property(env, exports, "liveAvcStop", &stopFn) == napi_ok);
  auto session = std::make_shared<Session>();
  auto genuine = makeHandle(env, session);
  assert(napi_create_object(env, &unrelated) == napi_ok);
  assert(napi_get_named_property(env, genuine, "playbackId", &id) == napi_ok);
  assert(napi_set_named_property(env, unrelated, "playbackId", id) == napi_ok);
  auto code = [&](napi_value obj) {
    int32_t value = 0;
    assert(napi_call_function(env, exports, statusFn, 1, &obj, &result) == napi_ok);
    assert(napi_get_value_int32(env, result, &value) == napi_ok);
    return value;
  };
  int unrelatedNativeState = 42;
  napi_value external{};
  assert(napi_create_external(env, &unrelatedNativeState, nullptr, nullptr, &external) == napi_ok);
  assert(code(external) == Protocol && code(unrelated) == Protocol && code(genuine) == Connecting);
  assert(napi_call_function(env, exports, stopFn, 1, &unrelated, &result) == napi_ok && !session->stopped);
  assert(napi_call_function(env, exports, stopFn, 1, &genuine, &result) == napi_ok && session->stopped);
  assert(napi_close_handle_scope(env, scope) == napi_ok);
  assert(napi_destroy_ark_runtime(&env) == napi_ok);
  assert(handles.empty());
}
#endif
int main() {
  testResolver();
  std::cout << "PASS: DNS cancellation, deadline, isolation and numeric fallback\n";
#ifndef RTSP_DNS_ONLY
  testHandles();
  std::cout << "PASS: Ark branded handle lifetime\n";
#endif
}
