"""Offline host test: C++17 compiler plus FFmpeg with software libx264/libx265.
Set CXX and FFMPEG if not on PATH. No device, server, camera, or recording file.
Only generated gray frames are encoded, in memory. OH codec calls are mocked;
actual synthetic source and transcoded target parameter sets/pictures are not.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
CPP = TESTS.parent / 'entry/src/main/cpp'


def main():
    ffmpeg = os.environ.get('FFMPEG') or shutil.which('ffmpeg')
    cc = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
    if not (ffmpeg and cc):
        raise SystemExit('Set FFMPEG and CXX to existing local x264/x265 FFmpeg and C++17 compiler paths')

    def ff(args, data):
        return subprocess.run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', *args],
                              input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=25).stdout

    def encode(args, data, hevc):
        codec = 'x265' if hevc else 'x264'
        return ff([*args, '-an', '-pix_fmt', 'yuv420p', '-c:v', 'lib' + codec, '-threads', '1',
                   '-preset', 'ultrafast', '-bf', '0', '-g', '60', '-b:v', '500k', '-' + codec + '-params',
                   ('pools=none:frame-threads=1:log-level=error:' if hevc else '') + 'scenecut=0',
                   '-f', 'hevc' if hevc else 'h264', 'pipe:1'], data)

    def fixture(stream, hevc):
        nals = [x for x in re.split(b'\x00\x00+\x01', stream) if x]
        units = {(x[0] >> 1) & 63 if hevc else x[0] & 31: x for x in nals}
        vps, sps, pps = (units[32], units[33], units[34]) if hevc else (b'', units[7], units[8])
        idr = next(units[t] for t in (19, 20) if t in units) if hevc else units[5]
        delta = units[1]
        def annex(*seq):
            return b''.join(b'\x00\x00\x00\x01' + s for s in seq if s)
        return [vps, sps, pps, annex(vps, sps, pps), annex(idr), annex(delta)]

    raw = bytes([80]) * (64 * 64) + bytes([128]) * (64 * 64 // 2)
    raw_args = ['-f', 'rawvideo', '-pix_fmt', 'yuv420p', '-s', '64x64', '-r', '30', '-i', 'pipe:0']
    streams = {k: encode(raw_args, raw * 2, k == 'hevc') for k in ['h264', 'hevc']}
    header = '#pragma once\n#include <vector>\nstruct Fixture { std::vector<uint8_t> vps,sps,pps,config,idr,delta; };\n'
    for k, target in [('h264', 'hevc'), ('hevc', 'h264')]:
        out = encode(['-f', k, '-i', 'pipe:0'], streams[k], target == 'hevc')
        for name, seq in [(('avc' if k == 'h264' else 'hevc') + 'Source', fixture(streams[k], k == 'hevc')),
                          (('avc' if target == 'h264' else 'hevc') + 'Target', fixture(out, target == 'hevc'))]:
            header += 'inline const Fixture ' + name + '={' + ','.join('{' + ','.join(map(str, x)) + '}' for x in seq) + '};\n'
    with tempfile.TemporaryDirectory(prefix='record-private-codec-') as tmp:
        work = Path(tmp)
        for h in ('native_avbuffer', 'native_avcapability', 'native_avcodec_videodecoder', 'native_avcodec_videoencoder'):
            file = work / ('multimedia/player_framework/' + h + '.h'); file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text('#include "record_video_encoder_mock.h"\n')
        (work / 'native_window').mkdir(); (work / 'native_window/external_window.h').write_text('#include "record_video_encoder_mock.h"\n')
        (work / 'record_fixtures.h').write_text(header)
        # Build the actual, small production routing methods, not a duplicate of their logic.
        def method(src, signature):
            pos = src.index(signature); body = src.index('{', pos); level = 0
            for end in range(body, len(src)):
                level += (src[end] == '{') - (src[end] == '}')
                if not level:
                    return src[pos:end+1] + '\n'
            raise AssertionError(signature)
        napi = (CPP / 'napi_init.cpp').read_text(encoding='utf-8')
        (work / 'record_fanout.inc').write_text(''.join(method(napi, 'void ' + s + '(') for s in ['PushVideo', 'PushAudio']), encoding='utf-8')
        (work / 'record_rtmp_clock.inc').write_text(method((CPP/'rtmp_publisher.cpp').read_text(encoding='utf-8'), 'bool RtmpPublisher::TimestampToMs('))
        exe = work / ('host.exe' if os.name == 'nt' else 'host')
        subprocess.run([cc, '-std=c++17', '-I', str(CPP), '-I', str(TESTS), '-I', str(work),
                        str(CPP/'record_video_encoder.cpp'), str(TESTS/'record_video_encoder_host.cpp'),
                        *([] if os.name == 'nt' else ['-pthread']), '-o', str(exe)], cwd=work, check=True)
        lines = subprocess.check_output([str(exe)], timeout=20).decode().splitlines()
        assert len(lines) == 2
        for line in lines:
            fmt, packet = line.split()
            image = ff(['-f', fmt, '-i', 'pipe:0', '-frames:v', '1', '-pix_fmt', 'yuv420p', '-f', 'rawvideo', 'pipe:1'], bytes.fromhex(packet))
            assert image == raw, 'transcoded muxer-facing config + first sample must decode to the source picture'
    print('PASS: actual AVC↔HEVC synthetic bitstreams, cached config, input/output IDR gates, shared RTMP clock, AAC gate, private failures/teardown and passthrough')


if __name__ == '__main__':
    main()
