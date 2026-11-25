// c++ -std=c++17 tests/live_rtsp_client_test.cpp -o /tmp/rtsp-test && /tmp/rtsp-test
#include "../entry/src/main/cpp/rtsp_client_protocol.h"
#include <cassert>
#include <iostream>
using namespace liveavc;
static Bytes pkt(int seq, int ts, bool marker, std::initializer_list<uint8_t> payload) {
  Bytes b={0x80,static_cast<uint8_t>(96|(marker?128:0)),uint8_t(seq>>8),uint8_t(seq),0,0,0,uint8_t(ts),0,0,0,5};
  b.insert(b.end(),payload);return b;
}
int main(){
  Track tr;
  const std::string sdp="v=0\r\na=control:*\r\nm=audio 0 RTP/AVP 98\r\na=rtpmap:98 MPEG4-generic/48000/2\r\n"
    "a=control:trackID=1\r\nm=video 0 RTP/AVP 97 96\r\na=rtpmap:97 H265/90000\r\na=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAHw==,aO48gA==\r\na=control:trackID=0\r\n";
  assert(selectAvc(sdp,tr) && tr.pt==96 && tr.control=="trackID=0" && tr.config.size()==16 && tr.config[4]==0x67 && tr.config[12]==0x68);
  assert(!selectAvc("m=video 0 RTP/AVP 96\na=rtpmap:96 H264/90000\na=fmtp:96 packetization-mode=2\na=control:trackID=0\n",tr));
  assert(!selectAvc("m=video 0 RTP/AVP 96\na=rtpmap:96 H265/90000\na=control:video\n",tr));
  const std::string root="rtsp://camera:8554/live?secret=private";
  assert(controlUrl(root,"","video")=="rtsp://camera:8554/live/video");
  assert(controlUrl(root,"rtsp://camera:8554/media/","video")=="rtsp://camera:8554/media/video");
  assert(controlUrl(root,"","*")==root);
  for(auto bad:{"rtsp://different/live","//third-party/track","rtsp://secret@camera:8554/live","track\r\nFake-Header: x"})assert(controlUrl(root,"",bad).empty());
  assert(controlUrl(root,"rtsp://other/live/","video").empty());
  RtpAvc r; Bytes au; uint32_t pts=0;
  assert(r.packet(pkt(65535,1,true,{0x65,0xaa}),96,au,pts) && au==Bytes({0,0,0,1,0x65,0xaa}) && pts==1);
  assert(!r.packet(pkt(0,2,false,{0x7c,0x85,1,2}),96,au,pts));
  assert(!r.packet(pkt(1,2,false,{0x7c,5,3,4}),96,au,pts));
  assert(r.packet(pkt(2,2,true,{0x7c,0x45,5,6}),96,au,pts) && au==Bytes({0,0,0,1,0x65,1,2,3,4,5,6}));
  assert(r.packet(pkt(3,3,true,{24,0,2,0x67,1,0,2,0x68,2}),96,au,pts) && au==Bytes({0,0,0,1,0x67,1,0,0,0,1,0x68,2}));
  assert(!r.packet(pkt(4,4,false,{0x7c,0x81,1}),96,au,pts));
  assert(!r.packet(pkt(6,4,true,{0x7c,0x41,3}),96,au,pts)); // lost middle fragment
  assert(!r.packet(pkt(7,5,true,{24,0,255,0x65}),96,au,pts));
  assert(!r.packet(pkt(8,6,true,{0x7c,0x45,1}),96,au,pts)); // orphan end
  auto p=pkt(9,7,true,{0x65,0xaa});p[0]=0xb1;p.insert(p.begin()+12,{0,0,0,9,1,0,0,1,0,0,0,8});p.insert(p.end(),{0,2});
  assert(r.packet(p,96,au,pts)&&au==Bytes({0,0,0,1,0x65,0xaa})); // CSRC, header extension and padding
  assert(!r.packet(Bytes(11),96,au,pts));
  std::cout<<"PASS: AVC SDP selection, credential origin, single NAL/STAP-A/FU-A, RTP extensions/padding, loss recovery\n";
}
