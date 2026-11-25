// Host-only SDK doubles. Compile with m4a_voice_test.py. The FD, decoder and
// speaker are simulated. Optional fixtures are made from silence in a temp dir.
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "voice_player.h"
#include "m4a_voice.h"

namespace {
using Bytes = std::vector<uint8_t>;
Bytes body{0,0,0,20,'f','t','y','p','M','4','A',' ',0,0,0,0,'i','s','o','m'};
const Bytes lc{0x11,0x88}; // 48 kHz mono
const Bytes raw{0x01,0x40,0x22,0x80,0xa3,0x4e}; // deterministic test payload
Bytes adts(const Bytes& data) {
  size_t n = data.size()+7;
  Bytes bytes{0xff,0xf1,0x4c,uint8_t(0x40|(n>>11)),uint8_t(n>>3),uint8_t((n<<5)|31),0xfc};
  bytes.insert(bytes.end(), data.begin(), data.end()); return bytes;
}
struct Scenario {
  std::string fail, mime="audio/mp4a-latm";
  int tracks=1, type=0, rate=48000, channels=1, frames=150, sizeOverride=0, offset=3, flags=2;
  int64_t duration=3200000, ptsOverride=0;
  Bytes asc=lc, payload=raw;
  std::deque<Bytes> units;
} s;
int liveFd, lastFd=20, sources, formats, demuxers, buffers, reads, fdCreates;
Bytes written;
int memfd_create(const char*, unsigned flags) {
  assert(flags==1 && !liveFd); ++fdCreates;
  written.clear(); return s.fail=="fd" ? -1 : liveFd=++lastFd;
}
int write(int fd,const uint8_t* p,size_t n) {
  assert(fd==liveFd); if (s.fail=="write") return 0;
  static int interrupted; if (!(interrupted++%3)) {errno=EINTR;return -1;}
  n=std::min(n,size_t(5)); written.insert(written.end(),p,p+n);return int(n);
}
int lseek(int fd,int off,int whence){assert(fd==liveFd && !off && whence==SEEK_SET);return s.fail=="seek"?-1:0;}
int close(int fd){assert(fd==liveFd && !sources && !demuxers);liveFd=0;return 0;}
#define MFD_CLOEXEC 1
constexpr int AV_ERR_OK=0, MEDIA_TYPE_AUD=0;
constexpr unsigned AVCODEC_BUFFER_FLAGS_NONE=0, AVCODEC_BUFFER_FLAGS_SYNC_FRAME=2, AVCODEC_BUFFER_FLAGS_EOS=1;
constexpr const char *OH_MD_KEY_TRACK_TYPE="type", *OH_MD_KEY_TRACK_COUNT="tracks", *OH_MD_KEY_DURATION="duration",
 *OH_MD_KEY_CODEC_MIME="mime", *OH_AVCODEC_MIMETYPE_AUDIO_AAC="audio/mp4a-latm", *OH_MD_KEY_CODEC_CONFIG="asc",
 *OH_MD_KEY_AUD_SAMPLE_RATE="rate", *OH_MD_KEY_AUD_CHANNEL_COUNT="channels";
struct OH_AVSource {};struct OH_AVFormat {};struct OH_AVDemuxer {};
struct OH_AVBuffer {Bytes bytes;};
struct OH_AVCodecBufferAttr {int64_t pts=0;int size=0,offset=0;unsigned flags=0;};
OH_AVSource* OH_AVSource_CreateWithFD(int fd,int offset,int64_t size){
  assert(fd==liveFd && !offset && size==int64_t(body.size()) && written==body);
  if(s.fail=="source")return nullptr; ++sources;return new OH_AVSource;
}
void OH_AVSource_Destroy(OH_AVSource* p){assert(liveFd && !demuxers && !formats);--sources;delete p;}
OH_AVFormat* OH_AVFormat_Create(){++formats;return new OH_AVFormat;}
void OH_AVFormat_Destroy(OH_AVFormat* p){--formats;delete p;}
OH_AVFormat* OH_AVSource_GetSourceFormat(OH_AVSource*){return s.fail=="meta"?nullptr:OH_AVFormat_Create();}
OH_AVFormat* OH_AVSource_GetTrackFormat(OH_AVSource*,unsigned i){assert(!i);return s.fail=="track"?nullptr:OH_AVFormat_Create();}
bool OH_AVFormat_GetIntValue(OH_AVFormat*,const char* k,int* out){
  if(s.fail==k)return false;
  *out=k==OH_MD_KEY_TRACK_COUNT?s.tracks:k==OH_MD_KEY_TRACK_TYPE?s.type:k==OH_MD_KEY_AUD_SAMPLE_RATE?s.rate:s.channels;
  return true;
}
bool OH_AVFormat_GetLongValue(OH_AVFormat*,const char* k,int64_t* out){*out=s.duration;return s.fail!=k;}
bool OH_AVFormat_GetStringValue(OH_AVFormat*,const char* k,const char** out){*out=s.mime.c_str();return s.fail!=k;}
bool OH_AVFormat_GetBuffer(OH_AVFormat*,const char* k,uint8_t** p,size_t* n){*p=s.asc.data();*n=s.asc.size();return s.fail!=k;}
OH_AVDemuxer* OH_AVDemuxer_CreateWithSource(OH_AVSource*){if(s.fail=="demux")return nullptr;++demuxers;reads=0;return new OH_AVDemuxer;}
void OH_AVDemuxer_Destroy(OH_AVDemuxer* p){assert(sources && !buffers);--demuxers;delete p;}
int OH_AVDemuxer_SelectTrackByID(OH_AVDemuxer*,unsigned i){assert(!i);return s.fail=="select"?-1:0;}
OH_AVBuffer* OH_AVBuffer_Create(int cap){if(s.fail=="buffer")return nullptr;++buffers;return new OH_AVBuffer{Bytes(cap)};}
void OH_AVBuffer_Destroy(OH_AVBuffer* p){--buffers;delete p;}
int OH_AVDemuxer_ReadSampleBuffer(OH_AVDemuxer*,unsigned i,OH_AVBuffer* b){
  assert(!i && liveFd && ++reads<2000);
  if(!s.units.empty() && reads<=s.frames)s.payload=s.units.at(reads-1);
  if(s.offset>=0 && size_t(s.offset)+s.payload.size()<=b->bytes.size())
    std::copy(s.payload.begin(),s.payload.end(),b->bytes.begin()+s.offset);
  return s.fail=="late read" && reads==s.frames?-1:0;
}
int OH_AVBuffer_GetBufferAttr(OH_AVBuffer*,OH_AVCodecBufferAttr* a){
  bool eos=reads>s.frames; a->size=eos?0:s.sizeOverride?s.sizeOverride:int(s.payload.size());
  a->offset=s.offset;a->flags=eos?1:s.flags;a->pts=s.ptsOverride?s.ptsOverride:(reads-1)*21333;
  return s.fail=="attr"?-1:0;
}
int OH_AVBuffer_GetCapacity(OH_AVBuffer* b){return int(b->bytes.size());}
uint8_t* OH_AVBuffer_GetAddr(OH_AVBuffer* b){return s.fail=="addr"?nullptr:b->bytes.data();}

// Playback double drives real VoicePlayer callbacks reentrantly. Capture every
// submitted AAC unit, and deliver recognizable synthetic PCM for ring checks.
#define OH_LOG_Print(...) ((void)0)
constexpr const char* OH_MD_KEY_AUDIO_SAMPLE_FORMAT="sample-format";
constexpr int SAMPLE_S16LE=1;
constexpr int AUDIOSTREAM_SUCCESS=0,AUDIOSTREAM_ERROR_INVALID_PARAM=-1,AUDIOSTREAM_TYPE_RENDERER=1,
 AUDIOSTREAM_SAMPLE_S16LE=1,AUDIOSTREAM_ENCODING_TYPE_RAW=1,AUDIOSTREAM_USAGE_VOICE_COMMUNICATION=1;
struct OH_AVCodec;struct OH_AVMemory {uint8_t data[8192];};
struct OH_AVCodecAsyncCallback {
  void (*onError)(OH_AVCodec*,int,void*);void (*onStreamChanged)(OH_AVCodec*,OH_AVFormat*,void*);
  void (*onNeedInputData)(OH_AVCodec*,unsigned,OH_AVMemory*,void*);
  void (*onNeedOutputData)(OH_AVCodec*,unsigned,OH_AVMemory*,OH_AVCodecBufferAttr*,void*);
};
struct OH_AVCodec {OH_AVCodecAsyncCallback cb;void* ctx;OH_AVMemory mem;};
int decoderCount,builderCount,rendererCount,eosCount;
std::vector<Bytes> submitted;
OH_AVCodec* OH_AudioDecoder_CreateByMime(const char*){++decoderCount;return new OH_AVCodec;}
int OH_AudioDecoder_Destroy(OH_AVCodec* p){--decoderCount;delete p;return 0;}
int OH_AudioDecoder_SetCallback(OH_AVCodec* p,OH_AVCodecAsyncCallback cb,void* ctx){p->cb=cb;p->ctx=ctx;return 0;}
bool OH_AVFormat_SetStringValue(OH_AVFormat*,const char*,const char*){return true;}
bool OH_AVFormat_SetIntValue(OH_AVFormat*,const char*,int){return true;}
bool OH_AVFormat_SetBuffer(OH_AVFormat*,const char*,uint8_t* p,size_t n){assert(Bytes(p,p+n)==lc);return true;}
int OH_AudioDecoder_Configure(OH_AVCodec*,OH_AVFormat*){return s.fail=="configure"?-1:0;}
int OH_AudioDecoder_Prepare(OH_AVCodec*){return 0;}
int OH_AudioDecoder_Start(OH_AVCodec* p){p->cb.onNeedInputData(p,0,&p->mem,p->ctx);return s.fail=="start"?-1:0;}
int OH_AudioDecoder_Stop(OH_AVCodec*){return 0;}
int OH_AudioDecoder_FreeOutputData(OH_AVCodec*,unsigned){return 0;}
uint8_t* OH_AVMemory_GetAddr(OH_AVMemory* p){return p->data;}
int OH_AVMemory_GetSize(OH_AVMemory*){return 8192;}
int OH_AudioDecoder_PushInputData(OH_AVCodec* p,unsigned,OH_AVCodecBufferAttr a){
  if(s.fail=="push")return -1;
  assert(!a.offset);if(a.flags==1){assert(!a.size);++eosCount;return 0;}
  assert(a.size>0);submitted.emplace_back(p->mem.data,p->mem.data+a.size);
  a.size=2048;std::memset(p->mem.data,int(submitted.size()&255),2048);
  p->cb.onNeedOutputData(p,0,&p->mem,&a,p->ctx);p->cb.onNeedInputData(p,0,&p->mem,p->ctx);return 0;
}
struct OH_AudioRenderer{}; struct OH_AudioStreamBuilder{};
struct OH_AudioRenderer_Callbacks {int (*OH_AudioRenderer_OnWriteData)(OH_AudioRenderer*,void*,void*,int);};
OH_AudioRenderer_Callbacks output;void* outputCtx;
int OH_AudioStreamBuilder_Create(OH_AudioStreamBuilder** p,int){++builderCount;*p=new OH_AudioStreamBuilder;return 0;}
int OH_AudioStreamBuilder_Destroy(OH_AudioStreamBuilder* p){--builderCount;delete p;return 0;}
#define SETTER(name) int OH_AudioStreamBuilder_Set##name(OH_AudioStreamBuilder*,int){return 0;}
SETTER(SamplingRate) SETTER(ChannelCount) SETTER(SampleFormat) SETTER(EncodingType) SETTER(RendererInfo)
int OH_AudioStreamBuilder_SetRendererCallback(OH_AudioStreamBuilder*,OH_AudioRenderer_Callbacks cb,void* p){output=cb;outputCtx=p;return 0;}
int OH_AudioStreamBuilder_GenerateRenderer(OH_AudioStreamBuilder*,OH_AudioRenderer** p){++rendererCount;*p=new OH_AudioRenderer;return 0;}
int OH_AudioRenderer_Stop(OH_AudioRenderer*){return 0;}
int OH_AudioRenderer_Start(OH_AudioRenderer*){return s.fail=="speaker"?-1:0;}
int OH_AudioRenderer_Release(OH_AudioRenderer* p){--rendererCount;delete p;return 0;}
}
#include "m4a_voice.cpp"
#include "voice_player.cpp"

int main(int argc,char**){
  using namespace ipcam;
  VoiceClip clip;
  auto clean=[](){assert(!liveFd && !sources && !formats && !demuxers && !buffers);};
  auto check=[&](bool ok){assert((ParseVoiceUpload(body.data(),body.size(),clip)==VoiceClipResult::Ok)==ok);clean();};
  check(true);assert(clip.frames.size()==150 && clip.frames.front()==raw);
  s.payload=adts(raw);s.asc.insert(s.asc.end(),{0x56,0xe5,0});check(true);assert(clip.frames.front()==raw);
  auto saved=clip.frames;
  for(auto f:{"fd","write","seek","source","meta","tracks","duration","track","type","mime","rate","channels","asc","demux","select","buffer","late read","attr","addr"}){
    s=Scenario{};s.fail=f;check(false);assert(clip.frames==saved);
  }
  for(int i=0;i<16;i++){
    s=Scenario{};
    switch(i){case 0:s.tracks=2;break;case 1:s.type=1;break;case 2:s.mime="audio/opus";break;case 3:s.rate=44100;break;
    case 4:s.channels=3;break;case 5:s.asc={0x29,0x88};break;case 6:s.offset=-1;break;case 7:s.sizeOverride=8191;break;
    case 8:s.duration=30000001;break;case 9:s.frames=0;break;case 10:s.flags=4;break;case 11:s.ptsOverride=30000001;break;
    case 12:s.frames=1500;s.ptsOverride=1;break;case 13:s.payload.resize(8190);s.offset=0;break;
    case 14:s.payload={0xff,0xf1,0};break;case 15:s.sizeOverride=-1;break;}
    check(false);assert(clip.frames==saved);
  }
  s=Scenario{};int before=fdCreates;
  auto packet=adts(raw);assert(ParseVoiceUpload(packet.data(),packet.size(),clip)==VoiceClipResult::Ok);
  packet.push_back(0);assert(ParseVoiceUpload(packet.data(),packet.size(),clip)==VoiceClipResult::Invalid);
  assert(ParseVoiceUpload(body.data(),kVoiceUploadBytes+1,clip)==VoiceClipResult::Invalid && before==fdCreates);
  using F=VoicePlayer::FeedResult;
  VoicePlayer player;
  // >64 compressed frames AND >1 MiB PCM: no live-queue rejection or loss of clip start.
  s.frames=700;s.duration=15000000;assert(player.FeedUpload(body.data(),body.size())==F::Accepted && player.Active());
  assert(eosCount==1 && submitted==std::vector<Bytes>(700,raw));
  Bytes pcm(700*2048);output.OH_AudioRenderer_OnWriteData(nullptr,outputCtx,pcm.data(),int(pcm.size()));
  for(size_t i=0;i<pcm.size();++i)assert(pcm[i]==((i/2048+1)&255));
  s.fail="late read";assert(player.FeedUpload(body.data(),body.size())==F::Invalid && player.Active());
  s=Scenario{};packet=adts(raw);assert(player.FeedUpload(packet.data(),packet.size())==F::Accepted && eosCount==2);
  for(auto fail:{"configure","speaker","start","push"}){
    s.fail=fail;assert(player.FeedUpload(body.data(),body.size())==F::Unavailable && !player.Active());
    clean();assert(!decoderCount && !builderCount && !rendererCount);
  }
  s=Scenario{};assert(player.Feed(packet.data(),3)==F::Accepted);
  assert(player.Feed(packet.data()+3,packet.size()-3)==F::Accepted && eosCount==2);
  player.Stop();clean();assert(!decoderCount && !builderCount && !rendererCount);
  if(argc>1){
    // Exclusively the two files generated by the runner in its fresh temporary directory.
    auto load=[](const char* name){std::ifstream f(name,std::ios::binary);assert(f);return Bytes(std::istreambuf_iterator<char>(f),{});};
    body=load("synth.m4a");auto reference=load("synth.aac");
    assert(ParseVoiceUpload(reference.data(),reference.size(),clip)==VoiceClipResult::Ok);
    assert(clip.frames.size()>64 && clip.config.sfIndex==3 && clip.config.channels==1);
    s=Scenario{};s.units=clip.frames;s.frames=int(s.units.size());s.asc.insert(s.asc.end(),{0x56,0xe5,0});
    submitted.clear();assert(player.FeedUpload(body.data(),body.size())==F::Accepted);
    assert(std::equal(submitted.begin(),submitted.end(),s.units.begin(),s.units.end()));
    player.Stop();clean();std::cout<<"PASS: locally synthesized M4A, reference demux and identical decoder input ("<<s.frames<<" AAC units)\n";
  }
  std::cout<<"PASS: isolated fd and RAII failures, AAC tracks/ASC, bounds, EOS, atomic rejection, full PCM and streaming ADTS\n";
}
