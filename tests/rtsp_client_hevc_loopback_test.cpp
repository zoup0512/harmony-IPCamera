// Offline POSIX test: compile with rtsp_server.cpp, C++17, OHOS SDK declaration headers,
// -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections,--wrap=bind. No SDK runtime required.
// Exercise the production client, including Decoder, up to the SDK boundary.
#include "../entry/src/main/cpp/rtsp_client.cpp"
#include "../entry/src/main/cpp/rtsp_server.h"
#include <hilog/log.h>
#include <sys/time.h>
#include <cassert>
#include <cerrno>
#include <iostream>

namespace {
// Real 64x64 yuv420p black IDRs encoded locally with libx265 / libx264.
const std::string vps="QAEMAf//AWAAAAMAkAAAAwAAAwAeugJA", sps="QgEBAWAAAAMAkAAAAwAAAwAeoCCBBZbqSTK8BaAgAAADACAAAAMAIQ==",
                  pps="RAHBcrQiQA==", avcSps="Z0LACtkEJsBEAAADAAQAAAMACDxImSA=", avcPps="aMuDyyA=";
const Bytes hevcIdr=unbase64("KAGvE4DmaOP//RfPx/bP"), avcIdr=unbase64("ZYiEBX///w9FAAFC3ycnJ1111111111114A=");
Bytes annex(std::initializer_list<Bytes> nals) {
  Bytes out; for (auto& nal:nals) {out.insert(out.end(),{0,0,0,1});out.insert(out.end(),nal.begin(),nal.end());} return out;
}
struct Fixture {
  bool avc=false, oversize=false, pending=false;
  OH_AVCodecBufferAttr attr{}; uint8_t data[512]{};
  int configs=0, renders=0; std::atomic<int> samples{0};
  const char* expectedMime() const {return avc?"video/avc":"video/hevc";}
};
Fixture* stub; // One client thread at a time; the server only reads atomic sample count.
}
struct OH_AVCodec {}; struct OH_AVFormat {}; struct OH_AVBuffer {}; struct NativeWindow {};
extern "C" {
const char* OH_MD_KEY_VIDEO_PIC_WIDTH="width"; const char* OH_MD_KEY_VIDEO_PIC_HEIGHT="height";
const char* OH_MD_KEY_ENABLE_SYNC_MODE="sync"; const char* OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY="lowLatency";
int32_t OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t id,OHNativeWindow** out){assert(id==123);*out=new NativeWindow;return 0;}
void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow* w){delete w;}
OH_AVCodec* OH_VideoDecoder_CreateByMime(const char* m){assert(!strcmp(m,stub->expectedMime()));return new OH_AVCodec;}
OH_AVFormat* OH_AVFormat_CreateVideoFormat(const char* m,int32_t w,int32_t h){assert(!strcmp(m,stub->expectedMime()) && w==kMaxWidth && h==kMaxHeight);return new OH_AVFormat;}
bool OH_AVFormat_SetIntValue(OH_AVFormat*,const char* key,int32_t v){assert((key==OH_MD_KEY_ENABLE_SYNC_MODE || key==OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY) && v==1);return true;}
void OH_AVFormat_Destroy(OH_AVFormat* f){delete f;}
OH_AVErrCode OH_VideoDecoder_Configure(OH_AVCodec*,OH_AVFormat*){return AV_ERR_OK;}
OH_AVErrCode OH_VideoDecoder_SetSurface(OH_AVCodec*,OHNativeWindow*){return AV_ERR_OK;}
OH_AVErrCode OH_VideoDecoder_Prepare(OH_AVCodec*){return AV_ERR_OK;}
OH_AVErrCode OH_VideoDecoder_Start(OH_AVCodec*){return AV_ERR_OK;}
OH_AVErrCode OH_VideoDecoder_Destroy(OH_AVCodec* c){delete c;return AV_ERR_OK;}
OH_AVErrCode OH_VideoDecoder_QueryInputBuffer(OH_AVCodec*,uint32_t* n,int64_t){*n=0;return AV_ERR_OK;}
OH_AVBuffer* OH_VideoDecoder_GetInputBuffer(OH_AVCodec*,uint32_t n){assert(n==0);static OH_AVBuffer in;return &in;}
int32_t OH_AVBuffer_GetCapacity(OH_AVBuffer*){return sizeof(stub->data);}
uint8_t* OH_AVBuffer_GetAddr(OH_AVBuffer*){return stub->data;}
OH_AVErrCode OH_AVBuffer_SetBufferAttr(OH_AVBuffer*,const OH_AVCodecBufferAttr* a){stub->attr=*a;return AV_ERR_OK;}
OH_AVErrCode OH_VideoDecoder_PushInputBuffer(OH_AVCodec*,uint32_t){
  auto& f=*stub; const auto& a=f.attr; assert(a.size>0 && size_t(a.size)<=sizeof(f.data));
  Bytes bytes(f.data,f.data+a.size);
  if(a.flags==AVCODEC_BUFFER_FLAGS_CODEC_DATA){
    assert(f.configs++==0 && !f.samples && a.pts==0);
    assert(bytes==(f.avc?annex({unbase64(avcSps),unbase64(avcPps)}):annex({unbase64(vps),unbase64(sps),unbase64(pps)})));
  }else{
    assert(f.configs==1 && a.flags==AVCODEC_BUFFER_FLAGS_NONE && a.pts==f.samples*100000);
    assert(bytes==annex({f.avc?avcIdr:hevcIdr}));
    if(++f.samples==2)f.pending=true; // no decoded output for the first accepted slice
  }
  return AV_ERR_OK;
}
OH_AVErrCode OH_VideoDecoder_QueryOutputBuffer(OH_AVCodec*,uint32_t* index,int64_t){
  if(!stub->pending)return AV_ERR_TRY_AGAIN_LATER;stub->pending=false;*index=7;return AV_ERR_OK;
}
OH_AVFormat* OH_VideoDecoder_GetOutputDescription(OH_AVCodec*){return new OH_AVFormat;}
bool OH_AVFormat_GetIntValue(OH_AVFormat*,const char* key,int32_t* v){
  assert(key==OH_MD_KEY_VIDEO_PIC_WIDTH || key==OH_MD_KEY_VIDEO_PIC_HEIGHT);
  *v=(stub->oversize && key==OH_MD_KEY_VIDEO_PIC_WIDTH)?3840:64;return true;
}
OH_AVErrCode OH_VideoDecoder_RenderOutputBuffer(OH_AVCodec*,uint32_t n){assert(n==7);++stub->renders;return AV_ERR_OK;}
}
namespace ipcam {
struct VoicePlayer::Impl {};
VoicePlayer::VoicePlayer()=default; VoicePlayer::~VoicePlayer()=default;
void VoicePlayer::Stop(){} VoicePlayer::FeedResult VoicePlayer::Feed(const uint8_t*,size_t){std::abort();}
}

static void sendAll(int fd,const void* data,size_t size){
  auto p=static_cast<const uint8_t*>(data);
  while(size){auto n=send(fd,p,size,MSG_NOSIGNAL);assert(n>0);p+=n;size-=n;}
}
static void scenario(bool dualTrack,bool oversize){
  Fixture f; f.avc=dualTrack;f.oversize=oversize;stub=&f;
  int server=socket(AF_INET,SOCK_STREAM,0);assert(server>=0);
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  socklen_t len=sizeof(addr);assert(bind(server,reinterpret_cast<sockaddr*>(&addr),len)==0 && listen(server,1)==0);
  assert(getsockname(server,reinterpret_cast<sockaddr*>(&addr),&len)==0);
  std::string url="rtsp://127.0.0.1:"+std::to_string(ntohs(addr.sin_port))+"/cam";
  Session s; Status ended=Connecting;
  std::thread client([&]{try{run(s,url,"dGVzdDp0ZXN0",123,false);}catch(Status st){ended=st;}});
  pollfd ready{server,POLLIN,0};assert(poll(&ready,1,3000)==1);
  int fd=accept(server,nullptr,nullptr);assert(fd>=0);timeval timeout{3,0};assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
  int seq=0;
  auto request=[&](const std::string& method,const std::string& suffix,const std::string& header){
    std::string msg;char ch;while(msg.size()<4096 && (msg.size()<4 || msg.compare(msg.size()-4,4,"\r\n\r\n")!=0)){assert(recv(fd,&ch,1,0)==1);msg+=ch;}
    assert(msg==method+" "+url+suffix+" RTSP/1.0\r\nCSeq: "+std::to_string(++seq)+"\r\nAuthorization: Basic dGVzdDp0ZXN0\r\n"+header+"\r\n");
  };
  auto reply=[&](const std::string& headers,const std::string& body=""){
    auto msg="RTSP/1.0 200 OK\r\nCSeq: "+std::to_string(seq)+"\r\n"+headers+"Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    sendAll(fd,msg.data(),msg.size());
  };
  std::string sdp="v=0\r\na=control:*\r\nm=video 0 RTP/AVP 110\r\na=rtpmap:110 H265/90000\r\na=control:main\r\na=fmtp:110 sprop-vps="+vps+";sprop-sps="+sps+";sprop-pps="+pps+"\r\n";
  if(dualTrack)sdp+="m=video 0 RTP/AVP 102\r\na=rtpmap:102 H264/90000\r\na=control:sub\r\na=fmtp:102 sprop-parameter-sets="+avcSps+","+avcPps+"\r\n";
  request("DESCRIBE","","Accept: application/sdp\r\n");reply("Content-Base: "+url+"/tracks/\r\n",sdp);
  request("SETUP",dualTrack?"/tracks/sub":"/tracks/main","Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
  assert(s.videoCodec==(dualTrack?VideoCodec::Avc:VideoCodec::Hevc) && s.status==Connecting);
  reply("Transport: RTP/AVP/TCP;unicast;interleaved=10-11\r\nSession: local;timeout=60\r\n");
  request("PLAY","","Session: local\r\n");reply("Session: local\r\n");
  int pt=dualTrack?102:110;auto idr=dualTrack?avcIdr:hevcIdr;
  auto packet=[&](int channel,int payloadType,int seq,int ts,bool marker,const Bytes& nal){
    Bytes b={'$',uint8_t(channel),0,uint8_t(nal.size()+12),0x80,uint8_t(payloadType|(marker?128:0)),0,uint8_t(seq),0,0,uint8_t(ts>>8),uint8_t(ts),0,0,0,1};
    b.insert(b.end(),nal.begin(),nal.end());sendAll(fd,b.data(),b.size());
  };
  // Earlier timestamps / different bytes expose accidental acceptance of a decoy.
  for(int ch:{0,11})packet(ch,pt,1,1,true,{0x26,1,0xab});
  packet(10,98,1,1,true,{0x26,1,0xab});
  packet(10,pt,1,9000,true,idr);
  auto end=Clock::now()+2s;while(!f.samples && Clock::now()<end)std::this_thread::yield();
  assert(f.samples==1 && s.status==Connecting);
  if(dualTrack)packet(10,pt,2,18000,true,idr);
  else{
    Bytes first{98,1,0x94}, last{98,1,0x54}; // RFC 7798 IDR_N_LP FU
    first.insert(first.end(),idr.begin()+2,idr.begin()+8);last.insert(last.end(),idr.begin()+8,idr.end());
    packet(10,pt,2,18000,false,first);packet(10,pt,3,18000,true,last);
  }
  shutdown(fd,SHUT_WR);client.join();close(fd);close(server);
  assert(f.configs==1 && f.samples==2 && f.renders==(oversize?0:1));
  assert(s.status==(oversize?Connecting:Playing) && ended==(oversize?Codec:Network));
}
// Also run the repository server against the repository client. Intercept the server's
// INADDR_ANY bind before it reaches the kernel; these tests never listen on a LAN.
static int port=0;
extern "C" int OH_LOG_Print(LogType,LogLevel,unsigned,const char*,const char*,...){return 0;}
extern "C" int __real_bind(int,const sockaddr*,socklen_t);
extern "C" int __wrap_bind(int fd,const sockaddr* a,socklen_t len){
  if (a->sa_family == AF_INET6) { errno = EAFNOSUPPORT; return -1; }
  assert(a->sa_family==AF_INET);auto local=*reinterpret_cast<const sockaddr_in*>(a);
  local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  int rc=__real_bind(fd,reinterpret_cast<const sockaddr*>(&local),len);
  if(!rc){assert(getsockname(fd,reinterpret_cast<sockaddr*>(&local),&len)==0);port=ntohs(local.sin_port);}return rc;
}
static void repositoryServer(){
  Fixture f;stub=&f;std::atomic<bool> playing{false};
  ipcam::RtspServer server;ipcam::RtspConfig cfg;cfg.port=0;cfg.user="test";cfg.password="test";
  assert(server.Start(cfg,[&](ipcam::RtspEvent ev,int,const std::string&){if(ev==ipcam::RtspEvent::kClientPlaying)playing=true;}));
  auto sets=annex({unbase64(vps),unbase64(sps),unbase64(pps)}),au=annex({hevcIdr});
  server.PushH264(sets.data(),sets.size(),0); // legacy public entry point auto-detects HEVC
  Session s;std::string url="rtsp://127.0.0.1:"+std::to_string(port)+"/live";
  std::thread client([&]{try{run(s,url,"dGVzdDp0ZXN0",123,false);}catch(Status){assert(s.stopped);}});
  auto wait=[](auto done){auto end=Clock::now()+3s;while(!done()){assert(Clock::now()<end);std::this_thread::sleep_for(1ms);}};
  wait([&]{return playing.load();});assert(s.videoCodec==VideoCodec::Hevc);
  server.PushH264(au.data(),au.size(),100000);wait([&]{return f.samples==1;});assert(s.status==Connecting);
  server.PushH264(au.data(),au.size(),200000);wait([&]{return s.status==Playing;});
  s.stop();client.join();server.Stop();assert(f.renders==1 && f.samples==2 && f.configs==1);
}
int main(){scenario(false,false);scenario(true,false);scenario(false,true);repositoryServer();
  std::cout<<"PASS: HEVC-only handshake, remapped RTP/FU samples and MIME, AVC preference, actual render gate and bounded output\n";
}
