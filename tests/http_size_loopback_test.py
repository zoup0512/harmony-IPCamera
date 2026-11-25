"""Linux/WSL: python3 tests/http_size_loopback_test.py -v (C++17, no device).

Real HTTP parser, authentication, workers and routes on an inherited loopback-only
listener. Only camera/codec callbacks and Harmony logging are stubbed.
"""
import base64
import contextlib
import http.client
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import time
import unittest


DRIVER = r'''
#include "opus_stream.h"
#include <thread>
#define private public
#include "http_server.h"
#undef private
#include <cstdlib>
#include <cstdio>
using namespace ipcam;
bool OpusStream::EnsureReady() { return false; }
bool OpusStream::Serve(int, const std::atomic<bool>&) { return false; }
int main(int argc, char** argv) {
  if (argc != 3) return 1;
  using Result = HttpServer::SizeResult;
  const std::string mode = argv[2];
  std::atomic<int> current{1}, listCalls{0}, switchCalls{0};
  HttpServer server;
  server.user_ = "user";
  server.password_ = "pass";
  if (mode == "legacy") server.allowLegacyVideoPassword_ = true;
  server.SetInfoSource([&] { return std::to_string(listCalls.load()) + "," +
    std::to_string(switchCalls.load()) + "," + std::to_string(current.load()); });
  if (mode != "no-frames") server.SetJpegSource([] { return std::vector<uint8_t>{1, 2, 3}; });
  if (mode != "unwired") server.SetSizeCallbacks([&](HttpServer::SizeList& list) {
    ++listCalls;
    if (mode == "closed") return Result::CameraClosed;
    if (mode != "empty") { list.sizes = {{344, 222}, {768, 432}}; list.currentIndex = current.load(); }
    return Result::Ok;
  }, [&](int index) {
    ++switchCalls;
    if (mode == "closed") return Result::CameraClosed;
    if (mode == "busy") return Result::Busy;
    if (index >= 2 || index < 0 || mode == "empty") return Result::NotFound;
    if (mode == "failed") return Result::RestartFailed;
    current.store(index);
    return Result::Ok;
  });
  server.listenFd_ = std::atoi(argv[1]);
  server.running_ = true;
  server.acceptThread_ = std::thread(&HttpServer::AcceptLoop, &server);
  std::getchar(); // EOF stops the server and joins its workers.
}
'''


@unittest.skipUnless(os.name == 'posix', 'Linux or WSL required')
class SizeLoopbackTest(unittest.TestCase):
    auth = 'bAsIc ' + base64.b64encode(b'user:pass').decode()

    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix='http-size-')
        cls.addClassCleanup(temp.cleanup)
        work = Path(temp.name)
        native = Path(__file__).resolve().parents[1] / 'entry/src/main/cpp'
        codec = work / 'multimedia/player_framework/native_avcodec_base.h'
        codec.parent.mkdir(parents=True)
        codec.write_text('struct OH_AVCodec; struct OH_AVFormat; struct OH_AVMemory; struct OH_AVCodecBufferAttr;')
        (work / 'hilog').mkdir()
        (work / 'hilog/log.h').write_text('#define OH_LOG_Print(...) ((void)0)\n')
        (work / 'driver.cpp').write_text(DRIVER)
        subprocess.run([os.environ.get('CXX', 'c++'), *shlex.split(os.environ.get('CXXFLAGS', '')),
                        '-std=c++17', '-pthread', '-I.', '-I', str(native), str(native / 'http_server.cpp'),
                        'driver.cpp', '-o', 'driver'], cwd=work, check=True, timeout=90)
        cls.exe = work / 'driver'

    @contextlib.contextmanager
    def serving(self, mode='active'):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            self.address = listener.getsockname()
            with subprocess.Popen([self.exe, str(listener.fileno()), mode], stdin=subprocess.PIPE,
                                  pass_fds=(listener.fileno(),)) as proc:
                try:
                    yield
                finally:
                    proc.stdin.close()
                    self.assertEqual(proc.wait(timeout=10), 0)

    def get(self, path, *, method='GET', auth=auth):
        conn = http.client.HTTPConnection(*self.address, timeout=5)
        try:
            conn.request(method, path, headers={'aUThorization': auth} if auth else {})
            with conn.getresponse() as response:
                return response.status, response.headers, response.read()
        finally:
            conn.close()

    def test_wire_formats_dashboard_and_confirmed_switch(self):
        with self.serving():
            page = self.get('/')[2]
            self.assertIn(b'/size?format=json', page)
            self.assertIn(b"addEventListener('click', () => showSizes(index))", page)
            self.assertNotIn(b'innerHTML', page)
            self.assertNotIn(b'344x222', page)
            status, headers, fragment = self.get('/size')
            self.assertEqual((status, headers['Content-Type']), (200, 'text/plain'))
            self.assertIn(b"onItemClick('size0')", fragment)
            self.assertIn(b'data-sel="yes" onclick="return false;"><strong>768x432</strong>', fragment)
            status, headers, body = self.get('/size?format=json')
            self.assertEqual((status, headers['Content-Type'], headers['Cache-Control']),
                             (200, 'application/json', 'no-store'))
            self.assertEqual(json.loads(body), {'sizes': [{'width': 344, 'height': 222},
                                                         {'width': 768, 'height': 432}], 'currentIndex': 1})
            self.assertEqual(self.get('/serverinfo')[2], b'2,0,1')
            status, headers, body = self.get('/size0')
            self.assertEqual((status, headers['Cache-Control'], body), (200, 'no-store', b''))
            self.assertEqual(json.loads(self.get('/size?format=json')[2])['currentIndex'], 0)
            self.assertEqual(self.get('/serverinfo')[2], b'3,1,0')

    def test_only_basic_and_exact_get_can_reach_callbacks(self):
        with self.serving():
            for path in ['/size', '/size0']:
                for auth in [None, 'Bearer pass', 'Basic dXNlcjpXUk9ORw==']:
                    status, headers, _ = self.get(path + '?pw=pass', auth=auth)
                    self.assertEqual(status, 401)
                    self.assertIn('Basic', headers['WWW-Authenticate'])
                for method in ['HEAD', 'PUT', 'POST', 'OPTIONS']:
                    status, headers, _ = self.get(path, method=method)
                    self.assertEqual((status, headers['Allow']), (405, 'GET'))
            for path in ['/size-1', '/size+1', '/size01', '/size1/other', '/size1x', '/size/0',
                         '/size%30', '/size2147483648', '/size' + '9'*40, '/sizefoo']:
                self.assertEqual(self.get(path)[0], 400, path)
            for path in ['/Size', '/SIZE0', '/video/../size0']:
                self.assertEqual(self.get(path)[0], 404, path)
            self.assertEqual(self.get('/serverinfo')[2], b'0,0,1')
            self.assertEqual(self.get('/size2147483647')[0], 404)
            self.assertEqual(self.get('/serverinfo')[2], b'0,1,1')

    def test_camera_errors_are_not_success(self):
        for mode, expected in [('unwired', 503), ('closed', 503), ('empty', 404),
                               ('busy', 409), ('failed', 500)]:
            with self.subTest(mode=mode), self.serving(mode):
                status, _, body = self.get('/size0')
                self.assertEqual(status, expected)
                self.assertTrue(body)
                self.assertTrue(self.get('/serverinfo')[2].endswith(b',1'))
                status, _, listing = self.get('/size?format=json')
                if mode in ['unwired', 'closed']:
                    self.assertEqual(status, 503)
                elif mode == 'empty':
                    self.assertEqual(json.loads(listing), {'sizes': [], 'currentIndex': -1})

    @contextlib.contextmanager
    def viewer(self, legacy=False):
        with socket.create_connection(self.address, timeout=5) as sock:
            target = '/video?pw=pass' if legacy else '/video'
            header = '' if legacy else f'Authorization: {self.auth}\r\n'
            sock.sendall(f'GET {target} HTTP/1.1\r\nHost: localhost\r\n{header}\r\n'.encode())
            with sock.makefile('rb') as reader:
                self.assertIn(b'200 OK', reader.readline())
                yield

    def test_video_password_requires_opt_in_and_is_not_a_console_login(self):
        for mode in ['active', 'legacy']:
            with self.subTest(mode=mode), self.serving(mode):
                if mode == 'legacy':
                    with self.viewer(legacy=True):
                        pass
                else:
                    status, headers, _ = self.get('/video?pw=pass', auth=None)
                    self.assertEqual(status, 401)
                    self.assertIn('Basic', headers['WWW-Authenticate'])
                for path in ['/', '/serverinfo', '/size', '/size0', '/light', '/camswitch',
                             '/getsnapshot', '/snapshot.jpg', '/audio.opus', '/live.flv',
                             '/getarchives', '/get/archive.mp4', '/recordings', '/recordings/archive.mp4',
                             '/VIDEO', '/video/', '/video/../light']:
                    self.assertEqual(self.get(path + '?pw=pass', auth=None)[0], 401, path)
                self.assertEqual(self.get('/put_voice?pw=pass', method='PUT', auth=None)[0], 401)
                for method in ['HEAD', 'POST', 'PUT']:
                    self.assertEqual(self.get('/video?pw=pass', method=method, auth=None)[0], 401)
                for query in ['notpw=pass', 'pw=pass&pw=pass', 'pw=wrong', 'pw=']:
                    self.assertEqual(self.get('/video?' + query, auth=None)[0], 401, query)
                for auth in ['Bearer pass', 'Basic eDp5']:
                    self.assertEqual(self.get('/video?pw=pass', auth=auth)[0], 401)
                self.assertEqual(self.get('/serverinfo')[2], b'0,0,1')
                self.assertNotIn(b'pw=', self.get('/')[2])

    def test_counts_only_live_mjpeg_workers(self):
        for mode in ['active', 'no-frames']:
            with self.subTest(mode=mode), self.serving(mode), self.viewer():
                self.assertEqual(self.get('/size0')[0], 200)
                with self.viewer():
                    self.assertEqual(self.get('/size1')[0], 409)
                    self.assertEqual(self.get('/serverinfo')[2], b'0,1,0')  # no select callback
                    self.assertEqual(self.get('/size')[0], 200)
                until = time.monotonic() + 4
                while self.get('/size1')[0] == 409 and time.monotonic() < until:
                    time.sleep(0.05)
                self.assertEqual(self.get('/serverinfo')[2], b'1,2,1')


if __name__ == '__main__':
    unittest.main()
