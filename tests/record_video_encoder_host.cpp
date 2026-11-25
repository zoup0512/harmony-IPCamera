#include "record_video_encoder.h"
#include "record_video_encoder_mock.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <mutex>
#include <thread>

using Encoder=ipcam::RecordVideoEncoder;
using Pts=std::vector<int64_t>;
constexpr int64_t T=987654321000LL;
struct Capture { std::vector<Bytes> samples; Pts pts;
  void put(const uint8_t* p,size_t n,int64_t t){samples.emplace_back(p,p+n);pts.push_back(t);}
};
// Compile the actual ServerHolder::Push{Audio,Video} fan-out and RTMP clock
// against narrow sinks. A camera start/reconfigure call cannot link here.
namespace ipcam {
using TimestampUs=int64_t; constexpr int64_t kNoTimestampUs=-1;
inline int64_t NowMonotonicUs(){assert(false);return 0;}
struct RtmpPublisher {
  Capture v,a; std::mutex paramMu_; bool tsBaseSet_=false,lastVideoTsSet_=false,lastAudioTsSet_=false;
  int64_t tsBaseUs_=0,lastVideoTsUs_=0,lastAudioTsUs_=0;
  bool TimestampToMs(int64_t,bool,uint32_t*);
  void SendH264Packet(const uint8_t* p,size_t n,int64_t t){uint32_t ms;assert(TimestampToMs(t,true,&ms));v.put(p,n,ms);}
  void SendAdtsPacket(const uint8_t* p,size_t n,int64_t t){uint32_t ms;assert(TimestampToMs(t,false,&ms));a.put(p,n,ms);}
};
inline int64_t NowUs(){assert(false);return 0;}
#include "record_rtmp_clock.inc"
struct StreamRecorder {
  Capture v,a; void FeedVideo(const uint8_t* p,size_t n,int64_t t){v.put(p,n,t);}
  void FeedAudio(const uint8_t* p,size_t n,int64_t t){a.put(p,n,t);}
};
}
struct Passive {
  Capture video,audio;struct Params {bool ready=false,h265=false;Bytes vps,sps,pps;};Params GetVideoParams(){return {};}
  void PushH264(const uint8_t* p,size_t n,int64_t t){video.put(p,n,t);} void PushAdts(const uint8_t* p,size_t n,int64_t t){audio.put(p,n,t);}
  void FeedVideo(const uint8_t* p,size_t n,int64_t t){video.put(p,n,t);} void FeedAudio(const uint8_t* p,size_t n,int64_t t){audio.put(p,n,t);}
  void FeedVideo(const uint8_t*,size_t){} void SetParams(bool,Bytes&,int,int){}
};
struct Holder {
  Passive server; std::shared_ptr<Passive> flv=std::make_shared<Passive>(),webStream=std::make_shared<Passive>(),snapshot=std::make_shared<Passive>();
  struct Size{int width,height;}; std::atomic<Size> encodedSize{Size{64,64}};std::atomic<bool> webParamsInjected{true};
  std::mutex outputsMu;std::shared_ptr<ipcam::RtmpPublisher> rtmp=std::make_shared<ipcam::RtmpPublisher>();
  std::shared_ptr<ipcam::StreamRecorder> recorder=std::make_shared<ipcam::StreamRecorder>();std::shared_ptr<Encoder> recordEncoder;
#include "record_fanout.inc"
};

void test(bool hevc,mock::Failure fail,bool passthrough=false,bool invalid=false){
  mock::s={};mock::s.toHevc=hevc;mock::s.fail=fail;
  Holder h; const auto &s=mock::s.src(), &d=mock::s.dst();
  auto video=[&](const Bytes& b,int64_t t){h.PushVideo(b.data(),b.size(),T+t);};
  Bytes aac={0xff,0xf1,0x50,0x80,0x01,0x1f,0xfc,0xaa};
  auto audio=[&](int64_t t){h.PushAudio(aac.data(),aac.size(),T+t);};
  int configs=0;std::atomic<bool> failed{false};std::string failure,err;Bytes encoded;
  if(!passthrough){
    h.recordEncoder=std::make_shared<Encoder>();
    auto options=Encoder::Options{hevc,invalid?12000000:500000,2000};
    bool ok=h.recordEncoder->Configure(options,{!hevc,64,64,s.vps,s.sps,s.pps},{
      [&](const Encoder::Source& p){assert(p.h265==hevc && p.width==64 && p.height==64 && p.vps==d.vps && p.sps==d.sps && p.pps==d.pps && p.sps!=s.sps);++configs;
        auto add=[&](const Bytes& b){if(!b.empty()){encoded.insert(encoded.end(),{0,0,0,1});encoded.insert(encoded.end(),b.begin(),b.end());}};
        add(p.vps);add(p.sps);add(p.pps);},
      [&](const uint8_t* p,size_t n,int64_t t){assert(configs==1);if(h.recorder->v.pts.empty())encoded.insert(encoded.end(),p,p+n);h.recorder->FeedVideo(p,n,t);},
      [&](const uint8_t* p,size_t n,int64_t t){h.recorder->FeedAudio(p,n,t);},
      [&](const std::string& e){assert(!e.empty() && failure.empty());failure=e;failed.store(true);}},err);
    if(fail==mock::NoDecoder || fail==mock::Prepare || invalid){assert(!ok && !err.empty() && mock::s.creates==mock::s.destroys && !mock::s.windows);return;}
    assert(ok && err.empty());
  }
  // Start in the middle of a GOP. Original AAC/video reach RTMP even before
  // the private decoder sees its first IDR, and both keep one RTMP epoch.
  audio(0);video(s.delta,1000);audio(15000);audio(30100); // AAC arrived before either IDR callback.
  video(s.idr,10000);video(s.idr,30000);audio(31000);video(s.idr,50000);audio(65000);
  if(fail!=mock::None){
    // Dynamic failures finalize privately, without waiting for the user to stop.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!failed.load() && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    assert(failed.load() && mock::s.creates==mock::s.destroys && !mock::s.windows);
  }
  if(h.recordEncoder)h.recordEncoder->Stop();
  assert(h.server.video.samples==std::vector<Bytes>({s.delta,s.idr,s.idr,s.idr}));
  assert(h.flv->video.samples==h.server.video.samples && h.rtmp->v.samples==h.server.video.samples);
  assert(h.rtmp->a.samples==h.server.audio.samples && h.flv->audio.samples==h.server.audio.samples);
  assert(h.rtmp->a.pts==Pts({0,15,30,31,65}) && h.rtmp->v.pts==Pts({1,10,30,50}));
  if(passthrough){assert(!mock::s.creates && h.recorder->v.samples==h.server.video.samples && h.recorder->a.pts==h.server.audio.pts);return;}
  assert(mock::s.creates==mock::s.destroys && !mock::s.windows);
  if(fail!=mock::None){assert(!failure.empty() && h.recorder->v.samples.empty());return;}
  assert(failure.empty() && configs==1 && mock::s.inputs==3 && mock::s.renders==3);
  assert(h.recorder->v.samples==std::vector<Bytes>({d.idr,d.idr}) && h.recorder->v.pts==Pts({T+30000,T+50000}));
  assert(h.recorder->a.pts==Pts({T+30100,T+31000}) && h.recorder->a.samples==std::vector<Bytes>({aac,aac}));
  // Verify the muxer-facing bytes (actual parameter sink + first sample) with
  // FFmpeg in the parent process; no camera/file recording or SDK runtime.
  std::printf("%s ",hevc?"hevc":"h264");for(auto c:encoded)std::printf("%02x",unsigned(c));std::puts("");
}
int main(){
  test(true,mock::None,true);test(true,mock::None);test(false,mock::None);
  test(true,mock::None,false,true);
  for(auto fail:{mock::NoDecoder,mock::Prepare,mock::CorruptPts,mock::MissingConfig,mock::SizeChange,mock::BadOffset,mock::Runtime})test(true,fail);
}
