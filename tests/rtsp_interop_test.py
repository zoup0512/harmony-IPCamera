#!/usr/bin/env python3
"""Linux/WSL: python3 tests/rtsp_interop_test.py [--server <prebuilt fixture>]."""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
from urllib.parse import urljoin

TESTS = Path(__file__).resolve().parent
CPP = TESTS.parent / 'entry/src/main/cpp'
AUTH = 'Authorization: bAsIc dmlld2VyOnRlc3Q=\r\n'


def scenario(exe, protected):
    process = subprocess.Popen([str(exe)] + (['auth'] if protected else []),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        port = int(process.stdout.readline())
        stream = f'rtsp://127.0.0.1:{port}/live'
        with socket.create_connection(('127.0.0.1', port), timeout=4) as sock, sock.makefile('rb') as reader:
            seq = 0

            def request(method, url=stream, headers='', auth=protected, status=200):
                nonlocal seq
                seq += 1
                msg = f'{method} {url} RTSP/1.0\r\nCSeq: {seq}\r\n' + (AUTH if auth else '') + headers + '\r\n'
                sock.sendall(msg.encode())
                line = reader.readline().decode()
                assert line.startswith(f'RTSP/1.0 {status} '), (method, line)
                fields = {}
                for line in iter(reader.readline, b'\r\n'):
                    key, value = line.decode().rstrip('\r\n').split(': ', 1)
                    assert key not in fields, key
                    fields[key] = value
                assert fields['CSeq'] == str(seq)
                body = reader.read(int(fields.get('Content-Length', '0')))
                if status == 401:
                    assert fields['WWW-Authenticate'] == 'Basic realm="IPCamera"'
                return fields, body

            public = request('OPTIONS', '*', auth=False)[0]['Public'].split(', ')
            if protected:
                for method in ('DESCRIBE', 'SETUP', 'PLAY', 'PAUSE', 'GET_PARAMETER', 'SET_PARAMETER', 'TEARDOWN'):
                    assert method in public
                    request(method, auth=False, status=401)
            request('PLAY', status=455)
            audio = stream + '/trackID=1'
            video = stream + '/trackID=0'
            sid = ''
            if protected:
                h, sdp = request('DESCRIBE', stream + '?view=main')
                assert h['Content-Type'] == 'application/sdp'
                assert urljoin(h['Content-Base'], 'trackID=0') == video
                assert b'a=control:trackID=1\r\n' in sdp
                h, _ = request('SETUP', video, 'Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n')
                assert 'interleaved=4-5' in h['Transport']
                sid = h['Session']
            session = 'Session: ' + sid + '\r\n' if sid else ''
            h, _ = request('SETUP', 'trackID=1', session + 'Transport: RTP/AVP/TCP;interleaved=6-7\r\n')
            assert 'interleaved=6-7' in h['Transport']
            if sid:
                assert h['Session'] == sid
            sid = h['Session']
            session = 'Session: ' + sid + '\r\n'
            request('PLAY', headers='Session: not-this-session\r\n', status=454)
            # Cover both negotiated tracks and audio-only without DESCRIBE.
            tracks = ([(video, 4)] if protected else []) + [(audio, 6)]
            for frame_index in (0, 1):
                h, _ = request('PLAY', headers=session if protected else '')
                assert h['Session'] == sid
                assert h['RTP-Info'].split(',') == [
                    f'url={url};seq={frame_index * (2 if url == video else 1)}' for url, _ in tracks]
                if protected:
                    request('PAUSE', headers=session, auth=False, status=401)
                process.stdin.write(b'frame\n')
                process.stdin.flush()
                for url, channel in tracks:
                    count = 2 if url == video else 1
                    for n in range(count):
                        marker, actual_channel, length = struct.unpack('!BBH', reader.read(4))
                        assert (marker, actual_channel) == (ord('$'), channel)
                        packet = reader.read(length)
                        assert len(packet) == length and struct.unpack('!H', packet[2:4])[0] == frame_index * count + n
                request('PAUSE', headers=session)
            if not protected:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                    udp.bind(('127.0.0.1', 0))
                    udp.settimeout(4)
                    p = udp.getsockname()[1]
                    transport = f'Transport: RTP/AVP;unicast;client_port={p}-{p + 1}\r\n'
                    h, _ = request('SETUP', audio, session + transport)
                    assert h['Session'] == sid and f'client_port={p}-{p + 1}' in h['Transport']
                    h, _ = request('PLAY', headers=session)
                    assert h['RTP-Info'] == f'url={audio};seq=2'
                    process.stdin.write(b'frame\n')
                    process.stdin.flush()
                    assert struct.unpack('!H', udp.recv(1500)[2:4])[0] == 2
            for method in ('GET_PARAMETER', 'SET_PARAMETER'):
                assert request(method, headers=session)[0]['Session'] == sid
            request('TEARDOWN', headers=session)
            assert reader.read(1) == b''
    finally:
        process.stdin.close()
        process.stdout.close()
        try:
            assert process.wait(timeout=5) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, help='already built rtsp_interop_host executable')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        exe = args.server
        if exe is None:
            build = Path(tmp)
            (build / 'hilog').mkdir()
            (build / 'hilog/log.h').write_text('#define OH_LOG_Print(...) ((void)0)\n')
            exe = build / 'rtsp_interop_host'
            subprocess.run([os.getenv('CXX', 'c++'), '-std=c++17', '-pthread', '-I', str(build), '-I', str(CPP),
                            str(TESTS / 'rtsp_interop_host.cpp'), str(CPP / 'rtsp_server.cpp'),
                            '-Wl,--wrap=bind', '-o', str(exe)], check=True)
        for protected in (True, False):
            scenario(exe.resolve(), protected)
        print('PASS: public OPTIONS, per-request auth, SDP control URLs, Session, PLAY and actual RTP (A/V and audio-only)')


if __name__ == '__main__':
    main()
