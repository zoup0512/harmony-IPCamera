// Host: compile C++17 with entry/src/main/cpp/dual_camera_layout.cpp (-pthread on POSIX).
#include "../entry/src/main/cpp/dual_camera_layout.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <future>
#include <thread>
using ipcam::DualFrameGate;
using namespace std::chrono_literals;
namespace {
std::vector<uint8_t> frame(int w, int h, int y, int u, int v) {
  std::vector<uint8_t> f(w*h*3/2, y);
  for (int i = w*h; i < static_cast<int>(f.size()); i+=2) { f[i]=u; f[i+1]=v; }
  return f;
}
void layouts() {
  constexpr int w=24, h=24;
  for (int mode=1; mode <= 3; ++mode) {
    DualFrameGate gate;
    assert(gate.Configure(mode,w,h,8,8,0));
    auto main = frame(w,h,81,95,120), second = frame(8,8,132,150,195);
    auto original = main;
    assert(!gate.Composite(main.data(),main.size(),w,h));
    gate.Publish(second.data(),second.size()-1,8,8);
    assert(!gate.Composite(main.data(),main.size(),w,h));
    assert(main == original);
    gate.Publish(second.data(),second.size(),8,8);
    std::fill(second.begin(),second.end(),0); // borrowed camera buffer was released
    assert(!gate.Wait(1ms)); // a secondary frame alone is not enough
    assert(!gate.Composite(main.data(),main.size(),h/2,w));
    assert(gate.Composite(main.data(),main.size(),w,h));
    assert(gate.Wait(1ms));
    auto sample=[&](int x,int y,int Y,int U,int V){
      assert(main[y*w+x] == Y);
      int off=w*h+y/2*w+(x/2)*2;
      assert(main[off] == U && main[off+1] == V);
    };
    if(mode==1){sample(0,0,81,95,120); sample(20,2,132,150,195); sample(22,20,81,95,120);}
    if(mode==2){sample(4,10,81,95,120); sample(18,10,132,150,195); sample(4,0,16,128,128);}
    if(mode==3){sample(10,4,81,95,120); sample(10,20,132,150,195); sample(0,0,16,128,128);}
    gate.Close();
    assert(!gate.Wait(0ms) && !gate.Composite(main.data(),main.size(),w,h));
  }
}
void rotation() {
  // Four differently coloured luma/chroma quadrants, not just a solid box.
  for(int rot: {0,90,180,270}) {
    DualFrameGate gate;
    assert(gate.Configure(1,24,24,8,8,rot));
    auto a=frame(24,24,16,128,128), b=frame(8,8,0,0,0);
    for(int y=0;y<8;++y)for(int x=0;x<8;++x){
      int id=(y/4)*2+x/4;
      b[y*8+x]=static_cast<uint8_t>(41+id);
      size_t at=64+(y/2)*8+x/2*2;
      b[at]=static_cast<uint8_t>(81+id); b[at+1]=static_cast<uint8_t>(141+id);
    }
    gate.Publish(b.data(),b.size(),8,8);
    assert(gate.Composite(a.data(),a.size(),24,24));
    int expected= rot==0?0:rot==90?2:rot==180?3:1;
    assert(a[16]==41+expected && a[24*24+16]==81+expected && a[24*24+17]==141+expected);
  }
}
void oddSplitAndGuards() {
  for (int mode : {1,2,3}) for (int rot : {0,90,180,270}) {
    // Width and height are even but their halves are odd: no half-UV overwrite.
    DualFrameGate gate;
    assert(gate.Configure(mode,26,22,14,10,rot));
    auto f=frame(26,22,42,91,118), b=frame(14,10,142,144,192);
    std::vector<uint8_t> guarded(f.size()+32,233);
    std::copy(f.begin(),f.end(),guarded.begin()+16);
    gate.Publish(b.data(),b.size(),14,10);
    assert(gate.Composite(guarded.data()+16,f.size(),26,22));
    assert(std::all_of(guarded.begin(),guarded.begin()+16,[](uint8_t v){return v==233;}));
    assert(std::all_of(guarded.end()-16,guarded.end(),[](uint8_t v){return v==233;}));
    assert(std::find(guarded.begin()+16,guarded.begin()+16+26*22,142)!=guarded.begin()+16+26*22);
    for(size_t i=16+26*22;i<guarded.size()-16;i+=2){
      const auto u=guarded[i],v=guarded[i+1];
      assert((u==91 && v==118) || (u==144 && v==192) || (u==128 && v==128));
    }
  }
}
void races() {
  for (int attempt=0;attempt<100;++attempt) {
    DualFrameGate gate;
    assert(gate.Configure(2,26,22,8,8,270));
    auto a=frame(26,22,40,80,120), b=frame(8,8,140,180,220);
    auto pending=std::async(std::launch::async,[&] {return gate.Wait(3s);});
    if (attempt%2) {
      std::atomic<bool> quit{false};
      std::thread secondary([&]{while(!quit.load()){gate.Publish(b.data(),b.size(),8,8); std::this_thread::yield();}});
      std::thread primary([&]{while(!quit.load()){gate.Composite(a.data(),a.size(),26,22); std::this_thread::yield();}});
      assert(pending.wait_for(2s)==std::future_status::ready && pending.get());
      gate.Close(); // simultaneous worker callbacks must never revive this run
      quit=true; secondary.join(); primary.join();
    } else {
      gate.Close(); // cancel an attempted run before either camera has a frame
      assert(pending.wait_for(2s)==std::future_status::ready && !pending.get());
    }
    assert(!gate.Configure(1,26,22,8,8,0));
    gate.Publish(b.data(),b.size(),8,8);
    assert(!gate.Composite(a.data(),a.size(),26,22));
  }
}
}
void staleSecondary() {
  // Once running, a stalled secondary camera must not stop primary frames.
  for (int mode=1; mode <= 3; ++mode) {
    DualFrameGate gate;
    assert(gate.Configure(mode,24,24,8,8,0));
    auto second = frame(8,8,132,150,195);
    gate.Publish(second.data(),second.size(),8,8);
    auto first = frame(24,24,81,95,120);
    assert(gate.Composite(first.data(),first.size(),24,24));
    std::this_thread::sleep_for(1100ms);
    auto main = frame(24,24,81,95,120);
    assert(gate.Composite(main.data(),main.size(),24,24));
    assert(std::find(main.begin(),main.begin()+24*24,132)==main.begin()+24*24);  // no stale PiP pixels
    if (mode==1) assert(main == frame(24,24,81,95,120));
    gate.Publish(second.data(),second.size(),8,8);  // recovery redraws the secondary
    assert(gate.Composite(main.data(),main.size(),24,24));
    assert(std::find(main.begin(),main.begin()+24*24,132)!=main.begin()+24*24);
  }
  // Before the first composite, staleness still fails startup instead of hiding a dead lens.
  DualFrameGate gate;
  assert(gate.Configure(1,24,24,8,8,0));
  auto second = frame(8,8,132,150,195), main = frame(24,24,81,95,120);
  gate.Publish(second.data(),second.size(),8,8);
  std::this_thread::sleep_for(1100ms);
  assert(!gate.Composite(main.data(),main.size(),24,24) && !gate.Wait(0ms));
}
int main() {
  DualFrameGate bad;
  assert(!bad.Configure(0,8,8,8,8,0)); assert(!bad.Configure(1,8,8,7,8,0));
  assert(!bad.Configure(1,4096,4096,8,8,0)); assert(!bad.Configure(1,8,8,8,8,45));
  layouts(); rotation(); oddSplitAndGuards(); races(); staleSecondary();
}
