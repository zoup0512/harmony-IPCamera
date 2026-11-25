"""Linux/WSL: python3 tests/http_flv_test.py -v. C++17 and ffprobe; no device/media input."""
import base64
import http.client
import json
import os
import select
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / 'entry/src/main/cpp'
AUTH = {'Authorization': 'Basic ' + base64.b64encode(b'user:pass').decode()}


def ue(v):
    b = bin(v + 1)[2:]
    return '0' * (len(b) - 1) + b


def pack(bits):
    bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def nal(kind, rbsp):
    escaped = bytearray()
    for b in rbsp:
        if len(escaped) >= 2 and escaped[-2:] == b'\0\0' and b <= 3:
            escaped.append(3)
        escaped.append(b)
    return bytes((0, 0, 0, 1, kind)) + escaped


# Hand-built baseline 16x16 gray I_PCM picture. No recorded or downloaded bytes.
SPS = nal(0x67, b'\x42\xc0\x0a' + pack(ue(0) + ue(0) + ue(2) + ue(1) + '0' + ue(0)*2 + '11001'))
PPS = nal(0x68, pack(ue(0)*2 + '00' + ue(0)*3 + '000' + ue(0)*3 + '0001'))
IDR = nal(0x65, pack(ue(0) + ue(2) + ue(0) + '0000' + ue(0) + '00' + ue(0) + ue(25)) +
          bytes([128])*384 + b'\x80')
def predicted(frame_num):  # one skipped MB, predicted from the previous gray picture
    return nal(0x41, pack(ue(0)*3 + format(frame_num, '04b') + '000' + ue(0) + ue(1) + '1'))


P = predicted(1)
# AAC-LC single_channel_element with max_sfb=0 (silence), ID_END and alignment.
SILENCE = pack('000' + '0000' + '01100100' + '0000' + '000000' + '0000' + '111')


def adts(sf=4):
    length = 7 + len(SILENCE)
    return bytes((255, 241, (1 << 6) | sf << 2, 64 | length >> 11, (length >> 3) & 255,
                  (length & 7) << 5 | 31, 252)) + SILENCE


@unittest.skipUnless(os.name == 'posix', 'Linux/WSL host required')
class HttpFlv(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix='http-flv-')
        cls.addClassCleanup(tmp.cleanup)
        work = Path(tmp.name)
        sdk = work / 'multimedia/player_framework/native_avcodec_base.h'
        sdk.parent.mkdir(parents=True)
        sdk.write_text('struct OH_AVCodec; struct OH_AVFormat; struct OH_AVMemory; struct OH_AVCodecBufferAttr;')
        (work / 'hilog').mkdir()
        (work / 'hilog/log.h').write_text('#define OH_LOG_Print(...) ((void)0)\n')
        cls.exe = work / 'driver'
        subprocess.run([os.environ.get('CXX', 'c++'), *shlex.split(os.environ.get('CXXFLAGS', '')),
                        '-std=c++17', '-pthread', '-I.', '-I', str(CPP), str(CPP / 'http_server.cpp'),
                        str(ROOT / 'tests/http_flv_host.cpp'), '-o', 'driver'], cwd=work, check=True, timeout=90)

    def setUp(self):
        s = socket.socket()
        self.addCleanup(s.close)
        s.bind(('127.0.0.1', 0)); s.listen()
        self.addr = s.getsockname()
        self.proc = subprocess.Popen([self.exe, str(s.fileno())], pass_fds=(s.fileno(),), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        def stop():
            self.proc.stdin.close()
            try:
                self.assertEqual(self.proc.wait(timeout=4), 0)
            finally:
                if self.proc.poll() is None:
                    self.proc.kill(); self.proc.wait()
                self.proc.stdout.close()
        self.addCleanup(stop)
        self.assertEqual(self.proc.stdout.readline().strip(), 'ready')

    def cmd(self, line):
        self.proc.stdin.write(line + '\n'); self.proc.stdin.flush()
        self.assertTrue(select.select([self.proc.stdout], [], [], 5)[0], 'fixture did not acknowledge command')
        return self.proc.stdout.readline().strip()

    def feed(self, frame, stamp, audio=False):
        self.assertEqual(self.cmd(f'{"a" if audio else "v"} {stamp} {frame.hex()}'), 'ok')

    def request(self, path='/live.flv', method='GET', headers=AUTH):
        conn = http.client.HTTPConnection(*self.addr, timeout=4)
        self.addCleanup(conn.close)
        conn.request(method, path, headers=headers)
        return conn

    def await_reader(self):
        for _ in range(100):
            if int(self.cmd('count')) > 0: return
            time.sleep(.01)
        self.fail('no FLV subscriber')

    def test_gates_auth_methods_and_console(self):
        for path in ['/live.flv', '/live.flv?pw=pass', '/live.flv?notpw=pass', '/live.flv?pw=pass&pw=pass']:
            r = self.request(path, headers={}).getresponse()
            self.assertEqual(r.status, 401); self.assertIn('Basic', r.getheader('WWW-Authenticate')); r.close()
        self.assertEqual(self.request(headers={'Authorization': 'Basic eDp5'}).getresponse().status, 401)
        for method in ['POST', 'HEAD']:
            r = self.request(method=method).getresponse()
            self.assertEqual((r.status, r.getheader('Allow')), (405, 'GET'))
        self.assertEqual(self.request().getresponse().status, 503)
        self.cmd('hevc')
        r = self.request().getresponse()
        self.assertEqual(r.status, 501); self.assertIn(b'HEVC', r.read())
        html = self.request('/').getresponse().read()
        self.assertIn(b"href='/live.flv'", html); self.assertIn(b'VLC', html)
        self.assertNotIn(b"<video src='/live.flv'", html)

    @staticmethod
    def tags(data):
        offset = 13
        while offset < len(data):
            h = data[offset:offset + 11]
            assert len(h) == 11 and h[8:] == b'\0\0\0'
            n = int.from_bytes(h[1:4], 'big')
            ts = int.from_bytes(h[7:8] + h[4:7], 'big')
            end = offset + 11 + n
            assert int.from_bytes(data[end:end + 4], 'big') == 11 + n
            yield h[0], ts, data[offset + 11:end]
            offset = end + 4
        assert offset == len(data)

    def join(self):
        self.feed(SPS + PPS + IDR, 0)  # before joining; must not replay stale IDR
        c = self.request()
        self.await_reader()
        self.feed(P, 500000); self.feed(adts(), 505000, True)  # no inter/audio before new IDR
        self.feed(IDR, 1000000); self.feed(adts(), 1010000, True)
        r = c.getresponse(); self.addCleanup(r.close)
        self.assertEqual((r.status, r.getheader('Content-Type'), r.getheader('Cache-Control')),
                         (200, 'video/x-flv', 'no-store'))
        return r

    def test_playable_avc_aac(self):
        probe = os.environ.get('FFPROBE') or shutil.which('ffprobe')
        if not probe: self.skipTest('install ffprobe or set FFPROBE to its path')
        r = self.join()
        for i in range(1, 6):
            self.feed(predicted(i), 1000000 + i*40000); self.feed(adts(), 1010000 + i*40000, True)
        data = self.finish(r, 14)  # avcC + 6 video, ASC + 6 AAC
        self.assertEqual(data[:13], b'FLV\x01\x05\0\0\0\x09\0\0\0\0')
        tags = list(self.tags(data))
        self.assertEqual([(t, ts, b[:2]) for t, ts, b in tags[:4]],
                         [(9, 0, b'\x17\0'), (9, 0, b'\x17\1'), (8, 10, b'\xaf\0'), (8, 10, b'\xaf\1')])
        self.assertEqual(tags[3][2][2:], SILENCE)
        # Probe the exact response bytes. stdin works with native or Windows ffprobe under WSL.
        run = subprocess.run([probe, '-v', 'error', '-protocol_whitelist', 'pipe', '-show_frames',
                              '-show_streams', '-of', 'json', 'pipe:0'], input=data, capture_output=True, timeout=10)
        self.assertEqual((run.returncode, run.stderr), (0, b''))
        probed = json.loads(run.stdout); streams = {s['codec_name']: s for s in probed['streams']}
        self.assertEqual((streams['h264']['width'], streams['h264']['height']), (16, 16))
        self.assertEqual((streams['aac']['sample_rate'], streams['aac']['channels']), ('44100', 1))
        self.assertEqual([f['nb_samples'] for f in probed['frames'] if f['media_type'] == 'audio'], [1024]*6)
        self.assertEqual(len([f for f in probed['frames'] if f['media_type'] == 'video']), 6)

    def finish(self, r, count):
        # Drain asynchronously delivered tags before resetting the source (which
        # intentionally discards pending output, never leaking it to new readers).
        data = bytearray(r.read(13))
        for _ in range(count):
            h = r.read(11); self.assertEqual(len(h), 11)
            data.extend(h); data.extend(r.read(int.from_bytes(h[1:4], 'big') + 4))
        self.cmd('avc'); self.assertEqual(r.read(), b'')
        return bytes(data)

    def test_configuration_and_timestamps(self):
        r = self.join()
        self.feed(SPS + PPS, 1000000); self.feed(P, 1040000)  # repeat config; no new sequence header
        self.feed(SPS[:7] + b'\x0b' + SPS[8:], 1050000)  # new level, same PPS, wait for next IDR
        self.feed(P, 1060000); self.feed(adts(), 1070000, True)
        self.feed(IDR, 1100000); self.feed(adts(3), 1110000, True)
        self.feed(P, 1090000)  # regression clamps within track
        self.feed(P, 1000000 + 0x1000001 * 1000)
        self.feed(adts(3)[:-1], 1120000, True)  # malformed ADTS cannot become an FLV tag
        tags = list(self.tags(self.finish(r, 11)))
        self.assertEqual([(t, ts, b[:2]) for t, ts, b in tags[4:]], [
            (9, 40, b'\x27\1'), (9, 100, b'\x17\0'), (9, 100, b'\x17\1'),
            (8, 110, b'\xaf\0'), (8, 110, b'\xaf\1'), (9, 100, b'\x27\1'), (9, 0x1000001, b'\x27\1')])
        self.assertEqual(self.request().getresponse().status, 503)  # old SPS/PPS not reused after reset

    def test_video_only_and_late_audio(self):
        self.feed(SPS + PPS, 0)
        c = self.request(); self.await_reader(); self.feed(IDR, 2000000)
        r = c.getresponse(); self.addCleanup(r.close)
        self.assertEqual(r.read(13)[4], 1)
        self.feed(adts(), 2010000, True)
        self.assertTrue(r.read())  # clean EOF rather than audio in a video-only FLV
        self.assertEqual(list(self.tags(self.finish(self.join(), 4)))[-1][0], 8)

    def test_stalled_network_reader_isolated(self):
        self.feed(SPS + PPS, 0)
        slow = socket.socket(); self.addCleanup(slow.close)
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow.connect(self.addr)
        slow.sendall(b'GET /live.flv HTTP/1.1\r\nHost: localhost\r\nAuthorization: Basic ' +
                     base64.b64encode(b'user:pass') + b'\r\n\r\n')
        fast = self.request()
        for _ in range(100):
            if self.cmd('count') == '2': break
            time.sleep(.01)
        else: self.fail('missing reader')
        self.feed(IDR, 0); self.feed(adts(), 0, True)
        r = fast.getresponse(); self.addCleanup(r.close)
        self.assertEqual(r.status, 200); self.assertEqual(r.read(13)[4], 5)
        def tag():
            h = r.read(11); self.assertEqual(len(h), 11)
            self.assertEqual(len(r.read(int.from_bytes(h[1:4], 'big') + 4)), int.from_bytes(h[1:4], 'big') + 4)
            return h[0], int.from_bytes(h[4:7], 'big')
        for _ in range(4): tag()
        for i in range(1, 21):
            self.assertEqual(self.cmd('large ' + str(i*40000)), 'ok')
            self.assertEqual(tag(), (9, i*40))  # fast viewer never loses a frame
        self.assertEqual(self.request('/serverinfo').getresponse().status, 200)
        for _ in range(140):
            if self.cmd('count') == '1': break
            time.sleep(.05)
        else: self.fail('stalled writer held a reader slot past its send deadline')
        self.feed(P, 900000); self.assertEqual(tag(), (9, 900))

    def test_disconnect_and_stop_during_keyframe_wait(self):
        self.feed(SPS + PPS, 0)
        c = self.request(); self.await_reader(); c.close()
        for _ in range(100):
            if self.cmd('begin') == '1': break
            time.sleep(.01)
        else: self.fail('reader slot leaked after disconnect')
        self.assertEqual(self.request().getresponse().status, 503)
        self.cmd('end')
        waiting = self.request(); self.await_reader()
        self.assertEqual(self.cmd('stop'), 'ok')  # Stop joins the pending worker with the client still connected
        self.assertEqual(self.cmd('count'), '0')
        with self.assertRaises(http.client.RemoteDisconnected): waiting.getresponse()


if __name__ == '__main__':
    unittest.main()
