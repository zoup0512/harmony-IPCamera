// Standalone, offline C++17 host test (no sockets, SDK or codec dependency).
#include "../entry/src/main/cpp/rtsp_client_hevc.h"
#include <cstdlib>
#include <iostream>
using namespace liveavc;
#define CHECK(x) do { if (!(x)) { std::cerr<<"line "<<__LINE__<<": " #x "\n"; std::abort(); } } while (false)

// Ordinary black frames encoded offline with libx265, yuv420p, Main profile.
const std::string vps64="QAEMAf//AWAAAAMAkAAAAwAAAwAelZgJ";
const std::string sps64="QgEBAWAAAAMAkAAAAwAAAwAeoBAgYWWVmkkyvAWgIAAAAwAgAAADAyE=";
const std::string pps64="RAHBcrQiQA==";
const Bytes idr=unbase64("KAGvHYD3AVv/8k7+xwAf2AEK"), vps=unbase64(vps64), sps=unbase64(sps64), pps=unbase64(pps64);
const std::string fmt="sprop-pps="+pps64+";sprop-vps="+vps64+";sprop-sps="+sps64;
std::string media(const std::string& f, const std::string& map="H265/90000") {
  return "m=video 0 RTP/AVP 96\r\na=rtpmap:96 "+map+"\r\na=control:trackID=0\r\na=fmtp:96 "+f+"\r\n";
}
Bytes annex(std::initializer_list<Bytes> nals) {
  Bytes a; for (const auto& n:nals) { a.insert(a.end(),{0,0,0,1}); a.insert(a.end(),n.begin(),n.end()); } return a;
}
void sdpTests() {
  Track tr; auto prefix="v=0\r\na=control:*\r\nm=audio 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\na=control:audio\r\n";
  CHECK(selectHevc(prefix+media(fmt),tr)); CHECK(tr.pt==96 && tr.aggregate=="*" && tr.control=="trackID=0");
  CHECK(tr.config==annex({vps,sps,pps}));
  CHECK(controlUrl("rtsp://cam/live","",tr.control)=="rtsp://cam/live/trackID=0");
  CHECK(selectHevc(media("sprop-max-don-diff=0;tx-mode=SRST","h265/90000"),tr) && tr.config.empty());
  CHECK(selectHevc(media(""),tr)); CHECK(!selectHevc(media(fmt,"H265/8000"),tr));
  CHECK(!selectHevc("m=video 0 RTP/AVP 97\na=control:v\na=rtpmap:96 H265/90000\n",tr));
  CHECK(!selectHevc("m=video 0 RTP/AVP 96\na=rtpmap:96 H265/90000\nm=video 0 RTP/AVP 96\na=control:v\n",tr));
  CHECK(!selectHevc(media("sprop-sei=AQ=="),tr));
  auto ambiguous=media("sprop-max-don-diff=1")+"a=fmtp:96 "+fmt+"\r\n";
  CHECK(!selectHevc(ambiguous,tr)); CHECK(selectHevc(ambiguous+media(fmt),tr));
  CHECK(!selectHevc(media(fmt,"H264/90000")+"a=rtpmap:96 H265/90000\r\n",tr));
  CHECK(selectHevc(media("sprop-sps=QgEBAWAAAAMAkAAAAwAAAwB4oAPAgBDlllZpJMrwFoCAAAADAIAAAA8wTks3AAehIAD0JkA="),tr)); // VUI HRD
  for (std::string s:{"sprop-max-don-diff=1","tx-mode=MRST","sprop-depack-buf-nalus=4","sprop-depack-buf-bytes=42",
                      "profile-id=2","sprop-vps=","sprop-pps=RAHBcrQiQA==extra","sprop-pps=RAHBcrQiQB==",
                      "sprop-pps=RAHBcrQiQA=","sprop-pps=RAHBcrQiQ!","sprop-pps=RAHBcrQiQA==,","sprop-pps=RAHBcrQiQA==;sprop-pps=RAHBcrQiQA=="})
    CHECK(!selectHevc(media(s),tr));
  CHECK(!selectHevc(media("sprop-sps="+vps64),tr));
  CHECK(!selectHevc(media("sprop-sps="+std::string(6000,'A')),tr));
  CHECK(selectHevc(media("tx-mode=MRMT")+media(fmt),tr) && tr.config==annex({vps,sps,pps}));
  CHECK(selectHevc(media("sprop-vps=QAEMAf//AWAAAAMAkAAAAwAAAwB4lZgJ;sprop-sps=QgEBAWAAAAMAkAAAAwAAAwB4oAPAgBDlllZpJMrwFoCAAAADAIAAAA8E;sprop-pps="+pps64),tr));
}

struct Bits {
  Bytes b; unsigned pos=0;
  void put(unsigned val,unsigned n) { while (n) { if (!(pos%8)) b.push_back(0); b.back()|=((val>>--n)&1)<<(7-(pos++%8)); } }
  void ue(unsigned v) { unsigned n=0; for (unsigned x=v+1; x>1; x>>=1) ++n; put(0,n); put(v+1,n+1); }
  Bytes finish(unsigned t) { put(1,1); while (pos%8) put(0,1); Bytes n={uint8_t(t*2),1}; unsigned z=0;
    for (auto c:b) { if (z==2 && c<=3) { n.push_back(3); z=0; } n.push_back(c); z=c ? 0 : z+1; } return n; }
};
struct Shape { unsigned w=1920,h=1088,chroma=1,lumaDepth=0,chromaDepth=0,dpb=6,reorder=0,crop=4,lowerDpb=0; bool extension=false; };
Bytes sampleSps(Shape a) {
  Bits b; b.put(0,4); b.put(a.lowerDpb ? 1 : 0,3); b.put(1,1); b.put(1,8); b.put(0x60000000,32); b.put(0x90000000,32); b.put(0,16); b.put(120,8);
  if (a.lowerDpb) b.put(0,16); // no sublayer profile/level overrides
  b.ue(0); b.ue(a.chroma); b.ue(a.w); b.ue(a.h); b.put(1,1); b.ue(0); b.ue(0); b.ue(0); b.ue(a.crop);
  b.ue(a.lumaDepth); b.ue(a.chromaDepth); b.ue(4); b.put(a.lowerDpb!=0,1);
  if (a.lowerDpb) { b.ue(a.lowerDpb-1); b.ue(0); b.ue(0); }
  b.ue(a.dpb-1); b.ue(a.reorder); b.ue(0);
  b.ue(0); b.ue(3); b.ue(0); b.ue(3); b.ue(0); b.ue(0); b.put(0,4); b.ue(0); b.put(0,4); b.put(a.extension,1);
  return b.finish(33);
}
void admissionTests() {
  HevcBudget budget;
  CHECK(budget.inspect(annex({idr}),false)==Admission::WaitForSps);
  CHECK(budget.inspect(annex({sps,pps}),true)==Admission::Ready);
  CHECK(budget.inspect(annex({idr}),false)==Admission::WaitForSps);
  CHECK(budget.inspect(annex({vps,idr}),false)==Admission::Ready);
  CHECK(budget.inspect(annex({idr}),false)==Admission::Ready);
  CHECK(budget.inspect(annex({idr}),true)==Admission::Unsupported);
  for (Bytes n:{vps,sps,pps}) {
    n.resize(n.size()/2); CHECK(budget.inspect(annex({n}),true)==Admission::Unsupported);
  }
  Shape good; CHECK(budget.inspect(annex({sampleSps(good)}),true)==Admission::Ready);
  auto reject=[&](Shape a){CHECK(budget.inspect(annex({sampleSps(a)}),true)==Admission::Unsupported);};
  Shape a=good; a.w=1922; reject(a); a=good; a.h=1090; reject(a); // cropped display would still fit
  a=good; a.w=0; reject(a); a=good; a.crop=544; reject(a);
  a=good; a.lowerDpb=3; CHECK(budget.inspect(annex({sampleSps(a)}),true)==Admission::Ready);
  a.lowerDpb=7; reject(a);
  a=good; a.dpb=7; reject(a); a=good; a.reorder=6; reject(a); a=good; a.chroma=3; reject(a);
  a=good; a.lumaDepth=2; reject(a); a=good; a.chromaDepth=2; reject(a); a=good; a.extension=true; reject(a);
  CHECK(budget.inspect(annex({{82,1,128}}),false)==Admission::Unsupported);
  Bytes longSet=sps; longSet.resize(4097,1); CHECK(budget.inspect(annex({longSet}),false)==Admission::Unsupported);
  CHECK(budget.inspect(Bytes(hevc::maxAu+1,1),false)==Admission::Unsupported);
  CHECK(budget.inspect(Bytes(hevc::maxConfig+1,1),true)==Admission::Unsupported);
  HevcBudget fresh; CHECK(fresh.inspect(annex({vps,sps,pps,{82,1,128}}),false)==Admission::Unsupported);
  CHECK(fresh.inspect(annex({idr}),false)==Admission::WaitForSps); // rejected AU installed no parameter sets
}
Bytes rtp(unsigned seq,unsigned ts,bool marker,const Bytes& body,unsigned source=1) {
  Bytes b={0x80,uint8_t(96|(marker?128:0)),uint8_t(seq>>8),uint8_t(seq),uint8_t(ts>>24),uint8_t(ts>>16),uint8_t(ts>>8),uint8_t(ts),0,0,0,uint8_t(source)};
  b.insert(b.end(),body.begin(),body.end()); return b;
}
Bytes ap(std::initializer_list<Bytes> nals) {
  Bytes b={96,1}; for (const auto& n:nals) { b.push_back(uint8_t(n.size()>>8)); b.push_back(uint8_t(n.size())); b.insert(b.end(),n.begin(),n.end()); } return b;
}
// Same two-byte indicator, original type and S/E layout as Session::SendVideoFrame.
Bytes fu(const Bytes& n,size_t from,size_t until) {
  Bytes b={uint8_t((n[0]&0x81)|98),n[1],uint8_t(hevc::type(n.data())|(from==2 ? 128 : 0)|(until==n.size() ? 64 : 0))};
  b.insert(b.end(),n.begin()+from,n.begin()+until); return b;
}
void packetTests() {
  RtpHevc r; Bytes f; uint32_t ts=0;
  auto push=[&](unsigned seq,unsigned stamp,bool marker,const Bytes& b,unsigned ssrc=1){return r.packet(rtp(seq,stamp,marker,b,ssrc),96,f,ts);};
  CHECK(!push(65532,99,false,{100,1,128},9)); CHECK(!push(65533,99,true,idr,9));
  CHECK(!push(65534,100,false,ap({vps,sps,pps}))); // first malformed packet did not bind SSRC 9
  CHECK(!push(65535,100,false,fu(idr,2,5))); CHECK(!push(0,100,false,fu(idr,5,10),2)); // foreign packet never changes FU state
  CHECK(!push(0,100,false,fu(idr,5,10))); CHECK(!push(0,100,false,fu(idr,5,10))); // duplicate
  CHECK(push(1,100,true,fu(idr,10,idr.size()))); CHECK(f==annex({vps,sps,pps,idr}) && ts==100);
  CHECK(!push(2,100,true,idr)); // only one marker per timestamp
  CHECK(push(3,101,true,idr) && f==annex({idr})); CHECK(!push(4,102,false,fu(idr,2,5)));
  CHECK(!push(6,102,true,fu(idr,10,idr.size())) && f.empty()); // gap suppresses all output for that timestamp
  CHECK(!push(7,102,true,idr)); CHECK(push(8,103,true,idr));
  CHECK(!push(9,104,false,fu(idr,2,5))); CHECK(push(10,105,true,idr) && f==annex({idr})); // abandon old timestamp
  unsigned seq=11;
  for (Bytes bad:{ap({idr}),ap({idr,{98,1,128,2}}),Bytes{96,1,0,12,38,1},Bytes{100,1,128},Bytes{38,0,128},
                  Bytes{38,9,128},Bytes{166,1,128},Bytes{98,1,0xd4,128},Bytes{98,1,0x54,128},Bytes{98,1,0x94},
                  Bytes{38,1,128,0,0,1,2},Bytes{38,1,128,0,0,3,4}}) {
    CHECK(!push(seq,200+seq,true,bad)); ++seq;
  }
  CHECK(!push(seq++,320,false,fu(idr,2,5))); Bytes wrong=fu(idr,5,idr.size()); wrong[1]=2;
  CHECK(!push(seq++,320,true,wrong)); CHECK(push(seq++,321,true,idr));
  CHECK(!push(seq++,322,true,fu(idr,2,5))); CHECK(!push(seq++,322,true,idr)); // premature marker
  auto bad=rtp(seq++,323,true,idr); bad[0]=0xb0; bad.back()=0; CHECK(!r.packet(bad,96,f,ts));
  auto ext=rtp(seq++,324,true,idr); ext[0]=0xb1; ext.insert(ext.begin()+12,{0,0,0,7, 0,1,0,1, 0,0,0,0}); ext.insert(ext.end(),{0,2});
  CHECK(r.packet(ext,96,f,ts) && f==annex({idr}));
  auto badExt=rtp(seq++,325,true,idr); badExt[0]=0x90; badExt[14]=255; CHECK(!r.packet(badExt,96,f,ts));
  auto wrongPt=rtp(seq,326,true,idr); wrongPt[1]=97|128; CHECK(!r.packet(wrongPt,96,f,ts));
  CHECK(push(seq,326,true,idr));
}
void sizeTests() {
  // The decoder gate inspects what FU/AP actually reconstructed, not just SDP.
  RtpHevc r; HevcBudget budget; Bytes result; uint32_t time=0;
  CHECK(r.packet(rtp(1,1,true,ap({vps,sps,pps,idr})),96,result,time));
  CHECK(budget.inspect(result,false)==Admission::Ready);
  Shape beyond; beyond.h=1096; beyond.crop=8; auto nal=sampleSps(beyond);
  CHECK(!r.packet(rtp(2,2,false,fu(nal,2,9)),96,result,time));
  CHECK(r.packet(rtp(3,2,true,fu(nal,9,nal.size())),96,result,time));
  CHECK(budget.inspect(result,false)==Admission::Unsupported);
  CHECK(r.packet(rtp(4,3,true,ap({vps,nal,pps,idr})),96,result,time));
  CHECK(budget.inspect(result,false)==Admission::Unsupported);
  for (unsigned excess:{0u,1u}) {
    Bytes large(hevc::maxAu-4+excess,0x55); large[0]=38; large[1]=1; large[2]=0xe0;
    RtpHevc r; unsigned seq=0; uint32_t ts=0; Bytes f;
    for (size_t i=2; i<large.size();) {
      size_t j=std::min(i+60000,large.size()); bool last=j==large.size();
      CHECK(r.packet(rtp(seq++,42,last,fu(large,i,j)),96,f,ts)==(last && !excess)); i=j;
    }
    if (!excess) CHECK(f==annex({large})); else CHECK(f.empty());
    CHECK(r.packet(rtp(seq++,43,true,idr),96,f,ts));
  }
}
int main() { sdpTests(); admissionTests(); packetTests(); sizeTests(); std::cout<<"HEVC SDP / admission / RFC 7798 host tests passed\n"; }
