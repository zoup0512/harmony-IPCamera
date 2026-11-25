#pragma once
// Single-layer HEVC Main, RFC 7798 single-stream transmission, no DONL/DOND.
// Create both stateful objects anew at SETUP. Inspect SDP config and every
// completed access unit before passing exactly those bytes to the decoder.
#include "rtsp_client_protocol.h"
#include <utility>

namespace liveavc {
namespace hevc {
constexpr size_t maxAu = 4 * 1024 * 1024, maxParam = 4096, maxConfig = 64 * 1024, maxNals = 1024;
constexpr unsigned maxPictures = 6; // at 1920x1088, <= 18 MiB of 8-bit 4:2:0 DPB samples
inline unsigned type(const uint8_t* p) { return (p[0] >> 1) & 63; }
inline bool header(const uint8_t* p, size_t n) {
  return n >= 2 && !(p[0] & 0x81) && p[1] >= 1 && p[1] <= 7; // F=0, layer_id=0, temporal_id_plus1!=0
}
inline bool nalType(unsigned t) { return t <= 9 || (t >= 16 && t <= 21) || (t >= 32 && t <= 40); }
inline bool ebsp(const uint8_t* p, size_t n, Bytes* rbsp = nullptr) {
  unsigned zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    if (zeros == 2 && p[i] <= 3) {
      if (p[i] != 3 || i+1 == n || p[i+1] > 3) return false;
      zeros = 0; continue;
    }
    if (rbsp) rbsp->push_back(p[i]);
    zeros = p[i] == 0 ? zeros+1 : 0;
  }
  return true;
}
inline bool profile(SpsBits& b) {
  unsigned space = b.read(2); b.read(1); unsigned id = b.read(5), compat = b.read(32);
  b.read(32); b.read(16); // source and constraint flags
  return b.ok && space == 0 && (id == 1 || (compat & (1u << 30))); // Main or Main compatible
}
inline bool ptl(SpsBits& b, unsigned layers) {
  if (layers > 6 || !profile(b)) return false;
  b.read(8); // level_idc is not a substitute for actual dimensions / DPB
  std::array<unsigned,7> prof{}, level{};
  for (unsigned i=0; i<layers; ++i) { prof[i]=b.read(1); level[i]=b.read(1); }
  if (layers && b.read((8-layers)*2) != 0) return false;
  for (unsigned i=0; i<layers; ++i) {
    if (prof[i] && !profile(b)) return false;
    if (level[i]) b.read(8);
  }
  return b.ok;
}
inline bool ordering(SpsBits& b, unsigned layers) {
  for (unsigned i=b.read(1) ? 0 : layers; i<=layers; ++i) {
    unsigned minus1=b.ue(), reorder=b.ue(); b.ue();
    if (minus1>=maxPictures || reorder>minus1) return false;
  }
  return b.ok;
}
inline bool hrd(SpsBits& b, unsigned layers) {
  bool nal=b.read(1)!=0, vcl=b.read(1)!=0, subPic=false;
  unsigned scale=0, duScale=0;
  if (nal || vcl) {
    subPic=b.read(1)!=0;
    if (subPic) b.read(19);
    b.read(4); scale=b.read(4); if (subPic) duScale=b.read(4); b.read(15);
  }
  auto fitsCpb=[&](unsigned shift){return (uint64_t(b.ue()+1)<<(4+shift)) <= maxAu*8;};
  for (unsigned i=0; i<=layers; ++i) {
    bool fixed=b.read(1) || b.read(1), lowDelay=false;
    if (fixed) b.ue(); else lowDelay=b.read(1)!=0;
    unsigned last=lowDelay ? 0 : b.ue(); if (last>31) return false;
    for (unsigned kind=0; kind<unsigned(nal)+unsigned(vcl); ++kind) {
      for (unsigned j=0; j<=last; ++j) {
        b.ue(); if (!fitsCpb(scale)) return false;
        if (subPic) { if (!fitsCpb(duScale)) return false; b.ue(); }
        b.read(1);
      }
    }
  }
  return b.ok;
}
inline bool vps(SpsBits& b, unsigned& id) {
  id=b.read(4); b.read(2);
  if (b.read(6)!=0) return false; // vps_max_layers_minus1
  unsigned layers=b.read(3); b.read(1);
  if (b.read(16)!=65535 || !ptl(b,layers) || !ordering(b,layers)) return false;
  if (b.read(6)!=0 || b.ue()!=0) return false; // no extra layers or layer sets
  if (b.read(1)) {
    b.read(32); b.read(32); if (b.read(1)) b.ue();
    unsigned sets=b.ue();
    if (sets>1 || (sets && (b.ue()!=0 || !hrd(b,layers)))) return false;
  }
  return !b.read(1) && b.endOfRbsp(); // no VPS extensions
}
inline bool scalingLists(SpsBits& b) {
  for (unsigned size=0; size<4; ++size) for (unsigned m=0; m<6; m+=(size==3 ? 3 : 1)) {
    if (!b.read(1)) { if (b.ue()>m) return false; }
    else {
      if (size>1 && b.ue()>510) return false;
      for (unsigned i=0; i<(size ? 64u : 16u); ++i) if (b.ue()>256) return false;
    }
  }
  return b.ok;
}
inline bool referenceSets(SpsBits& b, unsigned maxRefs) {
  unsigned count=b.ue(), previous=0;
  if (count>64) return false;
  for (unsigned i=0; i<count; ++i) {
    unsigned refs=0;
    if (i && b.read(1)) {
      b.read(1); b.ue();
      for (unsigned j=0; j<=previous; ++j) if (b.read(1) || b.read(1)) ++refs;
    } else {
      unsigned neg=b.ue(), pos=b.ue();
      if (neg>maxRefs || pos>maxRefs-neg) return false;
      refs=neg+pos;
      for (unsigned j=0; j<refs; ++j) { b.ue(); b.read(1); }
    }
    if (refs>maxRefs) return false;
    previous=refs;
  }
  return b.ok;
}
inline bool vui(SpsBits& b, unsigned layers) {
  if (b.read(1) && b.read(8)==255) b.read(32);
  if (b.read(1)) b.read(1);
  if (b.read(1)) { b.read(4); if (b.read(1)) b.read(24); }
  if (b.read(1)) { b.ue(); b.ue(); }
  b.read(3);
  if (b.read(1)) { b.ue(); b.ue(); b.ue(); b.ue(); }
  if (b.read(1)) {
    b.read(32); b.read(32); if (b.read(1)) b.ue();
    if (b.read(1) && !hrd(b,layers)) return false;
  }
  if (b.read(1)) { b.read(3); for (int i=0; i<5; ++i) b.ue(); }
  return b.ok;
}
inline bool sps(SpsBits& b, unsigned& id, unsigned& parent) {
  parent=b.read(4); unsigned layers=b.read(3); b.read(1);
  if (!ptl(b,layers) || (id=b.ue())>15 || b.ue()!=1) return false;
  unsigned w=b.ue(), h=b.ue();
  if (!w || !h || (w|h)%2 || w>kMaxWidth || h>kMaxHeight) return false;
  if (b.read(1)) { // 4:2:0 conformance window, never discounted from coded memory
    unsigned l=b.ue(), r=b.ue(), t=b.ue(), d=b.ue();
    if (l>=w/2 || r>=w/2-l || t>=h/2 || d>=h/2-t) return false;
  }
  if (b.ue()!=0 || b.ue()!=0) return false; // luma/chroma bit_depth_minus8
  unsigned poc=b.ue(); if (poc>12 || !ordering(b,layers)) return false;
  unsigned cb=b.ue(), diff=b.ue(), tb=b.ue(), tdiff=b.ue();
  if (cb>3 || diff>3-cb || tb>3 || tdiff>3-tb) return false;
  unsigned minCb=3+cb, maxCb=minCb+diff, minTb=2+tb;
  if ((w|h)%(1u<<minCb) || minTb>minCb || minTb+tdiff>maxCb ||
      b.ue()>maxCb-minTb || b.ue()>maxCb-minTb) return false;
  if (b.read(1) && b.read(1) && !scalingLists(b)) return false;
  b.read(2); // AMP, SAO
  if (b.read(1)) { if (b.read(4)>7 || b.read(4)>7) return false; b.ue(); b.ue(); b.read(1); }
  if (!referenceSets(b,maxPictures-1)) return false;
  if (b.read(1)) {
    unsigned n=b.ue(); if (n>32) return false;
    for (unsigned i=0; i<n; ++i) { b.read(4+poc); b.read(1); }
  }
  b.read(2); // temporal MVP, smoothing
  if (b.read(1) && !vui(b,layers)) return false;
  return !b.read(1) && b.endOfRbsp(); // no range, multilayer, SCC or other extensions
}
inline bool pps(SpsBits& b, unsigned& id, unsigned& parent) {
  id=b.ue(); parent=b.ue(); if (id>63 || parent>15) return false;
  b.read(7); // dependent slices, output flag, extra header bits, sign hiding, cabac init
  if (b.ue()>14 || b.ue()>14 || b.ue()>52) return false;
  b.read(2); if (b.read(1) && b.ue()>3) return false;
  if (b.ue()>24 || b.ue()>24) return false;
  b.read(4);
  bool tiles=b.read(1)!=0; b.read(1); // entropy_coding_sync_enabled_flag
  if (tiles) {
    unsigned cols=b.ue(), rows=b.ue(); if (cols>19 || rows>21) return false;
    if (!b.read(1)) { for (unsigned i=0; i<cols+rows; ++i) if (b.ue()>239) return false; }
    b.read(1);
  }
  b.read(1);
  if (b.read(1)) { b.read(1); if (!b.read(1) && (b.ue()>12 || b.ue()>12)) return false; }
  if (b.read(1) && !scalingLists(b)) return false;
  b.read(1); if (b.ue()>4) return false;
  b.read(1);
  return !b.read(1) && b.endOfRbsp();
}
} // namespace hevc

class HevcBudget {
  struct Sets {
    std::array<bool,16> vps{};
    // Zero = unseen; otherwise parent id + 1.
    std::array<unsigned,16> sps{};
    std::array<unsigned,64> pps{};
  } sets_;
public:
  Admission inspect(const Bytes& au, bool config) {
    using namespace hevc;
    if (au.empty() || au.size()>(config ? maxConfig : maxAu)) return Admission::Unsupported;
    Sets next=sets_; // commit only if the entire AU will be submitted
    size_t off=0, count=0;
    while (off<au.size()) {
      size_t start=off;
      while (off<au.size() && !au[off]) ++off;
      if (off-start<2 || off==au.size() || au[off++]!=1 || ++count>maxNals) return Admission::Unsupported;
      size_t begin=off, end=begin;
      for (; off<au.size(); ++off) {
        if (!au[off]) continue;
        if (au[off]==1 && off-end>=2) break;
        end=off+1;
      }
      const uint8_t* p=au.data()+begin; size_t n=end-begin;
      if (n<3 || !header(p,n) || !nalType(type(p))) return Admission::Unsupported;
      unsigned t=type(p);
      if (t>=32 && t<=34) {
        Bytes rbsp;
        if (p[1]!=1 || n>maxParam || !ebsp(p+2,n-2,&rbsp)) return Admission::Unsupported;
        SpsBits b(rbsp); unsigned id=0, parent=0;
        if (t==32) { if (!vps(b,id)) return Admission::Unsupported; next.vps[id]=true; }
        if (t==33) { if (!sps(b,id,parent)) return Admission::Unsupported; next.sps[id]=parent+1; }
        if (t==34) { if (!pps(b,id,parent)) return Admission::Unsupported; next.pps[id]=parent+1; }
      } else if (config) return Admission::Unsupported;
      else if (t<32) {
        // Only the first <=15 slice-header bits are needed to resolve the PPS.
        // If those bits contain EPB, they already encode an out-of-range PPS id.
        Bytes prefix(p+2,p+std::min(n,size_t(5))); SpsBits b(prefix);
        b.read(1); if (t>=16) b.read(1);
        unsigned id=b.ue(); if (!b.ok || id>=64) return Admission::Unsupported;
        unsigned s=next.pps[id], v=s ? next.sps[s-1] : 0;
        if (!v || !next.vps[v-1]) return Admission::WaitForSps;
      }
      if (off<au.size()) off=end;
    }
    sets_=next; return Admission::Ready;
  }
};

namespace hevc {
// selectMedia normally keeps the last attribute. For HEVC never allow a second
// fmtp to hide a DON requirement, or an ambiguous rtpmap to change the codec.
// Disqualify only that payload, retaining unrelated tracks and control handling.
inline std::string uniqueAttributes(const std::string& sdp) {
  std::array<unsigned char,128> flags{};
  std::istringstream lines(sdp); std::string line, result;
  result.reserve(sdp.size());
  while (std::getline(lines,line)) {
    line=trim(line);
    if (line.rfind("m=",0)==0) flags.fill(0);
    unsigned mask=line.rfind("a=fmtp:",0)==0 ? 1 : line.rfind("a=rtpmap:",0)==0 ? 2 : 0;
    if (mask) {
      size_t start=line.find(':')+1, end=line.find_first_of(" \t",start);
      int pt=number(line.substr(start,end-start),127);
      if (pt>=0) {
        if (flags[pt]&mask) line=line.substr(0,end)+" ambiguous";
        flags[pt]|=mask;
      }
    }
    result+=line; result+='\n';
  }
  return result;
}
inline bool base64Nal(const std::string& s, unsigned t, Bytes& out) {
  // RFC 4648 standard alphabet; tolerate absent padding but never ignored suffixes.
  if (s.empty() || s.size()>4*((maxParam+2)/3)) return false;
  auto pad=s.find('='); size_t len=pad==s.npos ? s.size() : pad;
  if (len%4==1 || (pad!=s.npos && (s.size()%4 || s.size()-pad>2 || s.find_first_not_of('=',pad)!=s.npos))) return false;
  unsigned acc=0, bits=0; Bytes n;
  for (size_t i=0; i<len; ++i) {
    char c=s[i]; int v=c>='A'&&c<='Z' ? c-'A' : c>='a'&&c<='z' ? c-'a'+26 :
                       c>='0'&&c<='9' ? c-'0'+52 : c=='+' ? 62 : c=='/' ? 63 : -1;
    if (v<0) return false;
    acc=(acc<<6)|unsigned(v); bits+=6;
    if (bits>=8) { bits-=8; n.push_back(uint8_t(acc>>bits)); }
  }
  if ((acc & ((1u<<bits)-1)) || n.size()<3 || n.size()>maxParam || !header(n.data(),n.size()) ||
      type(n.data())!=t || !ebsp(n.data()+2,n.size()-2) || out.size()+4+n.size()>maxConfig) return false;
  out.insert(out.end(),{0,0,0,1}); out.insert(out.end(),n.begin(),n.end()); return true;
}
}
inline bool selectHevc(const std::string& sdp, Track& out) {
  if (sdp.size()>128*1024) return false;
  return selectMedia(hevc::uniqueAttributes(sdp),"video",[&](int pt,const std::string& map,const std::string& fmt,
                                     const std::string& control,const std::string& aggregate) {
    if (map!="h265/90000") return false;
    Track tr; tr.pt=pt; tr.control=control; tr.aggregate=aggregate;
    std::array<Bytes,3> sets; std::vector<std::string> seen;
    std::istringstream params(fmt); std::string field;
    while (std::getline(params,field,';')) {
      if (trim(field).empty()) continue;
      auto eq=field.find('='); if (eq==field.npos) return false;
      auto key=lower(trim(field.substr(0,eq))), v=trim(field.substr(eq+1));
      if (seen.size()>=20 || std::find(seen.begin(),seen.end(),key)!=seen.end() || v.empty()) return false;
      seen.push_back(key);
      if (key=="sprop-vps" || key=="sprop-sps" || key=="sprop-pps") {
        unsigned k=key=="sprop-vps" ? 0 : key=="sprop-sps" ? 1 : 2;
        std::istringstream list(v); std::string nal;
        if (v.back()==',') return false;
        while (std::getline(list,nal,',')) if (!hevc::base64Nal(trim(nal),32+k,sets[k])) return false;
      } else if (key=="tx-mode") { if (lower(v)!="srst") return false; }
      else if (key=="sprop-max-don-diff" || key=="sprop-depack-buf-nalus" || key=="sprop-depack-buf-bytes" ||
               key=="sprop-segmentation-id" || key=="profile-space") { if (v!="0") return false; }
      else if (key=="profile-id") { if (v!="1") return false; }
      else if (key=="level-id") { if (number(v,255)<=0) return false; }
      else if (key=="tier-flag") { if (v!="0" && v!="1") return false; }
      else if (key=="interop-constraints") {
        if (v.size()!=12 || lower(v).find_first_not_of("0123456789abcdef")!=v.npos) return false;
      } else return false;
    }
    for (auto& set:sets) {
      if (tr.config.size()+set.size()>hevc::maxConfig) return false;
      tr.config.insert(tr.config.end(),set.begin(),set.end());
    }
    if (!tr.config.empty() && HevcBudget{}.inspect(tr.config,true)!=Admission::Ready) return false;
    out=std::move(tr); return true;
  });
}

class RtpHevc {
  Bytes au_; size_t nalStart_=0, count_=0;
  bool bound_=false, initialBad_=false, bad_=false, fu_=false, closed_=false;
  uint16_t last_=0; uint32_t ssrc_=0, stamp_=0;
  void reset() { au_.clear(); count_=0; fu_=bad_=closed_=false; }
  bool add(const uint8_t* p,size_t n,bool start) {
    if (au_.size()+n+(start?4:0)>hevc::maxAu) return false;
    if (start) {
      if (++count_>hevc::maxNals) return false;
      au_.insert(au_.end(),{0,0,0,1}); nalStart_=au_.size();
    }
    au_.insert(au_.end(),p,p+n); return true;
  }
  bool complete(const uint8_t* p,size_t n) {
    return n>=3 && hevc::header(p,n) && hevc::nalType(hevc::type(p)) && hevc::ebsp(p+2,n-2) && add(p,n,true);
  }
  bool payload(const uint8_t* p,size_t n) {
    if (!hevc::header(p,n)) return false;
    unsigned t=hevc::type(p);
    if (t==48 && !fu_) {
      unsigned units=0, minTid=7;
      for (size_t i=2; i<n;) {
        if (n-i<2) return false;
        size_t sz=(p[i]<<8)|p[i+1]; i+=2;
        if (sz>n-i || !complete(p+i,sz)) return false;
        minTid=std::min(minTid,unsigned(p[i+1])); ++units; i+=sz;
      }
      return units>=2 && minTid==p[1];
    }
    if (t!=49) return !fu_ && complete(p,n); // includes rejection of PACI and nested structures
    if (n<4) return false;
    bool start=p[2]&128, end=p[2]&64; unsigned kind=p[2]&63;
    uint8_t h[]={uint8_t(kind<<1),p[1]};
    if (!hevc::nalType(kind) || (start && end) || start==fu_) return false;
    if (start) { if (!add(h,2,true)) return false; fu_=true; }
    else if (au_[nalStart_]!=h[0] || au_[nalStart_+1]!=h[1]) return false;
    if (!add(p+3,n-3,false)) return false;
    if (end) { fu_=false; return hevc::ebsp(au_.data()+nalStart_+2,au_.size()-nalStart_-2); }
    return true;
  }
public:
  bool packet(const Bytes& b,int pt,Bytes& frame,uint32_t& pts) {
    frame.clear();
    if (b.size()<12 || b.size()>65535 || b[0]>>6!=2 || (b[1]&127)!=pt) return false;
    auto word=[&](size_t i){return uint32_t(b[i])<<24 | uint32_t(b[i+1])<<16 | uint32_t(b[i+2])<<8 | b[i+3];};
    uint32_t source=word(8), ts=word(4); uint16_t seq=uint16_t(b[2]<<8 | b[3]);
    uint16_t delta=uint16_t(seq-last_);
    if (!bound_ && initialBad_ && ts==stamp_ && source==ssrc_) return false;
    if (bound_ && (source!=ssrc_ || !delta || delta>=0x8000)) return false;
    if (!bound_ || ts!=stamp_) reset();
    if (bound_ && delta!=1) bad_=true; // conservative even if the gap straddled two timestamps
    size_t off=12+4*(b[0]&15), end=b.size();
    if (b[0]&32) { if (!b.back() || b.back()>end) bad_=true; else end-=b.back(); }
    if (b[0]&16) { if (off+4>end) bad_=true; else off+=4+4*((b[off+2]<<8)|b[off+3]); }
    if (off>=end || closed_ || (!bad_ && !payload(b.data()+off,end-off))) bad_=true;
    bool mark=(b[1]&128)!=0;
    if (mark && fu_) bad_=true;
    if (!bound_ && bad_) { initialBad_=true; stamp_=ts; ssrc_=source; reset(); return false; }
    bound_=true; ssrc_=source; last_=seq; stamp_=ts;
    if (bad_) { au_.clear(); fu_=false; return false; } // poison this timestamp through its end
    if (!mark) return false;
    closed_=true; frame.swap(au_); pts=ts; return true;
  }
};
} // namespace liveavc
