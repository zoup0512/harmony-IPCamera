#pragma once
#include <napi/native_api.h>

// Existing entry points use interleaved TCP. Explicit *Udp variants use one unicast video track.
// Basic/anonymous and challenge-only Digest are separate modes on both transports.
void RegisterRtspClient(napi_env env, napi_value exports);
