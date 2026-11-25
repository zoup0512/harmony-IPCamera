"""Offline transcript test. No sockets, devices, DNS, real endpoints or credentials.
Builds the actual response parser and runTracks/run templates against an in-memory
socket, fake clock and sinks. Test hash primitive: Windows CNG / host OpenSSL.
Run: python tests/rtsp_client_digest_test.py
"""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

CPP = Path(__file__).resolve().parents[1] / 'entry/src/main/cpp'
URL = 'rtsp://127.0.0.1:8554/live'
CREDS = 'viewer:lab,one:fixture:password'
CN = '0123456789abcdef0123456789abcdef'
PREFIX = r'''
#include "rtsp_client_hevc.h"
#include "rtsp_client_digest.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#else
#include <openssl/evp.h>
#endif
#include "rtsp_client_udp.h"
namespace rtspdigest {
std::string hash(bool sha,const std::string& s) {
  uint8_t out[32]; unsigned n=sha?32:16;
#ifdef _WIN32
  if(BCryptHash(sha?BCRYPT_SHA256_ALG_HANDLE:BCRYPT_MD5_ALG_HANDLE,nullptr,0,
     (PUCHAR)s.data(),(ULONG)s.size(),out,n)) std::abort();
#else
  if(!EVP_Digest(s.data(),s.size(),out,&n,sha?EVP_sha256():EVP_md5(),nullptr)) std::abort();
#endif
  return hex(out,n);
}
std::string randomNonce(){return "0123456789abcdef0123456789abcdef";}
}
using namespace liveavc;
using namespace std::chrono_literals;
struct Clock:std::chrono::steady_clock {
 static inline int untilKeepAlive=0;
 static time_point now(){static time_point t{};if(untilKeepAlive && !--untilKeepAlive)t+=2s;return t;}
};
struct Monitor {void submit(Bytes&) {}};
struct Session {bool stopped=false;VideoCodec videoCodec{}; Monitor audio;};
struct AudioWorker {AudioWorker(Monitor&){} void start(AacTrack&) {}};
struct Sink {void init(uint64_t){} void push(const Bytes&,int64_t,bool,Session&,Clock::time_point&) {}};
struct Socket {
 rtspudp::Fd fd=rtspudp::invalid;Bytes pending;size_t offset=0;
 Socket(Session&){} void connectTo(const std::string& h,const std::string& p){if(h!="127.0.0.1" || p!="8554") std::abort();std::cout<<"CONNECT\n";}
 void write(const std::string& s){std::cout<<s;if(s.rfind("PLAY ",0)==0)Clock::untilKeepAlive=3;}
 void read(uint8_t* p,size_t n,Clock::time_point){if(!std::cin.read((char*)p,n))throw Network;}
 uint8_t byte(Clock::time_point t){uint8_t b;read(&b,1,t);return b;}
};
'''
MAIN = r'''
int main(int argc,char** argv){
#ifdef _WIN32
 _setmode(_fileno(stdin),_O_BINARY); _setmode(_fileno(stdout),_O_BINARY);
#endif
 std::string mode=argv[1]; Session s; rtspdigest::Credentials creds{"viewer","fixture:password"};
 if(mode=="empty")creds.password.clear();
 try {run<Sink>(s,"rtsp://127.0.0.1:8554/live",mode=="basic"?"YTpz":"",1,true,
       mode=="basic" || mode=="anonymous"?nullptr:&creds);}catch(Status st){std::cout<<"END "<<int(st);}
}
'''


def build(directory, prefix=PREFIX, main=MAIN):
    code = (CPP / 'rtsp_client.cpp').read_text(encoding='utf-8')
    def section(a, b):
        return code[code.index(a):code.index(b, code.index(a))]
    source = (section('enum Status', 'enum AudioState') + section('enum class VideoCodec', 'constexpr const char* mime'))
    src = directory / 'fixture.cpp'
    src.write_text(prefix.replace('struct Monitor', source + 'struct Monitor') +
                   section('struct Reply', 'template<VideoCodec codecType>') +
                   section('template<class AvcSink, class HevcSink>\nbool runTracks', '\nvoid run(Session& s,const std::string& url,const std::string& auth,uint64_t surface,bool offerAudio,') + main, encoding='utf-8')
    if os.name == 'nt':
        vcvars = os.environ.get('VCVARS64_BAT', r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
        cmd = directory / 'build.cmd'
        cmd.write_text(f'@call "{vcvars}" >nul && cl /nologo /std:c++17 /EHsc /I"{CPP}" fixture.cpp bcrypt.lib ws2_32.lib /Fefixture.exe\n')
        subprocess.run(['cmd.exe', '/d', '/c', 'build.cmd'], cwd=directory, check=True)
        return directory / 'fixture.exe'
    exe = directory / 'fixture'
    subprocess.run(['c++', '-std=c++17', '-I', str(CPP), str(src), '-lcrypto', '-o', str(exe)], check=True)
    return exe


def reply(seq, code, headers='', body=''):
    return f'RTSP/1.0 {code} Reply\r\nCSeq: {seq}\r\n{headers}Content-Length: {len(body)}\r\n\r\n{body}'


def test(exe):
    def run(script, count, status, mode='digest'):
        out = subprocess.run([exe, mode], input=script.encode(), capture_output=True, check=True, timeout=5).stdout.decode()
        assert out.count('CONNECT\n') == (mode != 'empty') and out.count(' RTSP/1.0\r\n') == count
        assert out.endswith(f'END {status}') and 'fixture:password' not in out
        assert ('Authorization: Basic' in out) == (mode == 'basic')
        return out

    field = 'WWW-Authenticate: Digest realm="lab,one", nonce="nonce1"'
    challenge = field + ', algorithm=SHA-256, qop="auth-int,auth", opaque="a\\\"b"\r\n'
    sdp = 'm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:v\r\n'
    setup = 'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nSession: inner;timeout=2\r\n'
    first = reply(1, 401, 'WWW-Authenticate: Basic realm="other"\r\n' + field + ', algorithm=MD5\r\n' + challenge, 'challenge body')
    for name, offer, qop in [('sha256', first, True), ('md5', reply(1, 401, field + ', qop=auth\r\n'), True), ('md5', reply(1, 401, field + '\r\n'), False)]:
        script = offer + reply(2, 200, body=sdp) + reply(3, 200, setup) + reply(4, 200) + reply(5, 403)
        out = run(script, 5, -2)
        req = out.split('\r\n\r\n'); assert 'Authorization:' not in req[0]
        h = lambda s: hashlib.new(name, s.encode()).hexdigest()
        for nc, (method, uri) in enumerate([('DESCRIBE', URL), ('SETUP', URL + '/v'), ('PLAY', URL), ('OPTIONS', URL)], 1):
            r = req[nc]; seq = nc + 1
            assert r.startswith(f'{method} {uri} RTSP/1.0\r\nCSeq: {seq}\r\nAuthorization: Digest ')
            assert f'uri="{uri}"' in r and 'realm="lab,one"' in r
            mid = f'{nc:08x}:{CN}:auth:' if qop else ''
            expected = h(h(CREDS) + ':nonce1:' + mid + h(method + ':' + uri))
            assert f'response="{expected}"' in r and ('cnonce=' in r) == qop
            if qop: assert f'qop=auth, nc={nc:08x}, cnonce="{CN}"' in r
            if name == 'sha256': assert 'opaque="a\\\"b"' in r and 'algorithm=SHA-256' in r

    def withheld(script, count, status, mode='digest'):
        assert 'Authorization:' not in run(script, count, status, mode)
    for bad in [field + ', algorithm=SHA-512', field + ', qop="auth-int"', field + ', nonce=again',
                field + ', algorithm="', 'WWW-Authenticate: Basic realm="lab"']:
        withheld(reply(1, 401, bad+'\r\n'), 1, -8)
    withheld(reply(2, 401, challenge), 1, -5)  # CSeq mismatch
    withheld(reply(1, 403, challenge), 1, -2)
    withheld(reply(1, 302, 'Location: rtsp://127.0.0.1:8555/live\r\n'), 1, -5)
    withheld(reply(1, 200, challenge, sdp) + reply(2, 403), 2, -2)
    withheld(first, 1, -2, 'anonymous'); withheld(first, 0, -2, 'empty')
    basic = run(reply(1, 403), 1, -2, 'basic'); assert 'Authorization: Basic YTpz\r\n' in basic
    for code in (401, 403):
        run(first + reply(2, code, field + ', stale=true\r\n', 'cut')[:-3], 2, -2)
    withheld(reply(1, 401, 'CSeq: 2\r\n' + challenge), 1, -5)
    withheld(first[:-1], 1, -1)  # incomplete first 401 body: no authenticated send
    run(first + reply(2, 302, 'Location: rtsp://127.0.0.1:8555/live\r\n'), 2, -5)
    for hdr, body in [('', sdp.replace('control:v', 'control:rtsp://127.0.0.1:8555/v')),
                      ('Content-Base: rtsp://127.0.0.1:8555/live\r\n', sdp)]:
        run(first + reply(2, 200, hdr, body), 2, -5)
    audio = 'm=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-generic/44100/2\r\na=control:audio\r\na=fmtp:97 mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210\r\n'
    late = reply(1, 200, body=sdp+audio) + reply(2, 200, setup) + reply(3, 401, challenge) + reply(4, 401, challenge)
    assert run(late, 4, -2).count('Authorization: Digest') == 1  # no video-only recovery
    late = reply(1, 200, body=sdp) + reply(2, 200, setup) + reply(3, 200) + reply(4, 401, challenge) + reply(5, 401, challenge)
    assert run(late, 5, -2).count('Authorization: Digest') == 1  # OPTIONS challenge


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as d:
        test(build(Path(d)))
    print('PASS: offline RTSP Digest request and rejection transcripts; legacy Basic preserved')
