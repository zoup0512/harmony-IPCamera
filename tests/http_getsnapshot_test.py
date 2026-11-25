"""Linux/WSL: python3 tests/http_getsnapshot_test.py -v. AF_UNIX only; no listener or device."""
from concurrent.futures import ThreadPoolExecutor
from http.client import HTTPResponse
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import unittest

from media_cache_reset_test import CPP, SDK, TESTS


DRIVER = r'''
#include "jpeg_encoder.h"
#include "opus_stream.h"
#include "web_stream.h"
#include <algorithm>
#include <cassert>
#include <thread>
#define private public
#include "http_server.h"
#undef private
#include <cstdio>
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
using namespace ipcam;
void BeforeDecodedOutput(uint8_t) {}  // synthetic NV12 from media_cache_reset_test.SDK
bool OpusStream::EnsureReady() { return false; }
bool OpusStream::Serve(int, const std::atomic<bool>&) { return false; }
// Force filename collisions without affecting the parser's monotonic deadlines.
extern "C" int __real_clock_gettime(clockid_t, timespec*);
extern "C" int __wrap_clock_gettime(clockid_t id, timespec* ts) {
  if (id != CLOCK_REALTIME) return __real_clock_gettime(id, ts);
  *ts = {1704067200, 123000000};
  return 0;
}
int main(int argc, char** argv) {
  if (argc != 3) return 1;
  WebStream web;
  HttpServer server;
  server.user_ = "user";
  server.password_ = "pass";
  server.archiveDir_ = argv[1]; // Start normally sets this from the app's filesDir.
  std::string mode(argv[2]);
  if (mode == "partial") {
    std::signal(SIGXFSZ, SIG_IGN);
    rlimit lim{32, 32};
    if (setrlimit(RLIMIT_FSIZE, &lim) != 0) return 2;
  }
  server.running_ = true;
  server.SetJpegSource([] {
    std::puts("cached");
    uint8_t nv12[16 * 16 * 3 / 2] = {};
    return JpegEncoder::EncodeNV12(nv12, 16, 16);
  });
  if (mode != "missing") server.SetFreshJpegSource([&](const std::atomic<bool>& running) -> std::vector<uint8_t> {
    std::puts("fresh");
    if (!running || mode == "empty") return {};
    if (mode == "throw") throw 1;
    if (mode == "stop") server.running_ = false;
    uint8_t nv12[16 * 16 * 3 / 2];
    std::fill_n(nv12, sizeof(nv12), 128);
    auto jpg = JpegEncoder::EncodeNV12(nv12, 16, 16);
    if (mode == "truncated") jpg.pop_back();
    return jpg;
  });
  if (mode == "timeout") {
    web.SetParams(false, {0,0,1,0x67,0x64,0,0x1f,0,0,1,0x68,0xce}, 16, 16);
    web.IncClients();
    const uint8_t idr[]{0,0,1,0x65,64};
    web.FeedVideo(idr, sizeof(idr));
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (web.LatestJpeg().empty()) {
      assert(std::chrono::steady_clock::now() < limit);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.SetJpegSource([&]{ std::puts("cached"); return web.LatestJpeg(); });
    server.SetFreshJpegSource([&](const std::atomic<bool>& running) {
      std::puts("fresh");
      return web.WaitForFreshJpeg(running, std::chrono::milliseconds(75));
    });
  }
  server.HandleConnection(STDIN_FILENO);
}
'''


@unittest.skipUnless(os.name == 'posix', 'Linux/WSL host required')
class GetSnapshot(unittest.TestCase):
    auth = 'Authorization: Basic dXNlcjpwYXNz'
    fixed = 'IPC_2024-01-01.00.00.00.0123'

    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        build = Path(tmp.name)
        stubs = build / 'multimedia/player_framework'
        stubs.mkdir(parents=True)
        (stubs / 'fake.h').write_text(SDK + '\nstruct OH_AVMemory;\n')
        for header in ('avbuffer', 'avcodec_base', 'avcodec_videodecoder', 'avformat'):
            (stubs / f'native_{header}.h').write_text('#include "fake.h"\n')
        (build / 'hilog').mkdir()
        (build / 'hilog/log.h').write_text('#define OH_LOG_Print(...) ((void)0)\n')
        (build / 'driver.cpp').write_text(DRIVER)
        cls.exe = build / 'driver'
        cc = [os.getenv('CXX', 'c++'), *shlex.split(os.getenv('CXXFLAGS', '')), '-std=c++17',
              '-pthread', '-I', str(build), '-I', str(CPP), str(CPP / 'web_stream.cpp')]
        subprocess.run([*cc, str(CPP / 'http_server.cpp'), str(CPP / 'jpeg_encoder.cpp'),
                        str(build / 'driver.cpp'), '-Wl,--wrap=clock_gettime', '-o', str(cls.exe)],
                       check=True, timeout=90)
        cls.fence_test = build / 'fresh'
        subprocess.run([*cc, '-static-libstdc++', str(TESTS / 'fresh_snapshot_host.cpp'),
                        '-Wl,--wrap=pthread_cond_timedwait,--wrap=pthread_cond_clockwait',
                        '-o', str(cls.fence_test)], check=True, timeout=90)

    def test_webstream_input_fence(self):
        subprocess.run([self.fence_test], check=True, cwd=self.root, timeout=25)

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'files'
        self.root.mkdir()

    def get(self, path='/getsnapshot', method='GET', mode='ok', auth=True, extra=(), root=None):
        client, peer = socket.socketpair()
        with client, peer, subprocess.Popen([self.exe, self.root if root is None else root, mode],
                                            stdin=peer, stdout=subprocess.PIPE, env=dict(os.environ, TZ='UTC')) as p:
            peer.close()
            client.settimeout(5)
            lines = [f'{method} {path} HTTP/1.1', 'Host: localhost'] + ([self.auth] if auth else [])
            client.sendall('\r\n'.join([*lines, *extra, '', '']).encode())
            client.shutdown(socket.SHUT_WR)
            with HTTPResponse(client) as resp:
                resp.begin()
                body = resp.fp.read()
                self.assertEqual(int(resp.headers['Content-Length']), len(body))
            calls = p.communicate(timeout=5)[0].splitlines()
            self.assertEqual(p.returncode, 0)
            return resp.status, resp.headers, body, calls

    def test_read_vs_save(self):
        code, h, _, calls = self.get('/snapshot.jpg?pw=pass', auth=False)
        self.assertEqual((code, calls), (401, []))
        self.assertIn('Basic', h['WWW-Authenticate'])
        code, h, jpg, calls = self.get('/snapshot.jpg')
        self.assertEqual((code, h['Content-Type'], calls), (200, 'image/jpeg', [b'cached']))
        self.assertEqual(list(self.root.iterdir()), [])
        outside = self.root.parent / 'outside.jpg'
        code, h, body, calls = self.get('/GETSNAPSHOT?path=' + str(outside))
        name = self.fixed + '.jpg'
        self.assertEqual((code, h['Content-Type'], h['Cache-Control'], calls), (200, 'text/plain', 'no-store', [b'fresh']))
        self.assertEqual(body.decode(), 'Snapshot ' + name)
        saved = self.root / name
        self.assertEqual(list(self.root.iterdir()), [saved])
        self.assertTrue(len(jpg) > 32 and jpg.startswith(b'\xff\xd8') and jpg.endswith(b'\xff\xd9'))
        fresh = saved.read_bytes()
        self.assertTrue(len(fresh) > 32 and fresh.startswith(b'\xff\xd8') and fresh.endswith(b'\xff\xd9'))
        self.assertNotEqual(fresh, jpg)
        self.assertFalse(outside.exists())

    def test_rejected_before_capture(self):
        for path, method, auth, extra, expected in [
            ('/getsnapshot', 'GET', False, (), 401),
            ('/getsnapshot?pw=pass', 'GET', False, (), 401),
            ('/getsnapshot', 'GET', False, ('Authorization: Basic YmFk',), 401),
            ('/getsnapshot', 'HEAD', True, (), 405),
            ('/getsnapshot', 'POST', True, (), 405),
            ('/getsnapshot', 'GET', True, ('Content-Length: 4',), 400),
            ('/getsnapshot/foo.jpg', 'GET', True, (), 404),
        ]:
            with self.subTest(path=path, method=method, extra=extra):
                code, h, _, calls = self.get(path, method, auth=auth, extra=extra)
                self.assertEqual((code, calls), (expected, []))
                if code == 405:
                    self.assertEqual(h['Allow'], 'GET')
                if code == 401:
                    self.assertIn('Basic', h['WWW-Authenticate'])
        self.assertEqual(list(self.root.iterdir()), [])

    def test_stalled_input_with_real_cached_jpeg(self):
        code, _, body, calls = self.get(mode='timeout')
        self.assertEqual((code, calls), (503, [b'fresh']))
        self.assertNotIn(b'Snapshot ', body)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_missing_frame_and_io_failure_cleanup(self):
        for mode in ['empty', 'missing', 'truncated', 'throw', 'stop', 'partial']:
            with self.subTest(mode=mode):
                code, _, body, _ = self.get(mode=mode)
                self.assertEqual(code, 500 if mode == 'partial' else 503)
                self.assertNotIn(b'Snapshot ', body)
                self.assertEqual(list(self.root.iterdir()), [])
        alias = self.root.parent / 'alias'
        alias.symlink_to(self.root, target_is_directory=True)
        for root in ['', str(self.root / 'missing'), str(alias)]:
            self.assertEqual(self.get(root=root)[0], 500)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_concurrent_name_collisions_do_not_overwrite(self):
        outside = self.root.parent / 'untouched'
        outside.write_bytes(b'keep')
        (self.root / (self.fixed + '.jpg')).symlink_to(outside)
        with ThreadPoolExecutor(8) as pool:
            replies = list(pool.map(lambda _: self.get(), range(8)))
        names = set()
        for code, _, body, calls in replies:
            self.assertEqual((code, calls), (200, [b'fresh']))
            self.assertRegex(body.decode(), r'^Snapshot IPC_[\d.\-]+_\d+\.jpg$')
            name = body.decode().split()[1]
            self.assertGreater((self.root / name).stat().st_size, 32)
            names.add(name)
        self.assertEqual(len(names), 8)
        self.assertEqual(outside.read_bytes(), b'keep')
        self.assertEqual({p.name for p in self.root.iterdir()}, names | {self.fixed + '.jpg'})


if __name__ == '__main__':
    unittest.main()
