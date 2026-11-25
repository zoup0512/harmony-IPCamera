#pragma once
// RFC 2326/4566, RFC 3550 and RFC 6184 (single NAL, STAP-A, FU-A).
// No OS/decoder dependencies: exercised by the host packet tests.
#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace liveavc {
using Bytes = std::vector<uint8_t>;
inline std::string trim(const std::string& s) {
  auto a = s.find_first_not_of(" \t\r\n");
  return a == s.npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
inline std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
inline bool token(const std::string& s) {
  return !s.empty() && s.find_first_of("\\#") == s.npos && std::all_of(s.begin(), s.end(), [](unsigned char c){ return c > 32 && c < 127; });
}
inline int number(const std::string& s, int max) {
  if (s.empty()) return -1;
  int value = 0;
  for (char c : s) { if (c < '0' || c > '9' || value > (max - (c-'0')) / 10) return -1; value = value * 10 + (c-'0'); }
  return value <= max ? value : -1;
}
inline std::string origin(const std::string& s) {
  if (s.rfind("rtsp://", 0) != 0 || !token(s)) return "";
  auto auth = s.substr(7, s.find_first_of("/?",7) - 7);
  return auth.empty() || auth.find('@') != auth.npos ? "" : "rtsp://" + lower(auth);
}
inline std::string controlUrl(const std::string& root, std::string base, const std::string& path) {
  auto host = origin(root);
  if (host.empty() || !token(path) || path.rfind("//",0)==0) return "";
  if (base.empty()) base = root;
  if (origin(base) != host) return "";
  if (path.find("://") != path.npos) return origin(path) == host ? path : "";
  if (path == "*") return root;
  if (path.front() == '/') return host + path;
  base = base.substr(0,base.find('?'));
  return base + (base.back() == '/' ? "" : "/") + path;
}
inline Bytes unbase64(const std::string& s) {
  const std::string chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  Bytes out; unsigned acc=0, bits=0;
  for(char c:s) { if(c=='=') break; auto n=chars.find(c); if(n==chars.npos) return {}; acc=(acc<<6)|unsigned(n); bits+=6; if(bits>=8){bits-=8;out.push_back(uint8_t(acc>>bits));} }
  return out;
}
struct Track { int pt=-1; std::string control, aggregate="*"; Bytes config; };
// Visit advertised payloads only in their own media section. Track selection never
// borrows control, config or rtpmap from another m= section with the same PT.
template<class Select>
inline bool selectMedia(const std::string& sdp, const char* kindWanted, Select select) {
  std::istringstream stream(sdp); std::string line, aggregate="*"; bool media=false, matching=false;
  std::vector<int> pts; std::vector<std::string> maps(128), fmts(128); std::string control;
  auto finish=[&]() {
    if (matching && !control.empty()) for (int n : pts)
      if (select(n, maps[n], fmts[n], control, aggregate)) return true;
    return false;
  };
  while(std::getline(stream,line)) { line=trim(line);
    if(line.rfind("m=",0)==0){ if(finish()) return true; media=true; pts.clear(); maps.assign(128,"");fmts.assign(128,"");control.clear();
      std::istringstream m(line.substr(2));std::string kind,port,proto,pt;m>>kind>>port>>proto;matching=kind==kindWanted && proto=="RTP/AVP";
      std::array<bool, 128> seen{};
      while (matching && m >> pt) {
        int n = number(pt, 127);
        if (n >= 0 && !seen[n]) { seen[n] = true; pts.push_back(n); }
      }
    } else if(line.rfind("a=control:",0)==0){ if(!media)aggregate=line.substr(10); else control=line.substr(10);
    } else if(matching && (line.rfind("a=rtpmap:",0)==0 || line.rfind("a=fmtp:",0)==0)) {
      auto start=line.find(':')+1, space=line.find_first_of(" \t",start); if(space==line.npos)continue;
      int n=number(line.substr(start,space-start),127);if(n<0)continue;auto value=trim(line.substr(space));
      if(line.rfind("a=rtpmap:",0)==0)maps[n]=lower(value);else fmts[n]=value;
    }
  } return finish();
}
inline bool selectAvc(const std::string& sdp, Track& out) {
  return selectMedia(sdp,"video",[&](int n, const std::string& map, const std::string& fmt,
                                      const std::string& control, const std::string& aggregate) {
      if (map != "h264/90000") return false;
      Track t; bool supported=true; t.pt=n; t.control=control; t.aggregate=aggregate;
      std::istringstream params(fmt); std::string p, sets;
      while(std::getline(params,p,';')) { p=trim(p); auto eq=p.find('='); if(eq==p.npos) continue;
        auto key=lower(trim(p.substr(0,eq))), value=trim(p.substr(eq+1));
        if(key=="packetization-mode" && value!="0" && value!="1") { supported=false; break; }
        if(key=="sprop-parameter-sets") sets=std::move(value);
      }
      if (!supported) return false;
      std::istringstream list(sets); std::string v;
      while (std::getline(list, v, ',')) {
        auto nal = unbase64(trim(v));
        if (nal.empty() || ((nal[0]&31) != 7 && (nal[0]&31) != 8)) { supported=false; break; }
        t.config.insert(t.config.end(), {0,0,0,1});
        t.config.insert(t.config.end(), nal.begin(), nal.end());
      }
      if(supported) out=std::move(t);
      return supported;
  });
}

struct AacTrack { int pt=-1, sfIndex=0, channels=0; std::string control; };
inline bool selectAac(const std::string& sdp, AacTrack& out) {
  return selectMedia(sdp,"audio",[&](int pt,const std::string& map,const std::string& fmt,
                                      const std::string& control,const std::string&) {
    // Only the non-interleaved AAC-LC 1024-sample form that VoicePlayer can configure.
    std::istringstream params(lower(fmt)); std::string item, config;
    bool mode=false, size=false, index=false, delta=false;
    while (std::getline(params,item,';')) {
      if (trim(item).empty()) continue;
      auto eq=item.find('='); if (eq==item.npos) return false;
      auto key=trim(item.substr(0,eq)), v=trim(item.substr(eq+1));
      if (key=="mode" && v=="aac-hbr" && !mode) mode=true;
      else if (key=="sizelength" && v=="13" && !size) size=true;
      else if (key=="indexlength" && v=="3" && !index) index=true;
      else if (key=="indexdeltalength" && v=="3" && !delta) delta=true;
      else if (key=="config" && v.size()==4 && config.empty()) config=v;
      else if (key=="profile-level-id" || (key=="streamtype" && v=="5") ||
               (key=="constantduration" && v=="1024")) continue;
      else return false; // no auxiliary data, interleaving, CTS/DTS or unknown layout
    }
    if (!mode || !size || !index || !delta || config.empty()) return false;
    unsigned asc=0;
    for (char c:config) {
      if (c>='0' && c<='9') asc=asc*16+c-'0';
      else if (c>='a' && c<='f') asc=asc*16+c-'a'+10;
      else return false;
    }
    int sf=(asc>>7)&15, ch=(asc>>3)&15;
    constexpr int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    if ((asc>>11)!=2 || (asc&7) || sf>=13 || (ch!=1 && ch!=2)) return false;
    auto encoding="mpeg4-generic/"+std::to_string(rates[sf]);
    if (map!=encoding+"/"+std::to_string(ch) && !(ch==1 && map==encoding)) return false;
    out={pt,sf,ch,control}; return true;
  });
}

struct ChannelPair { int rtp=-1, rtcp=-1; };
inline bool parseChannels(std::string value, ChannelPair& out) {
  std::istringstream fields(lower(value)); std::string f;
  if (value.find(',')!=value.npos || !std::getline(fields,f,';') || trim(f)!="rtp/avp/tcp") return false;
  ChannelPair pair;
  while (std::getline(fields,f,';')) {
    f=trim(f); if (f=="multicast") return false;
    if (f.rfind("interleaved=",0)!=0) continue;
    auto dash=f.find('-',12); if (dash==f.npos || pair.rtp>=0) return false;
    pair.rtp=number(f.substr(12,dash-12),255); pair.rtcp=number(f.substr(dash+1),255);
    if (pair.rtp<0 || pair.rtcp<0 || pair.rtp==pair.rtcp) return false;
  }
  if (pair.rtp<0) return false;
  out=pair; return true;
}

// RFC 3640: validate the entire AU header section before emitting any audio.
// Reassemble at most one ADTS-sized fragmented AU; no state crosses a mute epoch.
class RtpAac {
  Bytes partial;
  uint32_t source=0, stamp=0; uint16_t last=0;
  size_t expected=0; bool bound=false;
  static void append(Bytes& frames,const AacTrack& tr,const uint8_t* data,size_t size) {
    size_t len=size+7;
    frames.insert(frames.end(),{0xff,0xf1,uint8_t(0x40|(tr.sfIndex<<2)),uint8_t((tr.channels<<6)|(len>>11)),
                                uint8_t(len>>3),uint8_t((len<<5)|31),0xfc});
    frames.insert(frames.end(),data,data+size);
  }
public:
  bool packet(const Bytes& b,const AacTrack& tr,Bytes& frames) {
    frames.clear();
    if (b.size()<12 || b.size()>65535 || b[0]>>6!=2 || (b[1]&127)!=tr.pt) return false;
    auto word=[&](size_t i){return (uint32_t(b[i])<<24)|(uint32_t(b[i+1])<<16)|(b[i+2]<<8)|b[i+3];};
    uint32_t ssrc=word(8), ts=word(4); uint16_t seq=uint16_t(b[2]<<8|b[3]);
    if (bound && source!=ssrc) return false;
    if (bound && (last==seq || ts!=stamp || uint16_t(last+1)!=seq)) {
      if (last==seq) return false;
      partial.clear();
    }
    auto invalid=[&](){partial.clear();return false;};
    size_t off=12+4*(b[0]&15), end=b.size();
    if (b[0]&32) { if (!b.back() || b.back()>end) return invalid(); end-=b.back(); }
    if (b[0]&16) { if (off+4>end) return invalid(); off+=4+4*(b[off+2]*256+b[off+3]); }
    if (off+4>end) return invalid();
    unsigned bits=b[off]*256+b[off+1]; off+=2;
    if (!bits || bits%16 || bits/16>64 || off+bits/8>=end) return invalid();
    size_t pos=off+bits/8, total=0, count=bits/16;
    std::array<size_t,64> sizes{};
    for (size_t i=0;i<count;++i,off+=2) {
      sizes[i]=(b[off]*256+b[off+1])>>3;
      if ((b[off+1]&7) || !sizes[i] || sizes[i]>8184) return invalid();
      total+=sizes[i];
    }
    size_t n=end-pos; bool mark=(b[1]&128)!=0;
    if (!partial.empty() || total!=n || !mark) {
      if (count!=1 || (!partial.empty() && expected!=total) || partial.size()+n>total ||
          mark!=(partial.size()+n==total)) return invalid();
      expected=total; partial.insert(partial.end(),b.begin()+pos,b.begin()+end);
      if (mark) { append(frames,tr,partial.data(),total); partial.clear(); }
    } else {
      for (size_t i=0;i<count;++i) { append(frames,tr,b.data()+pos,sizes[i]); pos+=sizes[i]; }
    }
    bound=true; source=ssrc; stamp=ts; last=seq; return !frames.empty();
  }
};
// Playback supports ordinary 8-bit 4:2:0 AVC through level 4.2. Limit coded
// dimensions (not the cropped display size) and the decoded picture buffer.
constexpr unsigned kMaxWidth = 1920, kMaxHeight = 1088, kMaxDpbMbs = 34816;
inline bool fitsPicture(int width, int height) {
  return width > 0 && height > 0 && unsigned(width) <= kMaxWidth && unsigned(height) <= kMaxHeight;
}

class SpsBits {
 public:
  explicit SpsBits(const Bytes& bytes) : bytes_(bytes) {}
  unsigned read(unsigned count) {
    if (!ok || count > 32 || count > bytes_.size() * 8 - bit_) { ok = false; return 0; }
    unsigned value = 0;
    while (count--) { value = (value << 1) | ((bytes_[bit_ / 8] >> (7 - bit_ % 8)) & 1); ++bit_; }
    return value;
  }
  unsigned ue() {
    unsigned zeros = 0;
    while (ok && !read(1)) { if (++zeros > 30) { ok = false; return 0; } }
    return ((1u << zeros) - 1) + read(zeros);
  }
  bool endOfRbsp() {
    if (!read(1)) return false;
    while (bit_ % 8) if (read(1)) return false;
    return ok && bit_ == bytes_.size() * 8;
  }
  bool ok = true;
 private:
  const Bytes& bytes_;
  size_t bit_ = 0;
};

inline bool skipHrd(SpsBits& b) {
  unsigned countMinusOne = b.ue();
  if (countMinusOne > 31) return false;
  b.read(8);
  for (unsigned i = 0; i <= countMinusOne; ++i) { b.ue(); b.ue(); b.read(1); }
  b.read(20);
  return b.ok;
}

inline bool fitsVui(SpsBits& b, unsigned maxFrames, unsigned refs) {
  if (b.read(1) && b.read(8) == 255) b.read(32); // extended sample aspect ratio
  if (b.read(1)) b.read(1); // overscan
  if (b.read(1)) { b.read(4); if (b.read(1)) b.read(24); } // video signal
  if (b.read(1)) { b.ue(); b.ue(); } // chroma location
  if (b.read(1)) { b.read(32); b.read(32); b.read(1); } // timing
  bool nalHrd = b.read(1) != 0;
  if (nalHrd && !skipHrd(b)) return false;
  bool vclHrd = b.read(1) != 0;
  if (vclHrd && !skipHrd(b)) return false;
  if (nalHrd || vclHrd) b.read(1);
  b.read(1); // pic_struct_present_flag
  if (b.read(1)) {
    b.read(1); b.ue(); b.ue(); b.ue(); b.ue();
    unsigned reorder = b.ue(), buffering = b.ue();
    if (buffering < refs || reorder > buffering || buffering > maxFrames) return false;
  }
  return b.ok;
}

inline bool fitsSps(const uint8_t* nal, size_t size) {
  if (size < 2 || size > 4096 || (nal[0] & 0x9f) != 7 || !(nal[0] & 0x60)) return false;
  Bytes rbsp;
  rbsp.reserve(size - 1);
  unsigned zeros = 0;
  for (size_t i = 1; i < size; ++i) {
    uint8_t v = nal[i];
    if (zeros == 2 && v <= 3) {
      if (v != 3 || i + 1 == size || nal[i+1] > 3) return false;
      zeros = 0; continue;
    }
    rbsp.push_back(v);
    zeros = v == 0 ? zeros + 1 : 0;
  }
  SpsBits b(rbsp);
  unsigned profile = b.read(8);
  b.read(8); // constraint flags
  unsigned level = b.read(8);
  if (level == 0 || level > 42 || b.ue() > 31) return false;
  if (profile == 100) {
    if (b.ue() != 1 || b.ue() != 0 || b.ue() != 0) return false;
    b.read(1);
    if (b.read(1)) { // seq_scaling_matrix_present_flag (8 lists for 4:2:0)
      for (unsigned i = 0; i < 8; ++i) if (b.read(1)) {
        int next = 8;
        for (unsigned j = 0; j < (i < 6 ? 16u : 64u) && next; ++j) {
          unsigned code = b.ue();
          if (code > 256) return false;
          int delta = (code & 1) ? int((code + 1) / 2) : -int(code / 2);
          next = (next + delta + 256) % 256;
        }
      }
    }
  } else if (profile != 66 && profile != 77) return false;
  if (b.ue() > 12) return false; // log2_max_frame_num_minus4
  unsigned poc = b.ue();
  if (poc == 0) { if (b.ue() > 12) return false; }
  else if (poc == 1) {
    b.read(1); b.ue(); b.ue();
    unsigned cycle = b.ue();
    if (cycle > 255) return false;
    for (unsigned i = 0; i < cycle; ++i) b.ue();
  } else if (poc != 2) return false;
  unsigned refs = b.ue();
  b.read(1);
  unsigned cols = b.ue() + 1, rows = b.ue() + 1;
  if (!b.read(1)) { rows *= 2; b.read(1); }
  if (cols > kMaxWidth / 16 || rows > kMaxHeight / 16) return false;
  unsigned maxFrames = std::min(16u, kMaxDpbMbs / (cols * rows));
  if (refs > maxFrames) return false;
  b.read(1); // direct_8x8_inference_flag
  if (b.read(1)) { b.ue(); b.ue(); b.ue(); b.ue(); } // cropping doesn't reduce the budget
  if (b.read(1) && !fitsVui(b, maxFrames, refs)) return false;
  return b.endOfRbsp();
}

enum class Admission { Ready, WaitForSps, Unsupported };
class AvcBudget {
 public:
  // Inspect the exact Annex B bytes the decoder will receive, after FU/STAP assembly.
  Admission inspect(const Bytes& au, bool config) {
    bool seen = haveSps_;
    size_t i = 0;
    while (i < au.size()) {
      size_t zeros = i;
      while (i < au.size() && au[i] == 0) ++i;
      if (i - zeros < 2 || i == au.size() || au[i++] != 1 || i == au.size()) return Admission::Unsupported;
      size_t begin = i, end = i;
      for (; i < au.size(); ++i) {
        if (au[i] == 0) continue;
        if (au[i] == 1 && i - end >= 2) break;
        end = i + 1;
      }
      if (end == begin || (au[begin] & 128)) return Admission::Unsupported;
      unsigned type = au[begin] & 31;
      if (config && type != 7 && type != 8) return Admission::Unsupported;
      switch (type) {
        case 7:
          if (!fitsSps(au.data() + begin, end - begin)) return Admission::Unsupported;
          seen = true; break;
        case 1: case 5:
          if (!seen) return Admission::WaitForSps;
          break;
        case 6: case 8: case 9: case 10: case 11: case 12: break;
        default: return Admission::Unsupported; // extension/subset SPS and non-AVC layers
      }
      if (i != au.size()) i = end;
    }
    haveSps_ = seen;
    return au.empty() ? Admission::Unsupported : Admission::Ready;
  }
 private:
  bool haveSps_ = false;
};

// Drop a partial access unit on loss, timestamp switch or malformed fragment.
class RtpAvc {
  static constexpr size_t limit = 4*1024*1024;
  Bytes au; bool started=false, bad=false, fu=false; unsigned fuType=0; uint16_t seq=0;
  uint32_t ts=0, ssrc=0;
  bool add(const uint8_t* p,size_t n,bool start) {
    if(n>limit || au.size()+n+(start?4:0)>limit) return false;
    if(start)au.insert(au.end(),{0,0,0,1}); au.insert(au.end(),p,p+n);return true;
  }
public:
  bool packet(const Bytes& b,int pt,Bytes& frame,uint32_t& pts) {
    if(b.size()<12 || b[0]>>6!=2 || (b[1]&127)!=pt)return false;
    uint16_t s=(b[2]<<8)|b[3];uint32_t t=uint32_t(b[4])<<24|uint32_t(b[5])<<16|b[6]<<8|b[7];
    // One SETUP corresponds to one decoder/timestamp epoch. Ignore other sources;
    // accepting a new source requires a fresh session, including new codec state.
    uint32_t source = (uint32_t(b[8]) << 24) | (uint32_t(b[9]) << 16) | (b[10] << 8) | b[11];
    if (started && source != ssrc) return false;
    ssrc = source;
    bool loss=started && uint16_t(seq+1)!=s; if(!started||ts!=t){au.clear();bad=false;fu=false;} if(loss)bad=true;
    started=true;seq=s;ts=t;size_t off=12+4*(b[0]&15), end=b.size();
    if(b[0]&32){if(!b.back() || b.back()>end)bad=true;else end-=b.back();}
    if(b[0]&16){if(off+4>end)bad=true;else off+=4+4*((b[off+2]<<8)|b[off+3]);}
    if(off>=end)bad=true;
    if(!bad){const uint8_t* p=b.data()+off;size_t n=end-off;unsigned type=p[0]&31;
      if(p[0]&128)bad=true;
      else if(type>=1 && type<=23)bad=fu || !add(p,n,true);
      else if(type==24 && !fu){size_t i=1; if(i==n)bad=true;while(i<n&&!bad){if(i+2>n){bad=true;break;}size_t k=p[i]*256+p[i+1];i+=2;
        if(!k||k>n-i||(p[i]&128)||!(p[i]&31)||(p[i]&31)>23||!add(p+i,k,true)){bad=true;break;}i+=k;}}
      else if(type==28 && n>=3){bool a=p[1]&128,z=p[1]&64;unsigned kind=p[1]&31;
        if(!kind||kind>23||(p[1]&32)||(a&&z)||a==fu||(!a&&kind!=fuType))bad=true;
        else {if(a){uint8_t h=static_cast<uint8_t>((p[0]&224)|kind);bad=!add(&h,1,true);fu=true;fuType=kind;}if(!bad)bad=!add(p+2,n-2,false);if(z)fu=false;}
      }else bad=true;
    }
    if(!(b[1]&128))return false;
    bool ok=!bad&&!fu&&!au.empty();if(ok){frame.clear();frame.swap(au);pts=t;}au.clear();bad=false;fu=false;return ok;
  }
};
} // namespace liveavc
