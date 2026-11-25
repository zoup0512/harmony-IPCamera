"""Offline talk-back regression: synthetic SDK/FD/decoder doubles, optionally
also cross-link real production sources against a local OpenHarmony native SDK
for arm64 API 24: python tests/m4a_voice_test.py --sdk <openharmony/native>.
--ffmpeg <local executable> also synthesizes a silent M4A and remuxes it to
ADTS for the reference samples. No device, existing recordings, microphone,
downloads or third-party Python packages.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / 'entry/src/main/cpp'
SOURCES = [CPP / 'm4a_voice.cpp', CPP / 'voice_player.cpp']


def run(sdk, ffmpeg):
    with tempfile.TemporaryDirectory(prefix='m4a_voice_test_') as temp:
        work = Path(temp)
        # The test TU supplies the SDK declarations/implementations. Don't
        # rewrite the production parser, decoder queue, callbacks, or lifecycle.
        for source in SOURCES:
            for h in re.findall(r'#include <((?:multimedia|ohaudio|hilog|sys)/[^>]+|unistd.h)>',
                                source.read_text(encoding='utf-8')):
                stub = work / h
                stub.parent.mkdir(exist_ok=True, parents=True)
                stub.touch()
        test = Path(__file__).with_suffix('.cpp').resolve()
        if os.name == 'nt':
            vcvars = os.environ.get('VCVARS64_BAT', r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
            cmd = work / 'host.cmd'
            cmd.write_text(f'@call "{vcvars}" >nul || exit /b 1\n@cl /nologo /EHsc /std:c++17 /utf-8 /I"{work}" /I"{CPP}" "{test}" /Fe:test.exe\n')
            subprocess.run(['cmd.exe', '/d', '/c', str(cmd)], cwd=work, check=True, timeout=120)
            exe = work / 'test.exe'
        else:
            exe = work / 'test'
            subprocess.run(['c++', '-std=c++17', '-pthread', '-I', work, '-I', CPP, test, '-o', exe],
                           cwd=work, check=True, timeout=120)
        if ffmpeg:
            args = [ffmpeg.resolve(), '-nostdin', '-hide_banner', '-loglevel', 'error']
            subprocess.run([*args, '-f', 'lavfi', '-i', 'anullsrc=r=48000:cl=mono', '-t', '3',
                            '-c:a', 'aac', '-profile:a', 'aac_low', 'synth.m4a'], cwd=work, check=True, timeout=30)
            subprocess.run([*args, '-protocol_whitelist', 'file', '-i', 'synth.m4a', '-map', '0:a:0',
                            '-c:a', 'copy', '-f', 'adts', 'synth.aac'], cwd=work, check=True, timeout=30)
        subprocess.run([exe] + (['synthetic'] if ffmpeg else []), cwd=work, check=True, timeout=30)
        if sdk:
            sdk = sdk.resolve()
            clang = sdk / 'llvm/bin' / ('clang++.exe' if os.name == 'nt' else 'clang++')
            libs = ['native_media_avsource', 'native_media_avdemuxer', 'native_media_core',
                    'native_media_codecbase', 'native_media_adec', 'ohaudio', 'hilog_ndk.z']
            subprocess.run([clang, '--target=aarch64-linux-ohos24', f'--sysroot={sdk / "sysroot"}',
                            '-std=c++17', '-Werror=unguarded-availability', '-shared', '-fPIC',
                            '-Wl,--no-undefined', *SOURCES, *['-l' + lib for lib in libs],
                            '-o', work / 'libvoice.so'], cwd=work, check=True, timeout=120)
            version = json.loads((sdk / 'oh-uni-package.json').read_text(encoding='utf-8'))['apiVersion']
            print(f'PASS: arm64 API 24 compile and link (local native SDK package API {version})')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, help='optional local openharmony/native root')
    parser.add_argument('--ffmpeg', type=Path, help='optional local FFmpeg for a synthetic M4A fixture')
    args = parser.parse_args()
    run(args.sdk, args.ffmpeg)
