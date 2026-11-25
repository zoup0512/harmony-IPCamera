"""Synthetic RTSP/TCP + real UDP loopback. No device, DNS or non-loopback endpoints.
Compiles production negotiation, UDP sockets, source checks and depacketizers;
only TCP I/O, SDK decoder and clock are adapted for this host executable.
Run: python tests/rtsp_client_udp_test.py
"""
import contextlib
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parent))
from rtsp_client_digest_test import PREFIX, build, reply

HOST = '127.0.0.1'
PREFIX = PREFIX.split('struct Clock:')[0] + r'''
#include <atomic>
#include <thread>
struct Clock:std::chrono::steady_clock {
 static time_point now(){static auto zero=steady_clock::now();return zero+(steady_clock::now()-zero)*4;}
};
struct Monitor {void submit(Bytes&){std::abort();}};
struct Session {std::atomic<bool> stopped{false};VideoCodec videoCodec{};Monitor audio;};
struct AudioWorker {AudioWorker(Monitor&){} void start(AacTrack&){std::abort();}};
struct Sink {void init(uint64_t){} void push(const Bytes& f,int64_t,bool cfg,Session&,Clock::time_point& last){
 if(!cfg){last=Clock::now();std::cout<<"FRAME "<<int(f.back())<<std::endl;}}};
struct Socket {
 rtspudp::Datagram socket_;rtspudp::Fd fd=rtspudp::invalid;Bytes pending;size_t offset=0;
 Socket(Session&){} void connectTo(const std::string& h,const std::string& p){
  if(h!="127.0.0.1" && h!="::1")std::abort();rtspudp::Address a;int family=h=="::1"?AF_INET6:AF_INET;
  a.storage.ss_family=family;if(family==AF_INET){inet_pton(family,h.c_str(),&a.v4().sin_addr);a.size=sizeof(sockaddr_in);}
  else{inet_pton(family,h.c_str(),&a.v6().sin6_addr);a.size=sizeof(sockaddr_in6);}a.port(number(p,65535));
  socket_.fd=fd=socket(family,SOCK_STREAM,IPPROTO_TCP);if(connect(fd,a.addr(),a.size))throw Network;
 }
 void write(const std::string& s){size_t done=0;while(done<s.size()){auto n=send(fd,s.data()+done,int(s.size()-done),0);if(n<=0)throw Network;done+=n;}}
 void read(uint8_t* out,size_t count,Clock::time_point){while(count){auto n=recv(fd,(char*)out,int(count),0);if(n<=0)throw Network;out+=n;count-=n;}}
 uint8_t byte(Clock::time_point end){uint8_t b;read(&b,1,end);return b;}
};
'''
MAIN = r'''
int main(int argc,char** argv){
#ifdef _WIN32
 WSADATA w{};if(WSAStartup(MAKEWORD(2,2),&w))return 1;
#endif
 Session s;rtspdigest::Credentials creds{"viewer","fixture:password"};std::thread stop;
 if(std::string(argv[2])=="stop")stop=std::thread([&]{std::this_thread::sleep_for(500ms);s.stopped=true;});
 try{run<Sink>(s,argv[1],"",1,true,&creds,true);std::cout<<"STOP\n";}
 catch(Status st){std::cout<<"END "<<int(st)<<std::endl;} if(stop.joinable())stop.join();
}
'''


def ports(stack, family, host):
    for _ in range(20):  # local ephemeral allocation, not remote probing
        a = stack.enter_context(socket.socket(family, socket.SOCK_DGRAM)); a.bind((host, 0))
        b = stack.enter_context(socket.socket(family, socket.SOCK_DGRAM)); p = a.getsockname()[1]
        try: b.bind((host, p ^ 1))
        except OSError: continue
        return (b, a) if p & 1 else (a, b)
    raise RuntimeError('could not allocate loopback fixture pair')


def scenario(exe, case, host=HOST):
    family = socket.AF_INET if host == HOST else socket.AF_INET6
    with contextlib.ExitStack() as cx:
        listen = cx.enter_context(socket.socket(family)); listen.bind((host, 0)); listen.listen(1); listen.settimeout(4)
        url = 'rtsp://' + (host if family == socket.AF_INET else '['+host+']') + ':' + str(listen.getsockname()[1]) + '/live'
        rtp, rtcp = ports(cx, family, host)
        client = subprocess.Popen([exe, url, case], stdout=subprocess.PIPE, text=True)
        def cleanup():
            if client.poll() is None: client.kill()
            client.wait();client.stdout.close()
        cx.callback(cleanup)
        ctl = cx.enter_context(listen.accept()[0]); ctl.settimeout(5)
        reader = cx.enter_context(ctl.makefile('rb')); seq = 0
        def req(method, suffix=''):
            nonlocal seq
            seq += 1; assert reader.readline() == f'{method} {url}{suffix} RTSP/1.0\r\n'.encode()
            fields = {}
            while True:
                line = reader.readline(); assert line
                if line == b'\r\n': break
                name, val = line.decode().strip().split(': ', 1);fields[name]=val
            assert fields['CSeq'] == str(seq)
            if seq == 1: assert 'Authorization' not in fields
            else: assert fields['Authorization'].startswith('Digest ') and f'nc={seq-1:08x}' in fields['Authorization']
            if method in ('PLAY', 'OPTIONS'): assert fields.get('Session') == 'sid'
            return fields
        def answer(code=200, headers='', body=''):
            ctl.sendall(reply(seq, code, headers, body).encode())
        def finish(status, frames=()):
            output = client.communicate(timeout=5)[0].splitlines()
            assert client.returncode == 0 and output == [*(f'FRAME {n}' for n in frames), status], (case,output)
        req('DESCRIBE');answer(401,'WWW-Authenticate: Digest realm="lab", nonce="n", algorithm=SHA-256, qop=auth\r\n')
        codec = 'H265' if case == 'hevc' else 'H264'
        req('DESCRIBE');answer(body=f'm=video 0 RTP/AVP 96\r\na=rtpmap:96 {codec}/90000\r\na=control:v\r\n' +
                              'm=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-generic/44100/2\r\na=control:a\r\n')
        offer = req('SETUP','/v')['Transport'];m = re.fullmatch(r'RTP/AVP;unicast;client_port=(\d+)-(\d+)',offer); assert m
        p,q = map(int,m.groups()); assert not p%2 and p+1==q
        server = f';server_port={rtp.getsockname()[1]}-{rtcp.getsockname()[1]}'
        value = offer + server + f';source={host};destination={host};ssrc=00000007'
        bad = {'source':value.replace(f'source={host}', 'source=127.0.0.2'),
               'dest':value.replace(f'destination={host}', 'destination=224.0.0.1'),
               'missing':offer,'ports':value.replace(f'client_port={p}-{q}',f'client_port={q}-{q+1}'),
               'multi':value.replace('unicast','multicast'),'alternative':value+',RTP/AVP/TCP',
               'duplicate':value+';server_port=1-2','tcp':'RTP/AVP/TCP;unicast;interleaved=0-1'}
        if case in bad:
            answer(headers='Transport: '+bad[case]+'\r\nSession: sid\r\n');finish('END -4');return
        if case=='wrongpass': answer(401);finish('END -2');return
        if case=='bare':value=offer+server # default source is the RTSP peer; no advertised SSRC
        answer(headers='Transport: '+value+'\r\nSession: sid;timeout=2\r\n');req('PLAY')
        if case=='bad-session':answer(headers='Session: other\r\n');finish('END -5');return
        answer()
        if case=='stop': finish('STOP');return
        if case=='silent':
            # Answer keepalives, send no frames. The media deadline must still fire.
            while reader.peek(1): req('OPTIONS');answer()
            finish('END -7');return
        if case=='keep-timeout':req('OPTIONS');finish('END -7');return
        addr=(host,p);ctrl=(host,q)
        def packet(n,payload,ssrc=7,stamp=None,pt=96,mark=True):
            return struct.pack('!BBHII',128,pt|(128 if mark else 0),n,n*3000 if stamp is None else stamp,ssrc)+payload
        nal=b'\x26\x01' if codec=='H265' else b'\x65'
        def single(n,val=222,**kw):return packet(n,nal+bytes([val]),**kw)
        wrong=cx.enter_context(socket.socket(family,socket.SOCK_DGRAM));wrong.bind((host,0));wrong.sendto(single(1),addr)
        if host == HOST:
            foreign=cx.enter_context(socket.socket(family,socket.SOCK_DGRAM));foreign.bind(('127.0.0.2',rtp.getsockname()[1]));foreign.sendto(single(1),addr)
        bye=lambda id:struct.pack('!BBHI',129,203,1,id)
        rtcp.sendto(bye(99),ctrl);rtcp.sendto(bye(7)+b'bad',ctrl);rtp.sendto(bye(7),addr)
        rtcp.sendto(single(1),ctrl)
        if case!='bare':rtp.sendto(single(1,ssrc=99),addr)
        rtp.sendto(single(1,pt=98),addr)
        rtp.sendto(b'\x90\xe0'+b'\0'*12,addr) # truncated extension
        rtp.sendto(single(1,11),addr);rtp.sendto(single(1),addr);rtp.sendto(single(0),addr)
        rtp.sendto(single(3),addr) # missing 2: reject damaged AU, recover at next timestamp
        rtp.sendto(single(4,12),addr)
        if codec=='H264':
            rtp.sendto(packet(5,b'\x7c\x85\xaa',stamp=15000,mark=False),addr)
            rtp.sendto(packet(7,b'\x7c\x45\xdd',stamp=15000),addr) # lost middle
            rtp.sendto(single(8,13),addr)
        req('OPTIONS');answer()
        sr=struct.pack('!BBH',128,200,6)+bytes(24)
        sdes=struct.pack('!BBHI',129,202,2,7)+b'\x01\x01x\0'
        rtcp.sendto(sr+sdes+bye(7),ctrl)
        finish('END -1', (11,12,13) if codec=='H264' else (11,12))


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as d:
        exe=build(Path(d),PREFIX,MAIN)
        for case in ('avc','hevc','bare','source','dest','missing','ports','multi','alternative','duplicate','tcp','wrongpass','bad-session','stop','silent','keep-timeout'):
            scenario(exe,case)
        scenario(exe,'hevc','::1')
    print('PASS: synthetic UDP unicast IPv4/IPv6, SETUP/PLAY/Digest, packet filtering, RTCP, stop and deadlines')
