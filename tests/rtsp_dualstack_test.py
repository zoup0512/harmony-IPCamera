#!/usr/bin/env python3
"""Linux/WSL: python3 tests/rtsp_dualstack_test.py [--server PREBUILT_FIXTURE]."""
import argparse
from contextlib import ExitStack, contextmanager
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time

TESTS = Path(__file__).resolve().parent
CPP = TESTS.parent / 'entry/src/main/cpp'
HOSTS = ('127.0.0.1', '::1')


@contextmanager
def fixture(exe, error=None):
    env = os.environ.copy()
    if error:
        env['RTSP_TEST_FAMILY_ERROR'] = error
    proc = subprocess.Popen([str(exe), 'dual'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
    try:
        yield proc
    finally:
        proc.stdin.close()
        try:
            assert proc.wait(timeout=4) == (1 if error == 'busy4' else 0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            proc.stdout.close()


def command(proc, name):
    proc.stdin.write(name.encode() + b'\n')
    proc.stdin.flush()


class Session:
    def __init__(self, stack, host, port):
        self.host = host
        self.url = f'rtsp://{"[::1]" if host == "::1" else host}:{port}/live'
        self.sock = stack.enter_context(socket.create_connection((host, port), timeout=8))
        self.file = stack.enter_context(self.sock.makefile('rb'))
        self.seq = 0
        self.udp = []
        self.stack = stack

    def request(self, method, track=None, transport='', status=200):
        self.seq += 1
        url = self.url if track is None else self.url + f'/trackID={track}'
        msg = f'{method} {url} RTSP/1.0\r\nCSeq: {self.seq}\r\n'
        if transport:
            msg += f'Transport: {transport}\r\n'
        self.sock.sendall((msg + '\r\n').encode())
        line = self.file.readline()
        assert line.startswith(f'RTSP/1.0 {status} '.encode()), (method, status, line)
        fields = {}
        for line in iter(self.file.readline, b'\r\n'):
            key, value = line.rstrip().decode().split(': ', 1)
            fields[key] = value
        assert fields['CSeq'] == str(self.seq)
        return fields, self.file.read(int(fields.get('Content-Length', 0))).decode()

    def setup(self, tcp):
        for track in (0, 1):
            if tcp:
                pair = (4, 9) if track == 0 else (2, 7)
                spec = f'RTP/AVP/TCP;unicast;interleaved={pair[0]}-{pair[1]}'
                assert self.request('SETUP', track, spec)[0]['Transport'] == spec
            else:
                sockets = []
                for _ in range(2):
                    family = socket.AF_INET6 if ':' in self.host else socket.AF_INET
                    s = self.stack.enter_context(socket.socket(family, socket.SOCK_DGRAM))
                    s.bind((self.host, 0))
                    s.settimeout(8)
                    sockets.append(s)
                ports = [s.getsockname()[1] for s in sockets]
                spec = f'RTP/AVP;unicast;client_port={ports[0]}-{ports[1]}'
                trans = self.request('SETUP', track, spec)[0]['Transport']
                match = re.fullmatch(re.escape(spec) + r';server_port=(\d+)-(\d+)', trans)
                assert match, trans
                a, b = map(int, match.groups())
                assert a % 2 == 0 and b == a + 1
                self.udp.extend(zip(sockets, (a, b)))

    def packet(self, track, rtcp):
        if self.udp:
            sock, port = self.udp[track * 2 + rtcp]
            data, src = sock.recvfrom(2048)
            assert src[:2] == (self.host, port), (src, port)
            return data
        kind, channel, length = struct.unpack('!BBH', self.file.read(4))
        assert kind == ord('$') and channel == ((4, 9), (2, 7))[track][rtcp], channel
        data = self.file.read(length)
        assert len(data) == length
        return data


def read_media(session, round_idx):
    tracks = []
    for track, count in enumerate((2, 1)):
        payload_bytes = 0
        for i in range(count):
            p = session.packet(track, False)
            version, pt, seq, timestamp, ssrc = struct.unpack_from('!BBHII', p)
            assert version == 0x80 and pt & 127 == 96 + track and seq == count * round_idx + i
            payload_bytes += len(p) - 12
        tracks.append((ssrc, timestamp, count * (round_idx + 1), payload_bytes * (round_idx + 1)))
    assert tracks[0][0] != tracks[1][0]
    return tracks


def read_reports(s, tracks, before, after):
    names = []
    reports = []
    for track, (ssrc, timestamp, count, octets) in enumerate(tracks):
        p = s.packet(track, True)
        assert p[:4] == b'\x80\xc8\x00\x06'
        assert p[28:30] == b'\x81\xca' and 28 + 4 * (1 + int.from_bytes(p[30:32], 'big')) == len(p)
        sr_ssrc, sec, frac, rtp, n, size = struct.unpack_from('!IIIIII', p, 4)
        assert (sr_ssrc, n, size) == (ssrc, count, octets)
        sdes_ssrc, kind, length = struct.unpack_from('!IBB', p, 32)
        assert sdes_ssrc == ssrc and kind == 1 and length > 0
        names.append(p[38:38+length])
        assert 1 <= len(p[38+length:]) <= 4 and not any(p[38+length:])
        wall = sec - 2208988800 + frac / 2**32
        assert before <= wall <= time.time() + .05
        rate = (90000, 44100)[track]
        diff = ((rtp - timestamp) % 2**32) / rate
        assert before - .05 <= wall - diff <= after + .05, (diff, wall, before, after)
        reports.append((wall, rtp))
    assert names[0] == names[1]
    return reports


def scenario(exe):
    with fixture(exe) as proc, ExitStack() as stack:
        port = int(proc.stdout.readline())
        sessions = [Session(stack, host, port) for host in HOSTS for _ in range(2)]
        tcp = 'RTP/AVP/TCP;interleaved='
        for s in sessions:
            assert 'm=audio' not in s.request('DESCRIBE')[1]
            s.request('SETUP', 1, tcp + '2-3', 404)
            s.request('SETUP', 2, tcp + '2-3', 404)
        command(proc, 'frame')  # initialize AAC
        for i, s in enumerate(sessions):
            for _ in range(100):
                _, sdp = s.request('DESCRIBE')
                if 'm=audio' in sdp:
                    break
                time.sleep(.01)
            family, unspecified = ('IP6', '::') if ':' in s.host else ('IP4', '0.0.0.0')
            assert sdp.count(f'c=IN {family} {unspecified}\r\n') == 2
            assert re.search(r'o=- \d+ 0 IN ' + family + ' ' + re.escape(s.host) + r'\r\n', sdp)
            for invalid in (tcp+'255', tcp+'1-1', tcp+'2-256', tcp+'2-3junk', tcp+'-1-1',
                            tcp+'0-1;interleaved=2-3', 'RTP/AVP;client_port=65535',
                            'RTP/AVP;client_port=0-1', 'RTP/AVP;multicast;client_port=1234-1235',
                            'RTP/AVP;destination=example.invalid;client_port=1234-1235'):
                s.request('SETUP', 0, invalid, 461)
            s.setup(i % 2 == 0)
            if i % 2 == 0:
                s.request('SETUP', 1, tcp+'9-20', 461)  # video RTCP collision; old track intact
            s.request('PLAY')
            s.request('SETUP', 1, tcp+'6-7', 455)
        previous = []
        for round_idx in (0, 1):
            before = time.time()
            command(proc, 'frame')
            media = [read_media(s, round_idx) for s in sessions]
            after = time.time()
            current = [read_reports(s, tracks, before, after) for s, tracks in zip(sessions, media)]
            if round_idx:
                for first, second in zip(previous, current):
                    for track, ((ntp1, rtp1), (ntp2, rtp2)) in enumerate(zip(first, second)):
                        dt = ntp2 - ntp1
                        assert 4.7 < dt < 7
                        assert abs((rtp2 - rtp1) % 2**32 / (90000,44100)[track] - dt) < .02
            previous = current
        for s in sessions:
            s.request('PAUSE')
            s.request('TEARDOWN')
            assert s.file.read(1) == b''
        # Producer fenced, no playing sessions. Change AAC format; no stale
        # 44100/stereo declaration between stop and first fresh negotiated AU.
        s = Session(stack, HOSTS[1], port)
        old = s.request('DESCRIBE')[1]
        assert 'MPEG4-generic/44100/2' in old
        command(proc, 'reset-audio')
        assert proc.stdout.readline() == b'reset\n'
        cleared = s.request('DESCRIBE')[1]
        assert 'm=audio' not in cleared and cleared.split('m=video')[1] == old.split('m=video')[1].split('m=audio')[0]
        s.request('SETUP', 1, tcp+'2-3', 404)
        command(proc, 'new-audio')
        assert proc.stdout.readline() == b'new\n'
        assert 'MPEG4-generic/48000/1' in s.request('DESCRIBE')[1]
        # A nonblocking send can drop one complete RTP datagram. The next SR
        # must count only the one accepted by the kernel, not both attempts.
        s.setup(False)
        s.request('PLAY')
        command(proc, 'drop-next-audio')
        for _ in range(2):
            command(proc, 'new-audio')
            assert proc.stdout.readline() == b'new\n'
        p = s.packet(1, False)
        assert struct.unpack_from('!H', p, 2)[0] == 1
        sr = s.packet(1, True)
        assert struct.unpack_from('!II', sr, 20) == (1, len(p) - 12)


def shutdown_and_fallback(exe):
    with fixture(exe) as proc, ExitStack() as stack:
        port = int(proc.stdout.readline())
        for i in range(16):
            s = Session(stack, HOSTS[i % 2], port)
            s.request('OPTIONS')
            s.sock.sendall(b'$\x01\xff\xff')  # reader waiting for a partial interleaved frame
        with socket.create_connection((HOSTS[1], port), timeout=2) as extra:
            assert extra.recv(1) == b''
        command(proc, 'restart')
        assert int(proc.stdout.readline()) == port
        for host in HOSTS:
            Session(stack, host, port).request('OPTIONS')
    for error, host in (('no6', HOSTS[0]), ('no4', HOSTS[1])):
        with fixture(exe, error) as proc, ExitStack() as stack:
            Session(stack, host, int(proc.stdout.readline())).request('OPTIONS')
    with fixture(exe, 'busy4') as proc:
        assert proc.stdout.read() == b''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        build = Path(tmp)
        exe = args.server
        if not exe:
            (build / 'hilog').mkdir()
            (build / 'hilog/log.h').write_text('#define OH_LOG_Print(...) ((void)0)\n')
            exe = build / 'host'
            subprocess.run([os.getenv('CXX', 'c++'), '-std=c++17', '-pthread', '-I', str(build), '-I', str(CPP),
                            str(TESTS / 'rtsp_dualstack_host.cpp'), str(CPP / 'rtsp_server.cpp'),
                            '-Wl,--wrap=bind,--wrap=send', '-o', str(exe)], check=True)
        scenario(exe.resolve())
        shutdown_and_fallback(exe.resolve())
        print('PASS: IPv4/IPv6 × UDP/TCP, SR/SDES clocks, counts, sources, negotiation, cap, shutdown and fallback')


if __name__ == '__main__':
    main()
