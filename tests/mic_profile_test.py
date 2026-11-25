"""Linux/WSL: python3 tests/mic_profile_test.py. No network or device.
Windows: set ANDROID_NDK_HOME to a local NDK; uses its static x86_64 compiler
and executes the resulting mocks on the WSL host kernel (not an emulator).

Compile production mic, Opus and RTSP. Only the audio SDK is mocked; Ogg bytes
are read over a local Unix socketpair (no listener). Pass both resulting ADTS
files through the production recorder with the existing mock muxer, checking the
actual MP4 track sample rate, channel count and AudioSpecificConfig.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import sys

TESTS = Path(__file__).resolve().parent
CPP = TESTS.parent / 'entry/src/main/cpp'
sys.path.insert(0, str(TESTS))
from recorder_archive_test import SDK_STUB

RECORDER = r'''
#include <cassert>
#include <fstream>
#include <iterator>
#include "recorder.h"
#include "stub.h"
using Bytes = std::vector<uint8_t>;
int main() {
  for (bool stereo : {false,true}) {
    std::ifstream file(stereo ? "stereo.adts" : "mono.adts", std::ios::binary);
    Bytes adts((std::istreambuf_iterator<char>(file)), {});
    assert(!adts.empty());
    ipcam::StreamRecorder r;
    r.SetVideoParams(false, {}, {0x67,0x64,0,0x1f}, {0x68,0x11}, 640,480);
    assert(r.Start(stereo ? "stereo.mp4" : "mono.mp4", 0, [](auto&) { assert(false); }));
    Bytes key{0,0,1,0x65,0x88};
    r.FeedAudio(adts.data(),adts.size(),0);
    r.FeedVideo(key.data(),key.size(),0);
    r.FeedAudio(adts.data(),adts.size(),20000);
    r.Stop();
    auto track = sdk_mock::muxers.back().tracks.at(1).format;
    assert(track.rate == (stereo ? 44100 : 16000) && track.channels == (stereo ? 2 : 1));
    assert(track.config == (stereo ? Bytes{0x12,0x10} : Bytes{0x14,0x08}));
  }
}
'''


def run():
    with tempfile.TemporaryDirectory(prefix='mic-profile-') as tmp:
        work = Path(tmp)
        def shims(root, subdir, headers, body):
            folder = root / subdir
            folder.mkdir(parents=True, exist_ok=True)
            for header in headers.split():
                (folder / (header + '.h')).write_text(body)
        media = 'multimedia/player_framework'
        audio = work / 'audio'
        stub = '#include "mic_profile_mock.h"\n'
        shims(audio, 'hilog', 'log', stub)
        shims(audio, 'ohaudio', 'native_audio_common native_audiocapturer native_audiostreambuilder native_audiostream_base', stub)
        shims(audio, media, 'native_avcodec_base native_avcapability native_avcodec_audioencoder native_avformat native_avmemory', stub)
        cc = [os.getenv('CXX', 'c++')]
        def execute(exe):
            cmd = ['wsl.exe', '--', './'+exe] if os.name == 'nt' else [str(work/exe)]
            subprocess.run(cmd, cwd=work, timeout=30, check=True)
        if os.name == 'nt':
            tools = Path(os.environ['ANDROID_NDK_HOME'])/'toolchains/llvm/prebuilt/windows-x86_64'
            cc = [str(tools/'bin/clang++.exe'), '--target=x86_64-linux-android26',
                  '--sysroot='+str(tools/'sysroot'), '-static']
        cc += ['-std=c++17', '-pthread', '-I', str(CPP)]
        subprocess.run([*cc, '-I', str(TESTS), '-I', str(audio), str(TESTS/'mic_profile_host.cpp'),
                        *(str(CPP / (f+'.cpp')) for f in ('mic_streamer','opus_stream','rtsp_server')), '-o', 'profile'], cwd=work, check=True)
        execute('profile')
        mux = work / 'mux'
        shims(mux, 'hilog', 'log', '#define OH_LOG_Print(...) ((void)0)\n')
        shims(mux, media, 'native_avbuffer native_avcodec_base native_avformat native_avmuxer native_avmemory', '#include "stub.h"\n')
        (mux/'stub.h').write_text('#include <unistd.h>\n#include <cassert>\n#define _write write\n#define CHECK assert\n'+SDK_STUB)
        (work/'verify_mux.cpp').write_text(RECORDER)
        subprocess.run([*cc, '-I', str(mux), str(CPP/'recorder.cpp'), 'verify_mux.cpp', '-o', 'mux_test'], cwd=work, check=True)
        execute('mux_test')
        print('PASS: ADTS from the real mic callbacks configures both MP4 audio tracks')


if __name__ == '__main__':
    run()
