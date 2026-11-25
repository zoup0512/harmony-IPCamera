"""Offline self-tests: python -B tests/device_stream_smoke_test.py -v (stdlib only)."""
import base64
import contextlib
import io
import os
import socketserver
import struct
import subprocess
import threading
import unittest
from unittest.mock import patch
from urllib.parse import quote

import device_stream_smoke as smoke

DEVICE = 'fixture-USB'
USER, PASSWORD = 'private-viewer', 'private-password'
TOKEN = base64.b64encode(f'{USER}:{PASSWORD}'.encode()).decode()
NAME = 'MD/IPS_private #雪.mp4'
SAMPLE = b'\x00\x00\x00\x18ftypisom\x00\x00\x00\x00'
SDP = (b'v=0\r\na=control:*\r\nm=audio 0 RTP/AVP 97\r\na=control:trackID=1\r\n'
       b'm=video 0 RTP/AVP 101\r\na=rtpmap:101 H265/90000\r\na=control:trackID=7\r\n')
PACKET = struct.pack('!BBHII', 0x80, 0x80 | 101, 73, 90000, 17) + b'private-video'


def frame(channel, payload):
    return struct.pack('!BBH', 36, channel, len(payload)) + payload


class Fixture(socketserver.StreamRequestHandler):
    timeout = 2

    def handle(self):
        server = self.server
        while True:
            line = self.rfile.readline()
            if not line:
                return
            method, target, proto = line.decode('ascii').split()
            headers = {}
            for line in iter(self.rfile.readline, b'\r\n'):
                if not line:
                    return
                k, v = line.decode('ascii').split(':', 1)
                headers[k.lower()] = v.strip()
            server.calls.append((method, target, headers))
            protected = headers.get('authorization') == 'Basic ' + TOKEN
            status, fields, body, frames = 200, {}, b'', b''
            if proto == 'RTSP/1.0':
                fields['CSeq'] = headers['cseq']
                if method == 'OPTIONS' and target == '*':
                    fields['Public'] = 'OPTIONS, DESCRIBE, SETUP, PLAY'
                elif not protected and server.mode != 'public':
                    status = 401
                elif method == 'DESCRIBE' and target.endswith('/live'):
                    fields['Content-Type'] = 'application/sdp'
                    body = SDP
                elif method == 'SETUP' and target.endswith('/live/trackID=7') and headers.get('transport') == 'RTP/AVP/TCP;unicast;interleaved=0-1':
                    fields.update(Transport=headers['transport'], Session='test;timeout=60')
                elif method == 'PLAY' and target.endswith('/live') and headers.get('session') == 'test':
                    fields['Session'] = 'test'
                    frames = frame(1, b'control')  # Not evidence of video.
                    if server.mode != 'stall':
                        frames += frame(0, PACKET if server.mode != 'audio' else b'\x80\x61' + PACKET[2:])
                else:
                    status = 400
            elif method != 'GET':
                status = 405
            elif not protected and server.mode != 'unguarded':
                status = 401
                fields['WWW-Authenticate'] = 'Basic realm="fixture"'
            elif server.mode == 'redirect':
                status = 302
                fields['Location'] = '/light'  # Must not follow even a same-origin redirect.
            elif target == '/getarchives':
                body = b'ipc_manual.mp4 100\n'
                if server.mode != 'manual':
                    body += f'{NAME} 256\nMD/IPS_001.mp4 128\nMD/IPS_../../bad.mp4 64\n'.encode()
            elif target == '/recordings/media/' + quote(NAME) and headers.get('range') == 'bytes=0-15':
                status = 206 if server.mode != 'ignored-range' else 200
                fields['Content-Range'] = 'bytes 0-15/' + ('300' if server.mode == 'wrong-size' else '256')
                body = SAMPLE if server.mode != 'not-mp4' else b'x' * 16
            else:
                status = 404
            fields['Content-Length'] = str(len(body))
            reply = f'{proto} {status} Test\r\n' + ''.join(f'{k}: {v}\r\n' for k, v in fields.items())
            self.wfile.write((reply + '\r\n').encode() + body + frames)
            if proto != 'RTSP/1.0':
                return


@contextlib.contextmanager
def fixture(mode='normal'):
    with socketserver.TCPServer(('127.0.0.1', 0), Fixture) as server:
        server.mode, server.calls = mode, []
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.01})
        thread.start()
        try:
            yield server
        finally:
            server.shutdown()
            thread.join()


class SmokeTest(unittest.TestCase):
    def setUp(self):
        env = patch.dict(os.environ, {'IPCAM_USER': USER, 'IPCAM_PASSWORD': PASSWORD,
                                      'OHOS_HDC_SERVER_PORT': '8710'})
        env.start()
        self.addCleanup(env.stop)
        # No real HDC invocation, not even for missing-device or failure tests.
        runner = patch.object(smoke.subprocess, 'run')
        self.hdc = runner.start()
        self.addCleanup(runner.stop)

    def run_cli(self, rtsp, http=None, *, rules=None, extra=()):
        port = rtsp.server_address[1]
        ports = [port] + ([http.server_address[1]] if http else [])
        args = ['--device', DEVICE, '--rtsp-port', str(port), '--timeout', '1']
        if http:
            args += ['--http-port', str(ports[1])]
        if rules is None:
            rules = ''.join(f'{DEVICE}    tcp:{p} tcp:{r}    [Forward]\n' for p, r in zip(ports, (8554, 8081)))
        self.hdc.return_value = subprocess.CompletedProcess([], 0, rules.encode(), b'')
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            result = smoke.main(args + list(extra))
        text = out.getvalue()
        for private in (USER, PASSWORD, TOKEN, NAME, 'IPS_', 'private-video', quote(NAME), DEVICE):
            self.assertNotIn(private, text)
        return result, text

    def test_read_only_full_pass_repeat(self):
        with fixture() as r, fixture() as h:
            for _ in range(2):
                code, text = self.run_cli(r, h)
                self.assertEqual(code, 0, text)
                self.assertIn('video_rtp_packets=1', text)
                self.assertIn('md_entries=2 mp4_prefix_bytes=16', text)
                self.assertIn('Not verified', text)
            self.assertEqual([m for m, _, _ in r.calls], ['OPTIONS', 'DESCRIBE', 'SETUP', 'PLAY'] * 2)
            self.assertEqual([(m, path) for m, path, _ in h.calls], [('GET', '/getarchives'),
                             ('GET', '/getarchives'), ('GET', '/recordings/media/' + quote(NAME))] * 2)
            self.assertEqual(self.hdc.call_count, 2)
            for args, kw in self.hdc.call_args_list:
                self.assertEqual(args[0], ['hdc', '-p', '-s', '127.0.0.1:8710', '-t', DEVICE, 'fport', 'ls'])
                self.assertNotIn('IPCAM_PASSWORD', kw['env'])

    def test_public_rtsp_only(self):
        with fixture('public') as r, patch.dict(os.environ, {}, clear=True):
            self.assertEqual(self.run_cli(r)[0], 0)
            self.assertTrue(all('authorization' not in h for _, _, h in r.calls))

    def test_forward_ownership_before_connection(self):
        with fixture() as r:
            port = r.server_address[1]
            for row in ('', f'{DEVICE}x tcp:{port} tcp:8554 [Forward]',
                        f'{DEVICE} tcp:{port} tcp:8554 [Reverse]',
                        f'{DEVICE} tcp:{port} localabstract:foo [Forward]',
                        f'{DEVICE} tcp:{port} tcp:8554 [Forward]\nother tcp:{port} tcp:8554 [Forward]'):
                with self.subTest(row=row):
                    self.assertNotEqual(self.run_cli(r, rules=row)[0], 0)
            self.assertEqual(r.calls, [])

    def test_reject_unbound_target_or_url(self):
        with fixture() as r:
            for args in (('--device', 'any'), ('--device', '*'), ('--path', '//remote'), ('--timeout', 'nan')):
                self.assertNotEqual(self.run_cli(r, extra=args)[0], 0)
            self.hdc.assert_not_called()

    def test_hdc_timeout_before_connection(self):
        self.hdc.side_effect = subprocess.TimeoutExpired('hdc', 1)
        with fixture() as r:
            code, text = self.run_cli(r)
            self.assertEqual(code, 1)
            self.assertIn('HDC forwards: timeout', text)
            self.assertEqual(r.calls, [])

    def test_private_prompt(self):
        with fixture() as r, patch.dict(os.environ, {}, clear=True), \
                patch.object(smoke.getpass, 'getpass', side_effect=[USER, PASSWORD]) as prompt:
            self.assertEqual(self.run_cli(r, extra=('--prompt',))[0], 0)
            self.assertEqual(prompt.call_count, 2)

    def test_auth_failure(self):
        with fixture() as r, patch.dict(os.environ, {'IPCAM_PASSWORD': 'wrong'}):
            code, text = self.run_cli(r)
            self.assertEqual(code, 1)
            self.assertIn('DESCRIBE returned 401', text)
            self.assertNotIn('wrong', text)

    def test_no_video(self):
        for mode, reason in [('audio', 'payload type'), ('stall', 'timeout')]:
            with self.subTest(mode=mode), fixture(mode) as r:
                code, text = self.run_cli(r, extra=('--timeout', '0.25'))
                self.assertEqual(code, 1)
                self.assertIn(reason, text)

    def test_rtp_framing(self):
        for packet in (b'', b'\x40' + PACKET[1:], PACKET[:12], b'\x8f' + PACKET[1:],
                       b'\x90' + PACKET[1:12] + b'\x00\x00\xff\xff', b'\xa0' + PACKET[1:12] + b'\x00'):
            with self.assertRaises(smoke.Failure):
                smoke.rtp_payload_size(packet, 101)

    def test_track_scope(self):
        for sdp in (SDP.replace(b'm=video', b'm=audio'), SDP.replace(b'H265', b'AAC'),
                    SDP.replace(b'trackID=7', b'rtsp://other/video'), SDP.replace(b'rtpmap:101', b'rtpmap:97')):
            with self.assertRaises(smoke.Failure):
                smoke.video_track(sdp)

    def test_archive_failures(self):
        for mode in ('unguarded', 'redirect', 'manual', 'ignored-range', 'wrong-size', 'not-mp4'):
            with self.subTest(mode=mode), fixture() as r, fixture(mode) as h:
                code, text = self.run_cli(r, h)
                self.assertEqual(code, 1)
                self.assertIn('FAIL HTTP archive', text)
                self.assertNotIn('PASS HTTP', text)


if __name__ == '__main__':
    unittest.main()
