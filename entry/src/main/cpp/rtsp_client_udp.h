#pragma once
#include "rtsp_client_protocol.h"
#include <charconv>
#include <cstring>
#include <optional>
#include <set>
#ifdef _WIN32 // Also exercised with real Winsock loopback sockets in the host fixture.
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rtspudp {
#ifdef _WIN32
using Fd=SOCKET; using Len=int; using PollFd=WSAPOLLFD;
constexpr Fd invalid=INVALID_SOCKET;
inline void closeFd(Fd f){closesocket(f);}
inline bool nonblock(Fd f){u_long one=1;return ioctlsocket(f,FIONBIO,&one)==0;}
inline int poll3(PollFd* p){return WSAPoll(p,3,100);}
inline bool interrupted(){return WSAGetLastError()==WSAEINTR;}
#else
using Fd=int; using Len=socklen_t; using PollFd=pollfd;
constexpr Fd invalid=-1;
inline void closeFd(Fd f){close(f);}
inline bool nonblock(Fd f){return fcntl(f,F_SETFL,O_NONBLOCK)==0;}
inline int poll3(PollFd* p){return poll(p,3,100);}
inline bool interrupted(){return errno==EINTR;}
#endif
struct Datagram {
  Fd fd=invalid;
  Datagram()=default; Datagram(const Datagram&)=delete;
  void reset(){if(fd!=invalid){closeFd(fd);fd=invalid;}}
  ~Datagram(){reset();}
};
struct Address {
  sockaddr_storage storage{}; Len size=sizeof(storage);
  sockaddr* addr(){return reinterpret_cast<sockaddr*>(&storage);}
  const sockaddr* addr()const{return reinterpret_cast<const sockaddr*>(&storage);}
  sockaddr_in& v4(){return reinterpret_cast<sockaddr_in&>(storage);}
  sockaddr_in6& v6(){return reinterpret_cast<sockaddr_in6&>(storage);}
  const sockaddr_in& v4()const{return reinterpret_cast<const sockaddr_in&>(storage);}
  const sockaddr_in6& v6()const{return reinterpret_cast<const sockaddr_in6&>(storage);}
  bool usable()const{
    if(storage.ss_family==AF_INET){auto ip=ntohl(v4().sin_addr.s_addr);return ip && ip<0xe0000000u;}
    return storage.ss_family==AF_INET6 && !IN6_IS_ADDR_UNSPECIFIED(&v6().sin6_addr) && !IN6_IS_ADDR_MULTICAST(&v6().sin6_addr) && !IN6_IS_ADDR_V4MAPPED(&v6().sin6_addr);
  }
  void port(int n){if(storage.ss_family==AF_INET)v4().sin_port=htons(uint16_t(n));else v6().sin6_port=htons(uint16_t(n));}
  int port()const{return ntohs(storage.ss_family==AF_INET?v4().sin_port:v6().sin6_port);}
  bool literal(std::string text)const{
    if(text.empty() || text.find('\0')!=text.npos)return false;
    if(storage.ss_family==AF_INET){in_addr a{};return inet_pton(AF_INET,text.c_str(),&a)==1 && a.s_addr==v4().sin_addr.s_addr;}
    if(text.front()=='[' && text.back()==']')text=text.substr(1,text.size()-2);
    in6_addr a{};return inet_pton(AF_INET6,text.c_str(),&a)==1 && !std::memcmp(&v6().sin6_addr,&a,sizeof(a));
  }
  bool same(const Address& b)const{
    if(storage.ss_family!=b.storage.ss_family || port()!=b.port())return false;
    if(storage.ss_family==AF_INET)return v4().sin_addr.s_addr==b.v4().sin_addr.s_addr;
    return v6().sin6_scope_id==b.v6().sin6_scope_id && !std::memcmp(&v6().sin6_addr,&b.v6().sin6_addr,sizeof(in6_addr));
  }
};
inline uint32_t word(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|p[2]<<8|p[3];}
struct Stream {
  std::optional<uint32_t> ssrc;
  std::optional<uint16_t> last;
  bool rtp(const liveavc::Bytes& b,int pt){
    if(b.size()<13 || b[0]>>6!=2 || (b[1]&127)!=pt || (b[1]>=192 && b[1]<=223))return false;
    size_t off=12+4*(b[0]&15), end=b.size();
    if(b[0]&32){if(!b.back() || b.back()>end)return false;end-=b.back();}
    if(b[0]&16){if(off+4>end)return false;off+=4+4*((b[off+2]<<8)|b[off+3]);}
    if(off>=end)return false;
    uint32_t id=word(&b[8]);uint16_t seq=uint16_t(b[2]<<8|b[3]);
    if(ssrc && *ssrc!=id)return false;
    if(last){auto delta=uint16_t(seq-*last);if(!delta || delta>=0x8000)return false;}
    last=seq;ssrc=id;return true; // forward gaps go to the bounded AVC/HEVC depacketizer
  }
  // Only a structurally valid BYE naming this video SSRC has a side effect. Never
  // feed RTCP to a decoder or follow SDES/APP data. No receiver reports or NAT probes.
  bool bye(const liveavc::Bytes& b)const{
    bool endStream=false;
    for(size_t i=0;i<b.size();){
      if(b.size()-i<4 || b[i]>>6!=2 || b[i+1]<192 || b[i+1]>223)return false;
      size_t n=4*(1u+(b[i+2]<<8)+b[i+3]);if(n>b.size()-i)return false;
      size_t used=n; if(b[i]&32){if(i+n!=b.size() || !b.back() || b.back()>n-4)return false;used-=b.back();}
      size_t count=b[i]&31;auto type=b[i+1];
      if(type==200 && used!=28+24*count)return false;
      if(type==201 && used!=8+24*count)return false;
      if(type==202){ // Each SDES chunk has an SSRC, length-prefixed items, END and zero alignment.
        size_t k=4;
        for(size_t chunk=0;chunk<count;++chunk){
          if(used-k<4)return false;k+=4;
          while(k<used && b[i+k]){if(used-k<2 || b[i+k+1]>used-k-2)return false;k+=2+b[i+k+1];}
          if(k==used)return false;++k;
          while(k%4){if(k==used || b[i+k])return false;++k;}
        }
        if(k!=used)return false;
      }
      if(type<200 || type>203)return false; // ignore unhandled compound packets in their entirety
      if(type==203){
        size_t pos=4+4*count;if(!count || pos>used)return false;
        if(pos<used){size_t reason=1+b[i+pos];if(reason>used-pos || used-pos-reason>3)return false;
          for(size_t k=pos+reason;k<used;++k)if(b[i+k])return false;}
        for(size_t k=4;k<pos;k+=4)if(ssrc && word(&b[i+k])==*ssrc)endStream=true;
      }
      i+=n;
    }
    return endStream;
  }
};
struct Media {
  Datagram rtp,rtcp; Address peer,local,fromRtp,fromRtcp; int client=0; Stream stream;
  bool bindOne(Datagram& d,int port,int& assigned){
    Address a=local;a.port(port);d.fd=socket(a.storage.ss_family,SOCK_DGRAM,IPPROTO_UDP);
    if(d.fd==invalid || !nonblock(d.fd) || bind(d.fd,a.addr(),a.size)!=0 || getsockname(d.fd,a.addr(),&a.size)!=0)return false;
    assigned=a.port();return true;
  }
  bool open(Fd control){
    if(getpeername(control,peer.addr(),&peer.size) || getsockname(control,local.addr(),&local.size) ||
       !peer.usable() || !local.usable() || peer.storage.ss_family!=local.storage.ss_family)return false;
    // OS-selected ephemeral pair on the actual local interface. No remote probe or port search.
    for(int attempt=0;attempt<8;++attempt){
      int p=0,other=0;rtp.reset();rtcp.reset();
      if(!bindOne(rtp,0,p))return false;
      if(bindOne(rtcp,p^1,other) && other==(p^1)){
        if(p&1)std::swap(rtp.fd,rtcp.fd);client=p&~1;return client>0;
      }
    }
    return false;
  }
  std::string offer()const{return "RTP/AVP;unicast;client_port="+std::to_string(client)+"-"+std::to_string(client+1);}
  static int pair(const std::string& value){auto dash=value.find('-');if(dash==value.npos)return -1;
    int p=liveavc::number(value.substr(0,dash),65535),q=liveavc::number(value.substr(dash+1),65535);
    return p>0 && q==p+1?p:-1;}
  bool accept(const std::string& value){
    if(!client || value.empty() || value.find_first_of(",\r\n")!=value.npos || value.back()==';')return false;
    std::istringstream in(value);std::string item;std::getline(in,item,';');auto profile=liveavc::lower(liveavc::trim(item));
    if(profile!="rtp/avp" && profile!="rtp/avp/udp")return false;
    std::set<std::string> seen;int server=-1;
    while(std::getline(in,item,';')){
      auto eq=item.find('=');auto key=liveavc::lower(liveavc::trim(item.substr(0,eq)));
      if(!seen.insert(key).second)return false;
      if(eq==item.npos){if(key!="unicast")return false;continue;}
      auto arg=liveavc::trim(item.substr(eq+1));
      if(key=="client_port"){if(pair(arg)!=client)return false;}
      else if(key=="server_port"){server=pair(arg);if(server<0)return false;}
      else if(key=="source"){if(!peer.literal(arg))return false;}
      else if(key=="destination"){if(!local.literal(arg))return false;}
      else if(key=="ssrc"){uint32_t id=0;auto end=arg.data()+arg.size();auto r=std::from_chars(arg.data(),end,id,16);
        if(arg.empty() || arg.size()>8 || r.ec!=std::errc() || r.ptr!=end)return false;stream.ssrc=id;}
      else if(key=="mode"){if(liveavc::lower(arg)!="play" && liveavc::lower(arg)!="\"play\"")return false;}
      else return false;
    }
    if(!seen.count("unicast") || !seen.count("client_port") || server<0)return false;
    fromRtp=fromRtcp=peer;fromRtp.port(server);fromRtcp.port(server+1);
    return connect(rtp.fd,fromRtp.addr(),fromRtp.size)==0 && connect(rtcp.fd,fromRtcp.addr(),fromRtcp.size)==0;
  }
  enum Ready { Error=-1, Idle=0, Control=1, Video=2, End=3 };
  liveavc::Bytes scratch;
  bool receive(Fd fd,const Address& source,liveavc::Bytes& b){
    // One extra byte detects truncated/oversized datagrams even without MSG_TRUNC.
    constexpr size_t cap=65507;if(scratch.empty())scratch.resize(cap+1);Address actual;
    int n=static_cast<int>(recvfrom(fd,reinterpret_cast<char*>(scratch.data()),cap+1,0,actual.addr(),&actual.size));
    if(n<=0 || size_t(n)>cap || !source.same(actual))return false;
    b.assign(scratch.begin(),scratch.begin()+n);return true;
  }
  Ready next(Fd control,bool buffered,int pt,liveavc::Bytes& b){
    if(buffered)return Control;
    PollFd p[]={{control,POLLIN,0},{rtp.fd,POLLIN,0},{rtcp.fd,POLLIN,0}};
    int n=poll3(p);if(n<0)return interrupted()?Idle:Error;if(!n)return Idle;
    if(p[0].revents&POLLIN)return Control;
    for(const auto& f:p)if(f.revents&(POLLERR|POLLHUP|POLLNVAL))return Error;
    // Drain at most one datagram per socket per turn, even under a flood.
    if((p[2].revents&POLLIN) && receive(rtcp.fd,fromRtcp,b) && stream.bye(b))return End;
    if((p[1].revents&POLLIN) && receive(rtp.fd,fromRtp,b) && stream.rtp(b,pt))return Video;
    return Idle;
  }
};
} // namespace rtspudp
