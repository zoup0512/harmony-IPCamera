#!/usr/bin/env python3
"""Read-only Harmony server smoke test. Start the service/camera manually; for the
archive check, make a motion recording and stop it first. Supply *existing*,
loopback-only HDC TCP forwards to those services. Check the host listener bind
addresses yourself: `fport ls` shows ownership and direction, not bind addresses.
This client never creates/removes forwards, follows redirects, or writes media.
RTP and an MD file header do not prove decoding or a real motion detection hit.
"""
import argparse
import base64
from contextlib import contextmanager
import getpass
import os
import re
import socket
import subprocess
import sys
import time
from urllib.parse import quote
import warnings


class Failure(Exception):
    """Only constant diagnostics (or validated numeric status codes) reach stderr."""


def require(condition, message):
    if not condition:
        raise Failure(message)


def port_number(s):
    if not re.fullmatch(r'[0-9]{1,5}', s) or not 1 <= int(s) <= 65535:
        raise argparse.ArgumentTypeError('expected TCP port 1..65535')
    return int(s)


def verify_forwards(device, ports, timeout):
    # -p: client only; don't start/reconfigure an HDC server. Never pass secrets
    # to the child. Some HDC versions list other targets even with -t.
    server = port_number(os.environ.get('OHOS_HDC_SERVER_PORT', '8710'))
    env = {k: v for k, v in os.environ.items() if k not in ('IPCAM_USER', 'IPCAM_PASSWORD')}
    proc = subprocess.run(['hdc', '-p', '-s', f'127.0.0.1:{server}', '-t', device, 'fport', 'ls'],
                          stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          timeout=timeout, env=env)
    require(proc.returncode == 0, 'cannot list existing HDC forwards')
    rows = [line.split() for line in proc.stdout.decode('utf-8', 'replace').splitlines()]
    for port in ports:
        matches = [r for r in rows if len(r) == 4 and r[1] == f'tcp:{port}' and r[3] == '[Forward]']
        require(len(matches) == 1 and matches[0][0] == device and
                re.fullmatch(r'tcp:[0-9]{1,5}', matches[0][2]) and 1 <= int(matches[0][2][4:]) <= 65535,
                'no unambiguous TCP forward for the requested device and port')


def credentials(prompt):
    user, password = os.getenv('IPCAM_USER'), os.getenv('IPCAM_PASSWORD')
    if prompt:
        # getpass can otherwise fall back to echoed input on a non-terminal.
        with warnings.catch_warnings():
            warnings.simplefilter('error', getpass.GetPassWarning)
            user = getpass.getpass('Username (hidden): ')
            password = getpass.getpass('Password (hidden): ')
    if user is None and password is None:
        return ''
    require(user is not None and ':' not in user and password is not None,
            'set both IPCAM_USER and IPCAM_PASSWORD, or use --prompt')
    token = base64.b64encode(f'{user}:{password}'.encode()).decode('ascii')
    return f'Authorization: Basic {token}\r\n'


def remaining(deadline):
    seconds = deadline - time.monotonic()
    if seconds <= 0:
        raise TimeoutError
    return seconds


class Wire:
    """Bounded reads with an absolute deadline, including interleaved TCP frames."""
    def __init__(self, sock, deadline):
        self.sock, self.deadline, self.buf = sock, deadline, bytearray()

    def send(self, text):
        self.sock.settimeout(remaining(self.deadline))
        self.sock.sendall(text.encode('ascii'))

    def receive(self, limit):
        self.sock.settimeout(remaining(self.deadline))
        data = self.sock.recv(limit)
        require(data, 'unexpected end of stream')
        self.buf.extend(data)

    def take(self, size):
        while len(self.buf) < size:
            self.receive(min(65536, size - len(self.buf)))
        data = bytes(self.buf[:size])
        del self.buf[:size]
        return data

    def response(self, protocol):
        while b'\r\n\r\n' not in self.buf:
            require(len(self.buf) < 16384, 'response headers too large')
            self.receive(4096)
        end = self.buf.index(b'\r\n\r\n') + 4
        require(end <= 16384, 'response headers too large')
        lines = self.take(end - 4).decode('latin1').split('\r\n')
        self.take(4)
        status = lines[0].split(' ', 2)
        require(len(status) >= 2 and status[0] == protocol and re.fullmatch('[0-9]{3}', status[1]),
                'invalid response status')
        fields = {}
        for line in lines[1:]:
            key, sep, value = line.partition(':')
            key = key.lower()
            require(sep and key not in fields, 'malformed response headers')
            fields[key] = value.strip()
        require('transfer-encoding' not in fields, 'unsupported response framing')
        return int(status[1]), fields

    def body(self, fields, limit):
        length = fields.get('content-length', '0')
        require(re.fullmatch(r'[0-9]{1,10}', length) and int(length) <= limit, 'invalid body length')
        return self.take(int(length))


@contextmanager
def connection(port, deadline):
    # Numeric IPv4 only. No DNS, proxies, peer-supplied host names, or UDP.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(remaining(deadline))
        sock.connect(('127.0.0.1', port))
        yield Wire(sock, deadline)


def video_track(sdp):
    for media in re.split(r'(?m)^m=', sdp.decode('ascii').replace('\r\n', '\n'))[1:]:
        lines = media.splitlines()
        desc = lines[0].split() if lines else []
        if len(desc) < 4 or desc[0] != 'video' or desc[2] != 'RTP/AVP':
            continue
        # Only accept the app's relative track controls, never a remote URL.
        control = re.search(r'(?m)^a=control:(trackID=[0-9]+)$', media)
        for pt in re.findall(r'(?im)^a=rtpmap:([0-9]{1,3}) H26[45]/90000$', media):
            if control and pt in desc[3:] and 96 <= int(pt) <= 127:
                return control[1], int(pt)
    raise Failure('no supported SDP video track')


def rtp_payload_size(packet, pt):
    require(len(packet) >= 12 and packet[0] >> 6 == 2 and packet[1] & 127 == pt,
            'invalid RTP version, length or video payload type')
    start = 12 + (packet[0] & 15) * 4
    if packet[0] & 16:
        require(start + 4 <= len(packet), 'truncated RTP extension')
        start += 4 + 4 * int.from_bytes(packet[start + 2:start + 4], 'big')
    pad = packet[-1] if packet[0] & 32 else 0
    require(not (packet[0] & 32) or pad, 'invalid RTP padding')
    size = len(packet) - start - pad
    require(size > 0, 'empty or truncated RTP payload')
    return size


def probe_rtsp(port, path, auth, timeout):
    stream = f'rtsp://127.0.0.1:{port}{path}'
    with connection(port, time.monotonic() + timeout) as wire:
        seq = 0

        def request(method, url, headers=''):
            nonlocal seq
            seq += 1
            wire.send(f'{method} {url} RTSP/1.0\r\nCSeq: {seq}\r\n' +
                      (auth if method != 'OPTIONS' else '') + headers + '\r\n')
            status, fields = wire.response('RTSP/1.0')
            require(status == 200, f'{method} returned {status}')
            require(fields.get('cseq') == str(seq), 'RTSP CSeq mismatch')
            return fields, wire.body(fields, 65536)

        request('OPTIONS', '*')
        fields, sdp = request('DESCRIBE', stream, 'Accept: application/sdp\r\n')
        require(fields.get('content-type') == 'application/sdp', 'missing SDP')
        control, pt = video_track(sdp)
        fields, _ = request('SETUP', stream + '/' + control,
                            'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n')
        transport = fields.get('transport', '').lower().split(';')
        require(transport[0] == 'rtp/avp/tcp' and 'interleaved=0-1' in transport,
                'server did not accept the requested TCP video channels')
        session = fields.get('session', '').split(';')[0]
        require(re.fullmatch('[A-Za-z0-9._-]+', session), 'missing or invalid RTSP session')
        fields, _ = request('PLAY', stream, f'Session: {session}\r\n')
        require(fields.get('session', '').split(';')[0] == session, 'PLAY session mismatch')
        while True:
            marker, channel, hi, lo = wire.take(4)
            require(marker == ord('$') and channel in (0, 1), 'unexpected interleaved frame')
            packet = wire.take((hi << 8) | lo)
            if channel == 0:
                return rtp_payload_size(packet, pt)  # Closing this TCP connection releases our session.


def probe_archive(port, auth, timeout):
    deadline = time.monotonic() + timeout

    @contextmanager
    def get(path, headers=''):
        with connection(port, deadline) as wire:
            wire.send(f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n' +
                      headers + '\r\n')
            yield (*wire.response('HTTP/1.1'), wire)

    with get('/getarchives') as (status, fields, _):
        require(status == 401 and fields.get('www-authenticate', '').lower().startswith('basic '),
                'archive list did not require HTTP Basic')
    with get('/getarchives', auth) as (status, fields, wire):
        require(status == 200, f'archive list returned {status}')
        listing = wire.body(fields, 1024 * 1024).decode('utf-8')
    clips = []
    for line in listing.splitlines():
        match = re.fullmatch(r'(MD/IPS_[^/\\\x00-\x1f\x7f]*\.mp4) ([0-9]{1,19})', line)
        if match and '..' not in match[1]:
            clips.append((match[1], int(match[2])))
    require(clips, 'no MD recording in the archive list')
    name, size = max(clips)  # Recorder timestamps sort lexically; do not print them.
    with get('/recordings/media/' + quote(name), auth + 'Range: bytes=0-15\r\n') as (status, fields, wire):
        require(status == 206, f'archive Range returned {status}, expected 206')
        require(size >= 16 and fields.get('content-range') == f'bytes 0-15/{size}' and
                fields.get('content-length') == '16', 'incorrect archive byte range')
        prefix = wire.take(16)
        require(prefix[4:8] == b'ftyp' and 16 <= int.from_bytes(prefix[:4], 'big') <= size,
                'not an MP4 ftyp header')
    return len(clips), len(prefix)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--device', required=True, help='exact HDC connect key, never auto-selected')
    parser.add_argument('--rtsp-port', required=True, type=port_number, help='existing loopback RTSP forward')
    parser.add_argument('--http-port', type=port_number, help='existing loopback HTTP forward; enables archive check')
    parser.add_argument('--path', default='/live', help='RTSP stream path (default: /live)')
    parser.add_argument('--timeout', type=float, default=10, help='seconds per check (default: 10, maximum: 120)')
    parser.add_argument('--prompt', action='store_true', help='prompt privately instead of IPCAM_USER / IPCAM_PASSWORD')
    args = parser.parse_args(argv)
    stage = 'configuration'
    try:
        require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.:\-]{0,127}', args.device) and args.device != 'any',
                'expected an explicit HDC device ID')
        require(re.fullmatch(r'(?:/[A-Za-z0-9_.~-]+)+', args.path) and '..' not in args.path,
                'expected a local stream path, not a URL')
        require(0 < args.timeout <= 120 and args.rtsp_port != args.http_port, 'invalid timeout or overlapping ports')
        auth = credentials(args.prompt)
        require(args.http_port is None or auth, 'HTTP check requires credentials')
        stage = 'HDC forwards'
        verify_forwards(args.device, [p for p in (args.rtsp_port, args.http_port) if p], args.timeout)
        stage = 'RTSP'
        payload = probe_rtsp(args.rtsp_port, args.path, auth, args.timeout)
        print(f'PASS RTSP video_rtp_packets=1 payload_bytes={payload}')
        if args.http_port:
            stage = 'HTTP archive'
            count, length = probe_archive(args.http_port, auth, args.timeout)
            print(f'PASS HTTP md_entries={count} mp4_prefix_bytes={length}')
        print('Not verified: video decoding or an actual motion detection hit.')
        return 0
    except Failure as err:
        print(f'FAIL {stage}: {err}', file=sys.stderr)
    except (TimeoutError, subprocess.TimeoutExpired):
        print(f'FAIL {stage}: timeout', file=sys.stderr)
    except (OSError, ValueError, EOFError, getpass.GetPassWarning, argparse.ArgumentTypeError):
        # Library exceptions can contain request paths, raw headers or payloads.
        print(f'FAIL {stage}: unavailable or invalid input/response', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
