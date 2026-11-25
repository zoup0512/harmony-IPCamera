#!/usr/bin/env python3
"""Linux/WSL: python3 tests/rtmp_publisher_loopback_test.py [--client <fixture>]. Loopback only."""
import argparse
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
CPP = TESTS.parent / 'entry/src/main/cpp'
SID = 0x01020304  # exercises LE wire encoding and all four media-stream ID bytes


def text(s):
    b = s.encode()
    return b'\x02' + len(b).to_bytes(2, 'big') + b


def num(n):
    return b'\0' + struct.pack('>d', n)


def info(code, level='status'):
    fields = {'level': level, 'code': code, 'description': 'secret-key _error NetStream.Publish.Start'}
    return b'\x03' + b''.join(text(k)[1:] + text(v) for k, v in fields.items()) + b'\0\0\x09'


def cmd(name, tx, value):
    return text(name) + num(tx) + b'\x05' + value


def wire(body, *, typ=20, cs=3, sid=0, chunk=16, fmt=0):
    header = bytes([cs | (fmt << 6)]) + b'\0\0\0' + len(body).to_bytes(3, 'big') + bytes([typ])
    if fmt == 0:
        header += sid.to_bytes(4, 'little')
    return header + bytes([192 + cs]).join(body[i:i + chunk] for i in range(0, len(body), chunk))


def exact(sock, n):
    data = bytearray()
    while len(data) < n:
        piece = sock.recv(n - len(data))
        assert piece, 'unexpected EOF'
        data.extend(piece)
    return bytes(data)


def message(sock):  # client uses full headers; outgoing messages here all fit a chunk
    hdr = exact(sock, 12)
    assert hdr[0] >> 6 == 0
    return hdr[7], int.from_bytes(hdr[8:12], 'little'), exact(sock, int.from_bytes(hdr[4:7], 'big'))


def quiet(sock):
    assert not select.select([sock], [], [], 0.035)[0], 'client advanced before correct confirmation'


def scenario(client, mode, ipv6=False):
    with socket.socket(socket.AF_INET6 if ipv6 else socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(('::1' if ipv6 else '127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(4)
        scheme = 'rtmps' if mode == 'tls' else 'rtmp'
        base = f"{scheme}://{'[::1]' if ipv6 else '127.0.0.1'}:{listener.getsockname()[1]}/live"
        process = subprocess.Popen([str(client), base + '/secret-key?token=secret'],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            def finished(reason):
                out, _ = process.communicate(input='stop\n', timeout=3)
                assert process.returncode == 0 and reason in out and 'secret' not in out, (mode, out)
                assert 'publishing' not in out, (mode, out)

            if mode == 'tls':
                finished('startup failed: RTMPS transport unavailable')
                quiet(listener)
                return
            with listener.accept()[0] as sock:
                sock.settimeout(4)
                c1 = exact(sock, 1537)
                assert c1[0] == 3
                s1 = bytes(range(256)) * 6
                sock.sendall(b'\x03' + s1)
                assert exact(sock, 1536) == s1  # S2 may wait for C2
                sock.sendall(c1[1:])
                assert message(sock) == (1, 0, (60000).to_bytes(4, 'big'))
                typ, sid, body = message(sock)
                assert (typ, sid) == (20, 0) and body.startswith(text('connect') + num(1)) and text(base) in body
                sock.sendall(wire((16).to_bytes(4, 'big'), typ=1, cs=2))
                sock.sendall(wire(cmd('_error', 99, info('NetConnection.Connect.Rejected', 'error'))))
                quiet(sock)
                if mode == 'denied':
                    sock.sendall(wire(cmd('_error', 1, info('NetConnection.Connect.Rejected', 'error'))))
                    finished('startup failed: server rejected:')
                    return
                if mode == 'eof':
                    sock.shutdown(socket.SHUT_RDWR)
                    finished('startup failed: server closed connection')
                    return
                result = wire(cmd('_result', 1, info('NetConnection.Connect.Success')))
                for i in range(0, len(result), 7):
                    sock.sendall(result[i:i + 7])
                assert message(sock) == (20, 0, text('createStream') + num(2) + b'\x05')
                sock.sendall(wire(cmd('_result', 9, num(1))))
                quiet(sock)
                if mode == 'bad-id':
                    sock.sendall(wire(cmd('_result', 2, num(1.5))))
                    finished('invalid createStream stream ID')
                    return
                # Exercise negotiated acknowledgement windows and ping during setup.
                sock.sendall(wire(b'\0\0\0\x01', typ=5, cs=2))
                assert message(sock)[:2] == (3, 0)
                sock.sendall(wire((1 << 24).to_bytes(4, 'big'), typ=5, cs=2) + wire(b'\0\x06abcd', typ=4, cs=2))
                assert message(sock) == (4, 0, b'\0\x07abcd')
                sock.sendall(wire(cmd('_result', 2, num(SID))))
                assert message(sock) == (20, SID, cmd('publish', 0, text('secret-key?token=secret') + text('live')))
                sock.sendall(wire(cmd('onStatus', 0, info('NetStream.Publish.Start')), sid=1))
                quiet(sock)
                if mode == 'timeout':
                    out, _ = process.communicate(timeout=12)
                    assert process.returncode == 0 and out.strip() == 'startup failed: operation timed out', out
                    return
                if mode == 'bad-key':
                    sock.sendall(wire(cmd('onStatus', 0, info('NetStream.Publish.BadName', 'error')), sid=SID))
                    finished('startup failed: server rejected:')
                    return
                status = wire(cmd('onStatus', 0, info('NetStream.Publish.Start')), sid=SID)
                # Compressed header inherits the real stream ID across the handoff;
                # split its header over startup/runtime. Rejection must not leak text.
                later = wire(cmd('onStatus', 0, info('NetStream.Publish.Denied', 'error')), fmt=1)
                sock.sendall(status + (b'' if mode == 'lost' else later[:3]))
                for typ in (18, 9, 9, 8, 8):
                    t, sid, body = message(sock)
                    assert (t, sid) == (typ, SID), (t, sid)
                if mode == 'lost':
                    sock.shutdown(socket.SHUT_WR)
                    expect = 'connection lost: server closed connection'
                else:
                    sock.sendall(later[3:])
                    expect = 'connection lost: server rejected: NetStream.Publish.Denied'
                assert sock.recv(1) == b''
                out, _ = process.communicate(input='stop\n', timeout=3)
                assert process.returncode == 0 and out.splitlines() == ['publishing', expect], (mode, out)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--client', type=Path, help='already built rtmp_publisher_host')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        build = Path(tmp)
        exe = args.client
        if not exe:
            (build / 'hilog').mkdir()
            (build / 'hilog/log.h').write_text('#define OH_LOG_Print(...) ((void)0)\n')
            exe = build / 'publisher'
            subprocess.run([os.getenv('CXX', 'c++'), '-std=c++17', '-pthread', '-I', str(build), '-I', str(CPP),
                            str(TESTS / 'rtmp_publisher_host.cpp'), str(CPP / 'rtmp_publisher.cpp'), '-o', str(exe)], check=True)
        for mode in ('tls', 'denied', 'bad-id', 'bad-key', 'eof', 'lost', 'success', 'timeout'):
            scenario(exe.resolve(), mode)
            print('PASS IPv4', mode, flush=True)
        scenario(exe.resolve(), 'success', ipv6=True)
        print('PASS IPv6 success')


if __name__ == '__main__':
    main()
