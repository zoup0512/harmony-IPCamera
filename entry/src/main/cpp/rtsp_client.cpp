#include "rtsp_client.h"
#include "rtsp_client_digest.h"
#include "rtsp_client_udp.h"
#include "rtsp_client_hevc.h"  // includes the shared AVC/RTSP protocol helpers
#include <type_traits>
#include "voice_player.h"
#include <condition_variable>
#include <deque>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <map>
#include <memory>
#include <thread>

namespace {
using namespace liveavc;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
enum Status { Connecting=0, Playing=1, Network=-1, Auth=-2, Codec=-3, Transport=-4, Protocol=-5, Decode=-6, Timeout=-7, Digest=-8 };
enum AudioState { OutputFailed=-2, NoTrack=-1, Muted=0, Listening=1, Feeding=2 };
struct Monitor {
  std::atomic<int> state{NoTrack};
  std::mutex mu;
  std::condition_variable wake;
  std::deque<Bytes> packets; // <= 8 TCP interleaved packets (each <= 65535 bytes)
  uint64_t epoch=0;
  bool closed=false;
  void enable(bool on) {
    std::lock_guard<std::mutex> lock(mu);
    if (closed || state<Muted || on==(state>Muted)) return;
    ++epoch; packets.clear(); state=on?Listening:Muted; wake.notify_one();
  }
  void close() {
    std::lock_guard<std::mutex> lock(mu);
    closed=true; packets.clear(); wake.notify_one();
  }
  void submit(Bytes& packet) {
    if (state<=Muted) return;
    std::lock_guard<std::mutex> lock(mu);
    if (closed || state<=Muted || packets.size()>=8) return;
    packets.emplace_back(); packets.back().swap(packet); wake.notify_one();
  }
};
enum class VideoCodec { Unknown, Avc, Hevc };
constexpr const char* mime(VideoCodec codec) {
  return codec==VideoCodec::Avc ? "video/avc" : codec==VideoCodec::Hevc ? "video/hevc" : "";
}
struct Session {
  std::atomic<bool> stopped{false}; std::atomic<int> status{Connecting};
  std::atomic<VideoCodec> videoCodec{VideoCodec::Unknown}; // negotiated, not a render signal
  Monitor audio;
  void stop() { stopped=true; audio.close(); }
};
// Only this worker owns the renderer. Decoder creation, backpressure and teardown
// never execute on the socket/AVC or JS thread. Mute wakes it even on a silent socket.
struct AudioWorker {
  Monitor& audio;
  std::thread thread;
  explicit AudioWorker(Monitor& a):audio(a) {}
  ~AudioWorker() { if(thread.joinable()){audio.close();thread.join();} }
  void start(AacTrack track) {
    try {
      std::lock_guard<std::mutex> lock(audio.mu);
      if (audio.closed) return;
      thread=std::thread([this,track]{
        try { playback(track); } catch (...) { audio.state=OutputFailed; }
      });
      audio.state=Muted;
    } catch (...) { audio.state=OutputFailed; }
  }
  void playback(const AacTrack& track) {
    ipcam::VoicePlayer player; RtpAac dep; Bytes packet,adts; uint64_t gen=0;
    for (;;) {
      std::unique_lock<std::mutex> lock(audio.mu);
      audio.wake.wait(lock,[&]{return audio.closed || gen!=audio.epoch || !audio.packets.empty();});
      if (audio.closed) return;
      if (gen!=audio.epoch) {
        gen=audio.epoch; lock.unlock(); player.Stop(); dep=RtpAac{}; continue;
      }
      packet.swap(audio.packets.front()); audio.packets.pop_front(); lock.unlock();
      if (!dep.packet(packet,track,adts)) continue;
      auto result=player.Feed(adts.data(),adts.size());
      lock.lock();
      if (gen!=audio.epoch || audio.closed) continue;
      using Result=ipcam::VoicePlayer::FeedResult;
      if (result==Result::Accepted) audio.state=Feeding;
      else if (result!=Result::Busy) { audio.state=OutputFailed; audio.packets.clear(); return; }
    }
  }
};
using Handle=std::shared_ptr<Session>;
std::atomic<int> workers{0};
std::atomic<int> resolvers{0};
struct WorkerSlot { std::atomic<int>& count; ~WorkerSlot() { --count; } };

using Addresses = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;
Addresses lookup(const std::string& host, const std::string& port, int flags) {
  addrinfo hints{}, *raw = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICSERV | flags;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &raw) != 0) raw = nullptr;
  return Addresses(raw, freeaddrinfo);
}

Addresses resolve(const Session& session, const std::string& host, const std::string& port,
                  Clock::time_point deadline) {
  if (session.stopped) throw Network;
  if (auto numeric = lookup(host, port, AI_NUMERICHOST)) return numeric;

  // The platform lookup cannot be interrupted. A late result owns no session, socket,
  // or JS resources and uses a separate, bounded slot until libc returns.
  std::packaged_task<Addresses()> task([host, port] {
    WorkerSlot slot{resolvers};
    return lookup(host, port, 0);
  });
  auto result = task.get_future();
  if (resolvers.fetch_add(1) >= 2) { --resolvers; throw Network; }
  try { std::thread(std::move(task)).detach(); }
  catch (...) { --resolvers; throw; }
  while (!session.stopped) {
    if (Clock::now() >= deadline) throw Timeout;
    if (result.wait_until(std::min(deadline, Clock::now() + 50ms)) == std::future_status::ready) {
      auto addresses = result.get();
      if (!addresses) throw Network;
      return addresses;
    }
  }
  throw Network;
}
struct Socket {
  const Session& session;
  int fd=-1;
  std::vector<uint8_t> pending; size_t offset=0;
  explicit Socket(const Session& s):session(s){}
  ~Socket(){if(fd>=0)close(fd);}
  void wait(short events, Clock::time_point deadline){
    while(!session.stopped){if(Clock::now()>=deadline)throw Timeout; pollfd p{fd,events,0};int rc=poll(&p,1,100);
      if(rc>0){if(p.revents&events)return;throw Network;} if(rc<0 && errno!=EINTR)throw Network;
    } throw Network;
  }
  void connectTo(const std::string& host,const std::string& port){
    auto until = Clock::now() + 6s;
    auto addrs = resolve(session, host, port, until);
    for(auto a=addrs.get();a && !session.stopped && Clock::now()<until;a=a->ai_next){
      fd=socket(a->ai_family,SOCK_STREAM,0);if(fd<0)continue;
      if(fcntl(fd,F_SETFL,O_NONBLOCK)==0){int rc=connect(fd,a->ai_addr,a->ai_addrlen);if(rc==0)return;
        if(errno==EINPROGRESS){try{wait(POLLOUT,until);int err=0;socklen_t len=sizeof(err);if(!getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len)&&!err)return;}catch(Status){} }
      } close(fd);fd=-1;
    } if (Clock::now() >= until) throw Timeout;
    throw Network;
  }
  void write(const std::string& text){size_t off=0;auto until=Clock::now()+5s;while(off<text.size()){
    wait(POLLOUT,until);auto n=send(fd,text.data()+off,text.size()-off,MSG_NOSIGNAL); if(n>0)off+=size_t(n);else if(n==0 || (errno!=EAGAIN && errno!=EINTR))throw Network;}}
  void read(uint8_t* out,size_t n,Clock::time_point until){while(n){if(offset==pending.size()){
    wait(POLLIN,until);pending.resize(8192);auto rc=recv(fd,pending.data(),pending.size(),0);offset=0;
    if(rc<=0){pending.clear();if(rc<0 && (errno==EINTR||errno==EAGAIN))continue;throw Network;}pending.resize(rc);
  }auto take=std::min(n,pending.size()-offset);std::memcpy(out,pending.data()+offset,take);out+=take;n-=take;offset+=take;}}
  uint8_t byte(Clock::time_point until){uint8_t b;read(&b,1,until);return b;}
};
struct Reply {std::map<std::string,std::string> h;std::string body;int code=0;};
template<class Inspect>
Reply response(Socket& sock,uint8_t first,int cseq,Clock::time_point end,Inspect inspect){
  std::string header(1,char(first));while(header.size()<32768){header+=char(sock.byte(end));if(header.size()>=4 && header.compare(header.size()-4,4,"\r\n\r\n")==0)break;}
  if(header.size()>=32768)throw Protocol;std::istringstream lines(header);std::string line;std::getline(lines,line);
  if(line.rfind("RTSP/1.0 ",0)!=0 || line.size()<13 || (line[12]!=' ' && line[12]!='\r'))throw Protocol;
  Reply r; r.code=number(line.substr(9,3),999);
  while(std::getline(lines,line) && line!="\r") {
    auto colon=line.find(':'); if(colon==0 || colon==line.npos || line.back()!='\r' ||
        !std::all_of(line.begin(),line.begin()+colon,rtspdigest::tchar)) throw Protocol;
    auto key=lower(line.substr(0,colon)),value=trim(line.substr(colon+1));
    if (key=="www-authenticate" && r.h.count(key)) r.h[key]+=","+value;
    else {
      if ((key=="cseq" || key=="content-length" || key=="transport" || key=="session") && r.h.count(key)) throw Protocol;
      r.h[key]=value;
    }
  }
  if(number(r.h["cseq"],2147483647)!=cseq)throw Protocol;
  inspect(r); // a definitive denial stays terminal even if its error body is truncated
  int n=0;if(r.h.count("content-length"))n=number(r.h["content-length"],65536);if(n<0)throw Protocol;
  r.body.resize(n);if(n)sock.read(reinterpret_cast<uint8_t*>(r.body.data()),n,end);return r;
}
template<VideoCodec codecType>
class Decoder {
  OH_AVCodec* codec=nullptr; OHNativeWindow* window=nullptr;
  std::conditional_t<codecType==VideoCodec::Avc, AvcBudget, HevcBudget> budget;
  bool checkedOutput=false;
  void checkOutputFormat() {
    std::unique_ptr<OH_AVFormat, decltype(&OH_AVFormat_Destroy)> format(
        OH_VideoDecoder_GetOutputDescription(codec), OH_AVFormat_Destroy);
    int32_t width = 0, height = 0;
    if (!format || !OH_AVFormat_GetIntValue(format.get(), OH_MD_KEY_VIDEO_PIC_WIDTH, &width) ||
        !OH_AVFormat_GetIntValue(format.get(), OH_MD_KEY_VIDEO_PIC_HEIGHT, &height) ||
        !fitsPicture(width, height)) throw Codec;
  }
public:
  ~Decoder(){if(codec)OH_VideoDecoder_Destroy(codec);if(window)OH_NativeWindow_DestroyNativeWindow(window);}
  void init(uint64_t surface){
    if(OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface,&window)!=0 || !window)throw Decode;
    codec=OH_VideoDecoder_CreateByMime(mime(codecType));if(!codec)throw Decode;
    // Preallocation hints only. Both formats separately validate the actual input.
    OH_AVFormat* fmt=OH_AVFormat_CreateVideoFormat(mime(codecType),kMaxWidth,kMaxHeight);if(!fmt)throw Decode;
    OH_AVFormat_SetIntValue(fmt,OH_MD_KEY_ENABLE_SYNC_MODE,1);
    OH_AVFormat_SetIntValue(fmt,OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY,1);
    auto rc=OH_VideoDecoder_Configure(codec,fmt); OH_AVFormat_Destroy(fmt);
    if(rc!=AV_ERR_OK || OH_VideoDecoder_SetSurface(codec,window)!=AV_ERR_OK || OH_VideoDecoder_Prepare(codec)!=AV_ERR_OK || OH_VideoDecoder_Start(codec)!=AV_ERR_OK)throw Decode;
  }
  void drain(Session& s,Clock::time_point& last){for(int i=0;i<32;++i){uint32_t index=0;auto rc=OH_VideoDecoder_QueryOutputBuffer(codec,&index,0);
    if(rc==AV_ERR_TRY_AGAIN_LATER)return;if(rc==AV_ERR_STREAM_CHANGED){checkOutputFormat();checkedOutput=true;continue;}
    if(rc!=AV_ERR_OK)throw Decode;
    if(!checkedOutput){checkOutputFormat();checkedOutput=true;}
    if(OH_VideoDecoder_RenderOutputBuffer(codec,index)!=AV_ERR_OK)throw Decode;
    s.status=Playing;last=Clock::now();}}
  void push(const Bytes& frame,int64_t pts,bool config,Session& s,Clock::time_point& last){
    if (s.stopped) throw Network;
    switch (budget.inspect(frame, config)) {
      case Admission::Unsupported: throw Codec;
      case Admission::WaitForSps: return;
      case Admission::Ready: break;
    }
    uint32_t index=0;auto until=Clock::now()+2s;while(!s.stopped){drain(s,last);auto rc=OH_VideoDecoder_QueryInputBuffer(codec,&index,10000);
      if(rc==AV_ERR_OK)break;if(rc!=AV_ERR_TRY_AGAIN_LATER || Clock::now()>until)throw Decode;}
    if(s.stopped)throw Network;auto buf=OH_VideoDecoder_GetInputBuffer(codec,index);auto cap=buf?OH_AVBuffer_GetCapacity(buf):0;auto addr=buf?OH_AVBuffer_GetAddr(buf):nullptr;
    if(!addr||cap<=0||frame.size()>size_t(cap))throw Decode;std::memcpy(addr,frame.data(),frame.size());
    OH_AVCodecBufferAttr attr{};attr.pts=pts;attr.size=static_cast<int32_t>(frame.size());attr.flags=config?AVCODEC_BUFFER_FLAGS_CODEC_DATA:AVCODEC_BUFFER_FLAGS_NONE;
    if(OH_AVBuffer_SetBufferAttr(buf,&attr)!=AV_ERR_OK || OH_VideoDecoder_PushInputBuffer(codec,index)!=AV_ERR_OK)throw Decode;drain(s,last);
  }
};
template<class AvcSink, class HevcSink>
bool runTracks(Session& s,const std::string& url,const std::string& auth,uint64_t surface,bool offerAudio,
               const rtspdigest::Credentials* credentials,bool udp){
  s.videoCodec=VideoCodec::Unknown;  // a video-only retry negotiates a fresh session
  auto o=origin(url);if(o.empty() || (!auth.empty() && auth.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")!=auth.npos))throw Protocol;
  std::string hp=o.substr(7),host,port="554";if(hp.front()=='['){auto pos=hp.find(']');if(pos==hp.npos)throw Protocol;host=hp.substr(1,pos-1);if(pos+1<hp.size()){if(hp[pos+1]!=':')throw Protocol;port=hp.substr(pos+2);}}
  else{auto pos=hp.find(':');host=hp.substr(0,pos);if(pos!=hp.npos)port=hp.substr(pos+1);}if(host.empty()||number(port,65535)<=0)throw Protocol;
  if (credentials && (!credentials->valid() || !auth.empty())) throw Auth;
  Socket sock(s);sock.connectTo(host,port);int seq=0;std::string session;
  rtspdigest::Client digest(o); // nonce and authorization never survive a connection
  auto sendRequest=[&](const std::string& method,const std::string& uri,const std::string& extra){
    if(origin(uri)!=o)throw Protocol;
    std::string authorization;
    if (digest.active()) {
      authorization=digest.authorization(method,uri);
      if (authorization.empty()) throw Digest;
    } else if (!auth.empty()) authorization="Basic "+auth;
    sock.write(method+" "+uri+" RTSP/1.0\r\nCSeq: "+std::to_string(++seq)+"\r\n"+
      (authorization.empty()?"":"Authorization: "+authorization+"\r\n")+
      (session.empty()?"":"Session: "+session+"\r\n")+extra+"\r\n");};
  // True means repeat this exact request once. A second 401 (even stale=true),
  // a 403, or a Basic-only challenge never triggers another credential attempt.
  auto challenged=[&](const Reply& r,const std::string& uri,bool optional){
    if (r.code==401 || r.code==403) {
      auto field=r.h.find("www-authenticate");
      if (r.code==403 || field==r.h.end()) throw Auth;
      if (!credentials) {if(lower(field->second).rfind("digest",0)==0)throw Digest;throw Auth;}
      switch (digest.onResponse(r.code,uri,field->second,*credentials)) {
        case rtspdigest::Action::Repeat: return true;
        case rtspdigest::Action::Unsupported: throw Digest;
        default: throw Auth;
      }
    }
    if (r.code>=200 && r.code<300) {
      auto sid=r.h.find("session");
      if(udp && !session.empty() && sid!=r.h.end() && trim(sid->second.substr(0,sid->second.find(';')))!=session)throw Protocol;
      return false;
    }
    if (optional && r.code>=400 && r.code<600) return false;
    if (r.code==461) throw Transport;
    throw Protocol;
  };
  auto exchange=[&](const std::string& method,const std::string& uri,const std::string& extra,bool optional=false){
    for (;;) {
      sendRequest(method,uri,extra);
      auto end=Clock::now()+(optional?3s:7s);
      bool repeat=false;
      auto r=response(sock,sock.byte(end),seq,end,[&](const Reply& headers){repeat=challenged(headers,uri,optional);});
      if (!repeat) return r;
    }
  };
  auto desc=exchange("DESCRIBE",url,"Accept: application/sdp\r\n");Track tr;
  const auto selected=selectAvc(desc.body,tr) ? VideoCodec::Avc :
      selectHevc(desc.body,tr) ? VideoCodec::Hevc : VideoCodec::Unknown;
  if(selected==VideoCodec::Unknown)throw Codec;
  s.videoCodec=selected;
  auto base=desc.h["content-base"], track=controlUrl(url,base,tr.control), agg=controlUrl(url,base,tr.aggregate);if(track.empty()||agg.empty())throw Protocol;
  rtspudp::Media media;
  if(udp && (tr.pt<96 || !media.open(sock.fd)))throw Transport; // dynamic video PT only, disjoint from RTCP
  auto transport=udp?media.offer():"RTP/AVP/TCP;unicast;interleaved=0-1";
  auto setup=exchange("SETUP",track,"Transport: "+transport+"\r\n");
  ChannelPair video;
  if(!(udp?media.accept(setup.h["transport"]):parseChannels(setup.h["transport"],video)))throw Transport;
  auto field=setup.h["session"];session=trim(field.substr(0,field.find(';')));if(!token(session))throw Protocol;
  int keepSec=20;auto tk=lower(field).find("timeout=");if(tk!=field.npos){auto value=field.substr(tk+8);int t=number(value.substr(0,value.find(';')),86400);if(t>0)keepSec=std::max(1,std::min(20,t/2));}
  AacTrack aac; ChannelPair audio; bool attemptedAudio=false;
  if (!udp && offerAudio && selectAac(desc.body,aac)) {
    auto uri=controlUrl(url,base,aac.control);
    if (uri.empty() && credentials) throw Protocol; // no recovery from an unsafe control URI
    if (!uri.empty() && uri!=track && uri!=agg) {
      attemptedAudio=true;
      int ch=2;
      auto overlaps=[&](int n){return n==video.rtp || n==video.rtcp;};
      while (overlaps(ch) || overlaps(ch+1)) ch+=2;
      try {
        auto ar=exchange("SETUP",uri,"Transport: RTP/AVP/TCP;unicast;interleaved="+std::to_string(ch)+"-"+std::to_string(ch+1)+"\r\n",true);
        if (ar.code<300) {
          auto sid=ar.h["session"];
          if (trim(sid.substr(0,sid.find(';')))!=session || !parseChannels(ar.h["transport"],audio) ||
              overlaps(audio.rtp) || overlaps(audio.rtcp)) return false;
        }
      } catch (Status st) {
        if (credentials || st==Auth || st==Digest) throw;
        return false; // legacy media-only recovery; never replay Digest credentials
      }
    }
  }
  auto play=[&](auto& dec,auto& dep){
    dec.init(surface);
    try { exchange("PLAY",agg,""); }
    catch (Status st) {
      // Some cameras invalidate the first SETUP when rejecting a second track.
      if (attemptedAudio && !credentials && st!=Auth && st!=Digest) return false;
      throw;
    }
    auto last=Clock::now(),keep=last;bool awaiting=false;
    AudioWorker listener(s.audio); if (audio.rtp>=0) listener.start(aac);
    if(!tr.config.empty())dec.push(tr.config,0,true,s,last);
    Bytes packet,frame;bool gotTime=false;uint32_t prev=0;int64_t ticks=0;
    while(!s.stopped){auto now=Clock::now();if(now-last>12s)throw Timeout;
      if(awaiting && udp && now-keep>=7s)throw Timeout;
      if(now-keep>=std::chrono::seconds(keepSec) && (!udp || !awaiting)){
        if(awaiting)throw Timeout;sendRequest("OPTIONS",agg,"");keep=now;awaiting=true;
      }
      auto ready=udp?media.next(sock.fd,sock.offset<sock.pending.size(),tr.pt,packet):rtspudp::Media::Control;
      if(ready==rtspudp::Media::Idle)continue;
      if(ready==rtspudp::Media::Error || ready==rtspudp::Media::End)throw Network;
      if(ready==rtspudp::Media::Control){
        auto end=udp?std::min(last+12s,(awaiting?keep:now)+7s):now+8s;
        uint8_t first=sock.byte(end);
        if(first!='$'){
          if(!awaiting)throw Protocol;
          response(sock,first,seq,end,[&](const Reply& headers){awaiting=challenged(headers,agg,false);});
          if(awaiting){sendRequest("OPTIONS",agg,"");keep=Clock::now();}
          continue;
        }
        if(udp)throw Transport; // never interpret interleaving as an implicit fallback
        uint8_t h[3];sock.read(h,3,end);packet.resize(h[1]*256+h[2]);sock.read(packet.data(),packet.size(),end);
        if(h[0]==audio.rtp){s.audio.submit(packet);continue;}
        if(h[0]!=video.rtp)continue;
      }
      uint32_t pts;if(!dep.packet(packet,tr.pt,frame,pts))continue;
      if(gotTime)ticks+=int32_t(pts-prev);prev=pts;gotTime=true;dec.push(frame,ticks*100/9,false,s,last);
    }
    return true;
  };
  if(selected==VideoCodec::Avc){AvcSink dec;RtpAvc dep;return play(dec,dep);}
  HevcSink dec;RtpHevc dep;return play(dec,dep);
}
template<class AvcSink, class HevcSink=AvcSink>
void run(Session& s,const std::string& url,const std::string& auth,uint64_t surface,bool offerAudio=false,
         const rtspdigest::Credentials* credentials=nullptr,bool udp=false) {
  if (!runTracks<AvcSink,HevcSink>(s,url,auth,surface,offerAudio,credentials,udp) && !s.stopped) {
    if (credentials || udp) throw Transport;
    runTracks<AvcSink,HevcSink>(s,url,auth,surface,false,nullptr,false);
  }
}
void run(Session& s,const std::string& url,const std::string& auth,uint64_t surface,bool offerAudio,
         const rtspdigest::Credentials* credentials=nullptr,bool udp=false) {
  run<Decoder<VideoCodec::Avc>,Decoder<VideoCodec::Hevc>>(s,url,auth,surface,offerAudio,credentials,udp);
}
std::string arg(napi_env e,napi_value v,size_t max){size_t n=0;if(napi_get_value_string_utf8(e,v,nullptr,0,&n)!=napi_ok || n>max)throw Protocol;
  std::string out(n+1,'\0');if(napi_get_value_string_utf8(e,v,out.data(),out.size(),&n)!=napi_ok)throw Protocol;out.resize(n);return out;}
// Other APIs in this module unwrap arbitrary native pointers. Keep playback handles
// as plain branded objects: the only value visible to JS is an immutable numeric key.
constexpr napi_type_tag kPlaybackTag{0x58ec2e3ee1ab476dULL, 0xb1f3cf4bb626e5a8ULL};
std::mutex handlesMu;
std::map<uint32_t, Handle> handles;
uint32_t nextHandle = 0;
struct PlaybackKey { uint32_t id; };
void finalize(napi_env, void* data, void*) {
  std::unique_ptr<PlaybackKey> key(static_cast<PlaybackKey*>(data));
  std::lock_guard<std::mutex> lock(handlesMu);
  auto it = handles.find(key->id);
  if (it != handles.end()) { it->second->stop(); handles.erase(it); }
}

napi_value makeHandle(napi_env env, const Handle& session) {
  napi_value object{}, value{};
  if (napi_create_object(env, &object) != napi_ok ||
      napi_type_tag_object(env, object, &kPlaybackTag) != napi_ok) throw Network;
  auto key = std::make_unique<PlaybackKey>();
  {
    std::lock_guard<std::mutex> lock(handlesMu);
    do { key->id = ++nextHandle; } while (handles.count(key->id));
    handles.emplace(key->id, session);
  }
  napi_property_descriptor id{};
  id.utf8name = "playbackId"; // napi_default: not writable, enumerable or configurable
  if (napi_create_uint32(env, key->id, &value) == napi_ok) id.value = value;
  if (!id.value || napi_define_properties(env, object, 1, &id) != napi_ok ||
      napi_add_finalizer(env, object, key.get(), finalize, nullptr, nullptr) != napi_ok) {
    std::lock_guard<std::mutex> lock(handlesMu);
    handles.erase(key->id);
    throw Network;
  }
  key.release();
  return object;
}
template<bool digestMode=false,bool udp=false>
napi_value start(napi_env e,napi_callback_info info){
  napi_value args[6]{},ret=nullptr;size_t n=6;
  try{
    constexpr size_t required=digestMode?4:3;
    bool offerAudio=false;
    if(napi_get_cb_info(e,info,&n,args,nullptr,nullptr)!=napi_ok || n<required || n>required+(udp?0:1) ||
       (n>required && napi_get_value_bool(e,args[required],&offerAudio)!=napi_ok))throw Protocol;
    auto url=arg(e,args[0],4096),surface=arg(e,args[required-1],32);
    std::string auth; rtspdigest::Credentials credentials;
    if constexpr (digestMode) {
      credentials={arg(e,args[1],4096),arg(e,args[2],4096)};
      if (!credentials.valid() || origin(url).empty()) throw Protocol;
    } else auth=arg(e,args[1],4096);
    uint64_t id=0;
    auto r=std::from_chars(surface.data(),surface.data()+surface.size(),id);if(r.ec!=std::errc() || r.ptr!=surface.data()+surface.size() || !id)throw Protocol;
    auto h = std::make_shared<Session>();
    ret = makeHandle(e, h);
    if(workers.fetch_add(1)>=4){--workers;h->status=Network;return ret;}
    try{std::thread([h,url,auth,id,offerAudio,credentials=std::move(credentials)](){WorkerSlot slot{workers};try{run(*h,url,auth,id,offerAudio,digestMode?&credentials:nullptr,udp);}catch(Status st){h->status=st;}catch(...){h->status=Network;}}).detach();}catch(...){--workers;h->status=Network;}
  }catch(...){napi_throw_error(e,nullptr,"Invalid playback request");return nullptr;}return ret;
}
Handle handle(napi_env env, napi_callback_info info, bool* enabled=nullptr) {
  napi_value args[2]{}, value{};
  size_t argc = enabled?2:1;
  bool tagged = false;
  uint32_t id = 0;
  napi_valuetype type{};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc != (enabled?2:1) ||
      (enabled && napi_get_value_bool(env,args[1],enabled)!=napi_ok) ||
      napi_typeof(env, args[0], &type) != napi_ok || type != napi_object ||
      napi_check_object_type_tag(env, args[0], &kPlaybackTag, &tagged) != napi_ok || !tagged ||
      napi_get_named_property(env, args[0], "playbackId", &value) != napi_ok ||
      napi_get_value_uint32(env, value, &id) != napi_ok) return {};
  std::lock_guard<std::mutex> lock(handlesMu);
  auto it = handles.find(id);
  return it == handles.end() ? Handle{} : it->second;
}
napi_value status(napi_env e,napi_callback_info i){auto h=handle(e,i);napi_value v;napi_create_int32(e,h?h->status.load():Protocol,&v);return v;}
napi_value videoCodec(napi_env e,napi_callback_info i){auto h=handle(e,i);napi_value v;
  napi_create_string_utf8(e,h&&!h->stopped?mime(h->videoCodec.load()):"",NAPI_AUTO_LENGTH,&v);return v;}
napi_value audioStatus(napi_env e,napi_callback_info i){auto h=handle(e,i);napi_value v;napi_create_int32(e,h&&!h->stopped?h->audio.state.load():NoTrack,&v);return v;}
napi_value setAudio(napi_env e,napi_callback_info i){bool on=false;auto h=handle(e,i,&on);if(h&&!h->stopped)h->audio.enable(on);napi_value v;napi_get_undefined(e,&v);return v;}
napi_value stop(napi_env e,napi_callback_info i){auto h=handle(e,i);if(h)h->stop();napi_value v;napi_get_undefined(e,&v);return v;}
} // namespace
void RegisterRtspClient(napi_env e,napi_value exports){
  napi_property_descriptor p[]={{"liveAvcStart",nullptr,start<false>,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"liveAvcStartUdp",nullptr,start<false,true>,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"liveAvcStartDigestUdp",nullptr,start<true,true>,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"liveAvcStartDigest",nullptr,start<true>,nullptr,nullptr,nullptr,napi_default,nullptr},{"liveAvcStatus",nullptr,status,nullptr,nullptr,nullptr,napi_default,nullptr},{"liveAvcStop",nullptr,stop,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"liveVideoCodec",nullptr,videoCodec,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"liveAvcAudioStatus",nullptr,audioStatus,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"liveAvcSetAudio",nullptr,setAudio,nullptr,nullptr,nullptr,napi_default,nullptr}};
  napi_define_properties(e,exports,sizeof(p)/sizeof(p[0]),p);
}
