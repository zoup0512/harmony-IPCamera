// Included by camera_resize_test.cpp to share its CameraKit/encoder mocks.
// The capture double models independent primary/secondary arrivals. Exercise
// production CameraStreamer, not the separate pixel-layout/concurrency tests.
static int dualAttempts, dualStartsToFail, dualWaitsToFail;
static bool secondaryArrived;
namespace ipcam {
struct DualCameraCapture::State {};
DualCameraCapture::DualCameraCapture() = default;
DualCameraCapture::~DualCameraCapture() { assert(!dualProducers && !dualMockHook); }
bool DualCameraCapture::Start(const DualCameraPlan& p, int layout, uint64_t surface, int rotation) {
  assert(!dualProducers && dualMockHook && OsdPipeline::feed);
  assert(surface==2 && rotation==90 && live && live->prepared);
  assert(p.primaryId=="wide" && p.secondaryId=="selfie" && p.primaryType==0 && p.secondaryType==1);
  assert(p.width==8 && p.height==4 && p.secondaryWidth==4 && p.secondaryHeight==4);
  assert(layout>=1 && layout<=3);
  ++dualAttempts;
  dualProducers=true;  // Even partial-start failure must stop producers.
  compositeReady=secondaryArrived=false;
  if (dualStartsToFail) { --dualStartsToFail; return false; }
  return true;
}
bool DualCameraCapture::Composite(uint8_t* frame, size_t bytes, int w, int h) {
  assert(dualProducers && w==4 && h==8 && bytes==48);
  if (!secondaryArrived) return false;
  frame[0]=0x42;
  return compositeReady=true;
}
bool DualCameraCapture::WaitForComposite(std::chrono::milliseconds timeout) {
  assert(timeout.count()>0 && !compositeReady);
  uint8_t frame[48]{};
  assert(!dualMockHook(frame, sizeof(frame), 4, 8));
  if (dualWaitsToFail) { --dualWaitsToFail; return false; }
  secondaryArrived=true;
  assert(dualMockHook(frame, sizeof(frame), 4, 8));
  OsdPipeline::feed(frame, sizeof(frame));
  return compositeReady;
}
void DualCameraCapture::Stop() { dualProducers=compositeReady=secondaryArrived=false; }
} // namespace ipcam

static void TestDualIntegration(bool singleOsd) {
  using namespace ipcam;
  using R=CameraStreamer::ResizeResult;
  CameraStreamer camera;
  expectedOsd=singleOsd;
  expectedMime="video/avc";
  hd.size={8,4};  // Tightly-packed upright mock frame fits in OH_AVBuffer.
  const DualCameraPlan plan{"wide","selfie",0,1,8,4,4,4};
  int errors=0, frames=0;
  camera.SetVideoFormatCallback([](int w,int h){ formats.emplace_back(w,h); });
  assert(camera.SetDualLayout(1,&plan)==R::Stopped);
  assert(camera.Start(8,4,1876000,"video/avc",false,1234,74,singleOsd,
    [&](const uint8_t*,size_t,int64_t){ ++frames; }, [&](const std::string&){ ++errors; },
    "wide","hw.enc",AVC_PROFILE_HIGH));
  assert(camera.SetDualLayout(0,nullptr)==R::Unchanged);
  assert(camera.SetDualLayout(1,nullptr)==R::Rejected && camera.SetDualLayout(4,&plan)==R::Rejected);
  auto invalid=plan; invalid.primaryId="tele";
  assert(camera.SetDualLayout(1,&invalid)==R::Rejected);
  invalid=plan; invalid.width=2;
  assert(camera.SetDualLayout(1,&invalid)==R::Rejected && starts==1 && !dualAttempts);

  dualStartsToFail=1;
  assert(camera.SetDualLayout(1,&plan)==R::Restored && !camera.DualLayout() && ui=="74");
  int before=starts;
  dualWaitsToFail=1;
  assert(camera.SetDualLayout(1,&plan)==R::Restored && starts==before+1 && !dualMockHook);
  assert(formats.back()==(singleOsd?Size(4,8):Size(8,4)) && errors==2);
  for (int layout=1;layout<=3;++layout) {
    namedEncoder.clear();
    assert(camera.SetDualLayout(layout,layout==1?&plan:nullptr)==R::Changed);
    assert(camera.DualLayout()==layout && formats.back()==Size(4,8));
    assert(camera.CaptureWidth()==8 && camera.CaptureHeight()==4 && !camera.Orientation());
    assert(namedEncoder=="hw.enc" && live->fmt.value[OH_MD_KEY_PROFILE]==AVC_PROFILE_HIGH);
    before=starts;
    assert(camera.SetDualLayout(layout,&plan)==R::Unchanged);
    assert(!camera.SwitchFacing() && camera.ChangeResolution(8,4)==R::Rejected);
    assert(camera.ChangeResolution(640,480)==R::Rejected && starts==before);
  }
  // A stale secondary must NOT repeat the previous composition or lose the
  // codec input index. Hold it past the single path's old 200ms fallback.
  auto waitForFresh = [] {
    waitingInput=std::async(std::launch::async, [c=live] {
      OH_AVBuffer b;
      c->cb.onNeedInputBuffer(c,0,&b,c->ctx);
    });
    assert(waitingInput.wait_for(std::chrono::milliseconds(300))==std::future_status::timeout);
  };
  const int pushedBefore=pushed;
  waitForFresh();
  uint8_t latest[48]{};
  assert(dualMockHook(latest,sizeof(latest),4,8));
  OsdPipeline::feed(latest,sizeof(latest));
  waitingInput.get();
  assert(pushed==pushedBefore+1);
  waitForFresh();  // The upcoming transition must wake this callback before codec destruction.
  assert(camera.AttachPreview(85));
  dualWaitsToFail=1;
  assert(camera.SetDualLayout(2,&plan)==R::Restored && camera.DualLayout()==3 && ui=="85");
  commitsToFail=1;
  assert(camera.SetDualLayout(0,nullptr)==R::Restored && camera.DualLayout()==3 && ui=="85");
  assert(camera.SetDualLayout(0,nullptr)==R::Changed && !camera.DualLayout());
  assert(formats.back()==(singleOsd?Size(4,8):Size(8,4)) && opened=="wide" && ui=="85");
  assert(!dualMockHook && !dualProducers && (OsdPipeline::feed!=nullptr)==singleOsd);
  assert(camera.SetDualLayout(1,&plan)==R::Changed);
  dualWaitsToFail=2;
  assert(camera.SetDualLayout(2,&plan)==R::Restored && camera.DualLayout()==0);
  assert(opened=="wide" && ui=="85" && !dualProducers && !dualMockHook && !dualWaitsToFail);
  assert(formats.back()==(singleOsd?Size(4,8):Size(8,4)));
  dualWaitsToFail=1; commitsToFail=1;
  assert(camera.SetDualLayout(1,&plan)==R::Stopped && !camera.IsRunning() && !camera.DualLayout());
  assert(!dualMockHook && !dualProducers && !live && !windows && !outstandingLists && frames==starts);
}
