"""Linux host regression: python3 tests/http_recordings_test.py -v (C++17, no device).

Exercise the production HTTP parser, authentication, router and file streaming via
socketpair, without a network listener. Only Harmony logging, Opus and application
callbacks are stubbed. Payload checks do not require a video decoder.
"""
import base64
from html.parser import HTMLParser
from http.client import HTTPResponse
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import unittest
from urllib.parse import quote


HARNESS = r'''
#include "opus_stream.h"
#include <thread>
#define private public
#include "http_server.h"
#undef private
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <unistd.h>
using namespace ipcam;
bool OpusStream::EnsureReady() { return false; }
bool OpusStream::Serve(int, const std::atomic<bool>&) { return false; }
int main(int argc, char** argv) {
  HttpServer server;
  server.running_ = true;
  server.user_ = "user";
  server.password_ = "pass";
  server.SetArchiveSource([] {
    std::ifstream list("listing");
    return std::string(std::istreambuf_iterator<char>(list), {});
  });
  server.SetArchiveFile([](const std::string& key) {
    std::printf("open:%s\n", key.c_str());
    std::string name = key.rfind("md/ips_", 0) == 0 ? "MD/IPS_" + key.substr(7) : key;
    return open(name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  });
  if (argc == 2) server.SetCommandSink([&](const std::string& cmd) {
    std::printf("cmd:%s\n", cmd.c_str());
    return cmd == argv[1];
  });
  server.HandleConnection(STDIN_FILENO);
}
'''


class Links(HTMLParser):
    def __init__(self, page):
        super().__init__()
        self.links = []
        self.videos = []
        self.feed(page.decode())

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'a':
            self.links.append(attrs['href'])
        elif tag == 'video' and 'controls' in attrs:
            self.videos.append(attrs['src'])


@unittest.skipUnless(os.name == 'posix', 'Linux host (or WSL) required')
class HttpRecordingsTest(unittest.TestCase):
    manual = 'ipc_2026-11-24_2.mp4'
    motion = '''MD/IPS_CaM 雪<&>"'?#%+.mp4'''
    auth = 'Basic ' + base64.b64encode(b'user:pass').decode()
    payload = bytes(range(251)) * 670  # More than two 64 KiB chunks.

    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix='ipcam-web-')
        cls.addClassCleanup(temp.cleanup)
        cls.work = Path(temp.name)
        native = Path(__file__).resolve().parents[1] / 'entry/src/main/cpp'
        codec = cls.work / 'multimedia/player_framework/native_avcodec_base.h'
        codec.parent.mkdir(parents=True)
        codec.write_text('struct OH_AVCodec; struct OH_AVFormat; struct OH_AVMemory; struct OH_AVCodecBufferAttr;')
        (cls.work / 'hilog').mkdir()
        (cls.work / 'hilog/log.h').write_text('#define OH_LOG_Print(...) ((void)0)\n')
        (cls.work / 'driver.cpp').write_text(HARNESS)
        cls.exe = cls.work / 'driver'
        subprocess.run([os.environ.get('CXX', 'c++'), *shlex.split(os.environ.get('CXXFLAGS', '')),
                        '-std=c++17', '-pthread', '-I.', '-I', str(native), str(native / 'http_server.cpp'),
                        'driver.cpp', '-o', 'driver'], cwd=cls.work, timeout=90, check=True)
        (cls.work / 'MD').mkdir()
        (cls.work / cls.manual).write_bytes(cls.payload)
        (cls.work / cls.motion).write_bytes(b'motion')
        (cls.work / 'ipc_empty.mp4').touch()
        (cls.work / 'ipc_dir.mp4').mkdir()
        cls.listing = (f'{cls.manual} {len(cls.payload)}\n{cls.motion} 6\n'
                       'ipc_skip.mp4.bak 12\nMD/IPS_../skip.mp4 42\nipc_bad.mp4 not-a-size\n')

    def setUp(self):
        (self.work / 'listing').write_text(self.listing, encoding='utf-8')

    def fetch(self, path, *, method='GET', auth=True, headers=(), command=None):
        client, server = socket.socketpair()
        with client, server, subprocess.Popen([self.exe] + ([command] if command else []),
                                              cwd=self.work, stdin=server, stdout=subprocess.PIPE) as proc:
            server.close()
            client.settimeout(5)
            lines = [f'{method} {path} HTTP/1.1', 'Host: localhost']
            if auth:
                lines.append('Authorization: ' + self.auth)
            client.sendall(('\r\n'.join([*lines, *headers, '', ''])).encode())
            client.shutdown(socket.SHUT_WR)
            # Read raw body even on HEAD to catch accidental file data.
            with HTTPResponse(client) as response:
                response.begin()
                body = response.fp.read()
                result = response.status, response.headers, body
            self.calls = proc.communicate(timeout=5)[0].decode().splitlines()
            self.assertEqual(proc.returncode, 0)
            return result

    def test_follow_links_for_both_recorders(self):
        homepage = Links(self.fetch('/')[2])
        self.assertIn('/recordings', homepage.links)
        status, _, page = self.fetch('/recordings')
        self.assertEqual(status, 200)
        self.assertIn(b'&lt;&amp;&gt;&quot;&#39;', page)
        self.assertNotIn(b'skip', page)
        self.assertNotIn(b'not-a-size', page)
        links = Links(page).links
        for name, data in [(self.manual, self.payload), (self.motion, b'motion')]:
            key = quote(name)
            play = '/recordings/play/' + key
            download = '/recordings/download/' + key
            self.assertIn(play, links)
            self.assertIn(download, links)
            player = Links(self.fetch(play)[2])
            self.assertIn(download, player.links)
            self.assertEqual(len(player.videos), 1)
            status, headers, body = self.fetch(player.videos[0])
            self.assertEqual((status, headers['Content-Type'], body), (200, 'video/mp4', data))
            status, headers, body = self.fetch(download)
            self.assertEqual((status, int(headers['Content-Length']), body), (200, len(data), data))
            self.assertEqual(headers['Content-Disposition'], "attachment; filename*=UTF-8''" + key.split('/')[-1])

    def test_auth_applies_to_all_archive_routes(self):
        for path in ['/recordings', '/recordings/play/', '/recordings/media/', '/recordings/download/',
                     '/getarchives', '/get/']:
            if path.endswith('/'):
                path += self.manual
            status, headers, _ = self.fetch(path + '?pw=pass', auth=False)
            self.assertEqual(status, 401, path)
            self.assertTrue(headers['WWW-Authenticate'].startswith('Basic '))
            self.assertFalse(self.calls)

    def test_path_and_missing_files(self):
        for name in ['ipc_../other.mp4', 'MD%2FIPS_a.mp4', 'MD/IPS_a%2fb.mp4', 'ipc_%2e%2e.mp4',
                     'ipc_a%5Cb.mp4', 'ipc_a%00.mp4', 'ipc_bad%2.mp4', 'ipc_%0d%0a.mp4',
                     '/ipc_a.mp4', 'other/IPS_a.mp4', 'ipc_a.mp4.tmp']:
            self.assertEqual(self.fetch('/recordings/media/' + name)[0], 404, name)
            self.assertFalse(self.calls, name)
        for name in ['ipc_missing.mp4', 'ipc_dir.mp4']:
            self.assertEqual(self.fetch('/recordings/download/' + name)[0], 404)
        self.assertEqual(self.fetch('/recordings/media/' + self.manual, method='POST')[0], 405)
        self.assertFalse(self.calls)

    def test_empty_and_legacy(self):
        self.assertEqual(self.fetch('/GETARCHIVES')[2], self.listing.encode())
        self.assertEqual(self.fetch('/GET/' + self.manual.upper())[2], self.payload)
        (self.work / 'listing').write_bytes(b'')
        self.assertIn(b'No recordings', self.fetch('/recordings')[2])
        self.assertEqual(self.fetch('/recordings/media/ipc_empty.mp4')[2], b'')
        status, h, _ = self.fetch('/recordings/media/ipc_empty.mp4', headers=['Range: bytes=0-'])
        self.assertEqual((status, h['Content-Range']), (416, 'bytes */0'))

    def test_ranges_and_head(self):
        size = len(self.payload)
        path = '/recordings/media/' + self.manual
        for r, start, end in [('0-0', 0, 0), ('100-70000', 100, 70000), ('-9', size-9, size-1),
                              ('999-', 999, size-1), ('4-' + '9'*25, 4, size-1)]:
            status, h, body = self.fetch(path, headers=['rAnGe: bytes=' + r])
            self.assertEqual((status, h['Content-Range']), (206, f'bytes {start}-{end}/{size}'))
            self.assertEqual(int(h['Content-Length']), end-start+1)
            self.assertEqual(body, self.payload[start:end+1])
        for ranges in [['bytes=' + r] for r in ['-0', '-', '2-1', f'{size}-', '0-2,4-5', 'a-b', '9'*25+'-']] + [
                ['bytes=0-0', 'bytes=1-1']]:
            status, h, body = self.fetch(path, headers=['Range: ' + r for r in ranges])
            self.assertEqual((status, h['Content-Range'], body), (416, f'bytes */{size}', b''))
        status, h, body = self.fetch(path, method='HEAD')
        self.assertEqual((status, h['Accept-Ranges'], int(h['Content-Length']), body), (200, 'bytes', size, b''))
        status, _, body = self.fetch(path, headers=['Range: bytes=1-1', 'If-Range: "old"'])
        self.assertEqual((status, body), (200, self.payload))

    def test_sparse_large_recording(self):
        path = self.work / 'ipc_large.mp4'
        offset = 2**32 + 123
        with path.open('wb') as file:
            file.seek(offset)
            file.write(b'end')
        status, h, body = self.fetch('/recordings/download/' + path.name, headers=[f'Range: bytes={offset}-'])
        self.assertEqual((status, h['Content-Range'], body), (206, f'bytes {offset}-{offset+2}/{offset+3}', b'end'))

    def test_control_success_and_failure(self):
        for cmd, success in [('light', b'light toggled'), ('camswitch', b'camera switched')]:
            path = '/' + cmd
            self.assertEqual(self.fetch(path)[0], 503)
            self.assertEqual(self.fetch(path, command='fail')[0], 503)
            self.assertEqual(self.calls, ['cmd:' + cmd])
            status, _, body = self.fetch(path, command=cmd)
            self.assertEqual((status, body), (200, success))
            self.assertEqual(self.fetch(path, command=cmd, auth=False)[0], 401)
            self.assertFalse(self.calls)


if __name__ == '__main__':
    unittest.main()
