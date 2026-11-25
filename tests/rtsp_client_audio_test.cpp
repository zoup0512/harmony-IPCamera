// Standalone POSIX loopback test. Compile as C++17 together with rtsp_server.cpp,
// SDK declaration headers, -ffunction-sections -fdata-sections -pthread and
// -Wl,--gc-sections,--wrap=bind. Only the hardware decoder/renderer sinks are doubles.
#include "../entry/src/main/cpp/rtsp_client.cpp"
#include "../entry/src/main/cpp/rtsp_server.h"
#include "../entry/src/main/cpp/codec_records.h"
#include <cassert>
#include <cerrno>
#include <iostream>
#include <hilog/log.h>

extern "C" int OH_LOG_Print(LogType,LogLevel,unsigned,const char*,const char*,...) { return 0; }

static std::atomic<int> feeds{0}, renders{0}, stops{0};
static std::atomic<bool> failOutput{false};
static const Bytes adts={0xff,0xf1,0x50,0x80,1,0x5f,0xfc,0x11,0x22,0x33};
static const Bytes videoFrame={0,0,0,1,0x65,0x88};
namespace ipcam {
struct VoicePlayer::Impl {};
VoicePlayer::VoicePlayer()=default;
VoicePlayer::~VoicePlayer(){ Stop(); }
void VoicePlayer::Stop(){ if(active_.exchange(false)) ++stops; }
VoicePlayer::FeedResult VoicePlayer::Feed(const uint8_t* data,size_t size){
  assert(Bytes(data,data+size)==adts);
  if (failOutput) return FeedResult::Unavailable;
  active_=true; ++feeds; return FeedResult::Accepted;
}
}
struct VideoSink {
  void init(uint64_t) {}
  void push(const Bytes& b,int64_t,bool config,Session& s,Clock::time_point& last){
    assert(!config && b==videoFrame); ++renders; s.status=Playing; last=Clock::now();
  }
};

static const std::string format="streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210";
static const std::string media="m=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-generic/44100/2\r\na=control:audio\r\na=fmtp:97 ";
static Bytes packet(Bytes body,int seq=1,bool marker=true) {
  Bytes b={0x80,uint8_t(97|(marker?128:0)),0,uint8_t(seq),0,0,0,8,0,0,0,1};
  b.insert(b.end(),body.begin(),body.end());return b;
}
static void units() {
  AacTrack tr; assert(selectAac(media+format,tr) && tr.pt==97 && tr.sfIndex==4 && tr.channels==2);
  for (auto change:{";sizelength=9",";ctsdeltalength=6",";auxiliarydatasizelength=4",";mode=AAC-lbr"})
    assert(!selectAac(media+format+change,tr));
  for(auto cfg:{"1211","zzzz","1190","1208","2b92","121000"})
    assert(!selectAac(media+format.substr(0,format.size()-4)+cfg,tr));
  assert(!selectAac("m=audio 0 RTP/AVP 98\r\n"+media.substr(media.find("a=rtpmap"))+format,tr));
  ChannelPair p;assert(parseChannels("RTP/AVP/TCP;interleaved=10-11;unicast",p) && p.rtp==10 && p.rtcp==11);
  for (auto t:{"RTP/AVP;interleaved=0-1","RTP/AVP/TCP;interleaved=2-2","RTP/AVP/TCP;interleaved=0-256","RTP/AVP/TCP;interleaved=1-2;interleaved=2-3"}) assert(!parseChannels(t,p));
  RtpAac dep; Bytes frames;
  auto valid=packet({0,16,0,24,0x11,0x22,0x33});
  assert(dep.packet(valid,tr,frames) && frames==adts); assert(!dep.packet(valid,tr,frames));
  auto ext=packet({0,32,0,8,0,16,9,8,7},2); ext[0]=0xb1;
  ext.insert(ext.begin()+12,{0,0,0,0,0xab,0xcd,0,1,1,2,3,4}); ext.insert(ext.end(),{0,2});
  assert(dep.packet(ext,tr,frames)); ipcam::AdtsInfo info{};
  assert(ipcam::ParseAdts(frames.data(),frames.size(),&info) && info.frameLen==8 && frames[7]==9);
  assert(ipcam::ParseAdts(frames.data()+8,frames.size()-8,&info) && info.frameLen==9 && frames.back()==7);
  assert(!dep.packet(packet({0,16,0,24,0x11},3,false),tr,frames));
  assert(dep.packet(packet({0,16,0,24,0x22,0x33},4),tr,frames) && frames==adts);
  assert(!dep.packet(packet({0,16,0,24,0x11},5,false),tr,frames));
  assert(!dep.packet(packet({0,16,0,24,0x22,0x33},7),tr,frames));
  for (Bytes bad:{Bytes{0,0,0,1},Bytes{0,15,0,8,1},Bytes{0,16,0,0,1},Bytes{0,16,0,9,1},Bytes{0,32,0,8,0,16,1,2},Bytes{0,16,0xff,0xf8,1}})
    assert(!dep.packet(packet(bad,8),tr,frames) && frames.empty());
  for (int i:{0,1,11}) { auto b=valid; b[3]=9;b[i]^=1;assert(!dep.packet(b,tr,frames)); }
  auto b=valid;b[3]=9;b[0]|=32;b.back()=255;assert(!dep.packet(b,tr,frames));
}

// Even the repository RTSP server's bind(INADDR_ANY) is loopback-only here.
static int port=0;
extern "C" int __real_bind(int,const sockaddr*,socklen_t);
extern "C" int __wrap_bind(int fd,const sockaddr* addr,socklen_t len) {
  if (addr->sa_family == AF_INET6) { errno = EAFNOSUPPORT; return -1; }
  assert(addr->sa_family==AF_INET); auto a=*reinterpret_cast<const sockaddr_in*>(addr);
  a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  int rc=__real_bind(fd,reinterpret_cast<sockaddr*>(&a),len);if(rc) return rc;
  assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&len)==0);port=ntohs(a.sin_port);return 0;
}
static void localServer(bool advertised,bool offer) {
  feeds=0; renders=0; stops=0; failOutput=false;
  ipcam::RtspServer server; ipcam::RtspConfig cfg;cfg.port=0;cfg.user="viewer";cfg.password="test";
  assert(server.Start(cfg,nullptr));
  if(advertised)server.PushAdts(adts.data(),adts.size(),0);
  Session s;std::string url="rtsp://127.0.0.1:"+std::to_string(port)+"/live";
  std::thread client([&]{try{run<VideoSink>(s,url,"dmlld2VyOnRlc3Q=",1,offer);}catch(Status){assert(s.stopped);}});
  int tick=0;
  auto until=[&](auto done){auto end=Clock::now()+3s;while(!done()){
    assert(Clock::now()<end);++tick;
    server.PushH264(videoFrame.data(),videoFrame.size(),tick*24000);
    if(advertised)server.PushAdts(adts.data(),adts.size(),tick*24000);
    std::this_thread::sleep_for(10ms);
  }};
  until([&]{return renders>=3;}); assert(!feeds);
  if (offer && advertised) {
    assert(s.audio.state==Muted);s.audio.enable(true);until([&]{return s.audio.state==Feeding;});
    s.audio.enable(false); until([&]{return stops>=1;}); auto count=feeds.load();int r=renders;
    until([&]{return renders>=r+3;});assert(feeds==count && s.audio.state==Muted);
    failOutput=true;s.audio.enable(true);until([&]{return s.audio.state==OutputFailed;});
    r=renders;until([&]{return renders>=r+3;});
  } else {s.audio.enable(true);assert(s.audio.state==NoTrack && !feeds);}
  s.stop();client.join();server.Stop();
}
// Exercise replies not produced by our server: remapped channels (including RTCP
// collisions), a rejected SETUP with a body, and an unsafe audio control URL.
enum Case { Remap, Rejected, LostSession, Collision, WrongSession, Unsafe };
static void negotiation(Case which) {
  int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0 && listen(listener,2)==0);
  auto url="rtsp://127.0.0.1:"+std::to_string(port)+"/live";
  feeds=0;renders=0;failOutput=false;Session s;
  std::thread client([&]{try {run<VideoSink>(s,url,"",1,true);}catch(Status st){assert(st==Network && renders==1);}});
  const bool retry=which==Collision || which==WrongSession || which==LostSession;
  for(int pass=0;pass<=(retry?1:0);++pass) {
    pollfd ready{listener,POLLIN,0};assert(poll(&ready,1,3000)==1);
    int fd=accept(listener,nullptr,nullptr);assert(fd>=0);
    timeval timeout{3,0};assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    int cseq=0;
    auto req=[&](const std::string& method,const std::string& suffix,bool session){
      std::string text;char c;
      while(text.size()<8192 && (text.size()<4 || text.substr(text.size()-4)!="\r\n\r\n")) {assert(recv(fd,&c,1,0)==1);text+=c;}
      assert(text.rfind(method+" "+url+suffix+" RTSP/1.0\r\n",0)==0);
      assert(text.find("\r\nCSeq: "+std::to_string(++cseq)+"\r\n")!=text.npos);
      assert((text.find("\r\nSession: inner\r\n")!=text.npos)==session);
      return text;
    };
    auto reply=[&](const std::string& code,const std::string& headers,const std::string& body=""){
      std::string text="RTSP/1.0 "+code+"\r\nCSeq: "+std::to_string(cseq)+"\r\n"+headers+"Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
      assert(send(fd,text.data(),text.size(),MSG_NOSIGNAL)==ssize_t(text.size()));
    };
    std::string audio=media+format+"\r\n";
    if(which==Unsafe)audio.replace(audio.find("control:audio"),13,"control:rtsp://127.0.0.1:1/other");
    req("DESCRIBE","",false);reply("200 OK","",audio+"m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:vid\r\n");
    auto v=req("SETUP","/vid",false);assert(v.find("interleaved=0-1\r\n")!=v.npos);
    reply("200 OK","Session: inner;timeout=60\r\nTransport: RTP/AVP/TCP;interleaved=2-3\r\n");
    if(!pass && which!=Unsafe) {
      auto a=req("SETUP","/audio",true);assert(a.find("interleaved=4-5\r\n")!=a.npos);
      if(which==Rejected || which==LostSession) reply("461 Unsupported Transport","","not supported");
      else reply("200 OK",std::string("Session: ")+(which==WrongSession?"wrong":"inner")+"\r\nTransport: RTP/AVP/TCP;interleaved="+(which==Collision?"10-3":"10-11")+";unicast\r\n");
      if(retry){
        if(which==LostSession){req("PLAY","",true);reply("454 Session Not Found","");}
        char c;assert(recv(fd,&c,1,0)==0);close(fd);continue;
      }
    }
    req("PLAY","",true);reply("200 OK","Session: inner\r\n");
    auto interleave=[&](int channel,Bytes b){b.insert(b.begin(),{'$',uint8_t(channel),0,uint8_t(b.size())});assert(send(fd,b.data(),b.size(),MSG_NOSIGNAL)==ssize_t(b.size()));};
    if(which==Remap){
      auto end=Clock::now()+2s;while(s.audio.state!=Muted && Clock::now()<end)std::this_thread::yield();
      assert(s.audio.state==Muted);s.audio.enable(true);
      auto b=packet({0,16,0,24,0x11,0x22,0x33});interleave(11,b);interleave(4,b);interleave(10,b);
      while(s.audio.state!=Feeding && Clock::now()<end)std::this_thread::yield();assert(s.audio.state==Feeding && feeds==1);
    } else assert(s.audio.state==NoTrack);
    auto b=packet({0x65,0x88});b[1]=0xe0;interleave(2,b);close(fd);
  }
  client.join();close(listener);assert(renders==1);
}
int main(){ units();localServer(true,true);localServer(false,true);localServer(true,false);
  for(auto c:{Remap,Rejected,LostSession,Collision,WrongSession,Unsafe})negotiation(c);
  std::cout<<"PASS: validated SDP/ASC, RFC3640 AUs/fragments, actual loopback server A/V, mute/output failure, no audio and legacy video\n";
}
