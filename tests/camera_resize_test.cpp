// Built by camera_resize_test.py. Replace hardware only; compile both actual control implementations.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

#define OH_LOG_Print(...) ((void)0)
#define IPCAMERA_MEDIA_TIME_H
namespace ipcam { using TimestampUs = int64_t; }
using Size = std::pair<int,int>;
using Sizes = std::vector<Size>;
using Camera_ErrorCode = int;
using Camera_TorchMode = int;
enum { CAMERA_OK, CAMERA_INVALID_ARGUMENT, CAMERA_POSITION_BACK, CAMERA_POSITION_FRONT,
       CAMERA_FORMAT_YUV_420_SP };
struct Camera_Manager {};
struct Camera_Input {};
enum Camera_FocusMode { FOCUS_MODE_MANUAL, FOCUS_MODE_CONTINUOUS_AUTO, FOCUS_MODE_AUTO, FOCUS_MODE_LOCKED };
struct Camera_CaptureSession { Camera_FocusMode active = FOCUS_MODE_CONTINUOUS_AUTO; };
struct Camera_Point { double x,y; };
static bool rejectFocus;
static Camera_Point lastPoint{};
int OH_CaptureSession_IsFocusModeSupported(Camera_CaptureSession*,Camera_FocusMode mode,bool* supported) {
  *supported = mode == FOCUS_MODE_AUTO || mode == FOCUS_MODE_CONTINUOUS_AUTO; return 0;
}
int OH_CaptureSession_GetFocusMode(Camera_CaptureSession* s,Camera_FocusMode* mode) { *mode=s->active; return 0; }
int OH_CaptureSession_SetFocusMode(Camera_CaptureSession* s,Camera_FocusMode mode) {
  if (rejectFocus) return 1;
  s->active=mode; return 0;
}
int OH_CaptureSession_SetFocusPoint(Camera_CaptureSession*,Camera_Point p) {
  if (rejectFocus) return 1;
  lastPoint=p; return 0;
}
struct Camera_PreviewOutput { std::string id; };
struct Camera_Profile { int format; struct { uint32_t width, height; } size; };
struct Camera_Device { const char* cameraId; int cameraPosition; };
struct Camera_OutputCapability { Camera_Profile** previewProfiles; unsigned previewProfilesSize; };
struct Camera_FrameRateRange { unsigned min,max; };
static Camera_Profile hd{CAMERA_FORMAT_YUV_420_SP,{1280,720}}, sd{CAMERA_FORMAT_YUV_420_SP,{640,480}},
    fhd{CAMERA_FORMAT_YUV_420_SP,{1920,1080}}, odd{CAMERA_FORMAT_YUV_420_SP,{333,222}}, hdr{99,{4000,3000}};
static Camera_Profile *rear[]{&hd,&odd,nullptr,&sd,&hdr,&hd}, *front[]{&fhd,&hd}, *secondary[]{&fhd};
static Camera_Device devices[]{{"wide",CAMERA_POSITION_BACK},{"tele",CAMERA_POSITION_BACK},{"selfie",CAMERA_POSITION_FRONT}};
static std::string opened, ui;
static int outstandingLists, commitsToFail, osdStartsToFail, uiStartsToFail, windows;
static bool enumerateFails;
int OH_Camera_GetCameraManager(Camera_Manager** m) { *m=new Camera_Manager; return 0; }
void OH_Camera_DeleteCameraManager(Camera_Manager* m) { assert(!outstandingLists); delete m; }
int OH_CameraManager_GetSupportedCameras(Camera_Manager*,Camera_Device** d,uint32_t* n) {
  if (enumerateFails) return 1;
  *d=devices; *n=3; ++outstandingLists; return 0;
}
void OH_CameraManager_DeleteSupportedCameras(Camera_Manager*,Camera_Device*,unsigned) { --outstandingLists; }
int OH_CameraManager_GetSupportedCameraOutputCapability(Camera_Manager*,const Camera_Device* d,Camera_OutputCapability** c) {
  ++outstandingLists;
  std::string id=d->cameraId;
  *c=new Camera_OutputCapability(id=="wide" ? Camera_OutputCapability{rear,6} :
                                id=="selfie" ? Camera_OutputCapability{front,2} : Camera_OutputCapability{secondary,1});
  return 0;
}
void OH_CameraManager_DeleteSupportedCameraOutputCapability(Camera_Manager*,Camera_OutputCapability* c) { --outstandingLists; delete c; }
int OH_CameraManager_CreateCameraInput(Camera_Manager*,const Camera_Device* d,Camera_Input** p) { opened=d->cameraId; *p=new Camera_Input; return 0; }
int OH_CameraInput_Open(Camera_Input*) { return 0; }
void OH_CameraInput_Release(Camera_Input* p) { delete p; }
int OH_CameraManager_CreateCaptureSession(Camera_Manager*,Camera_CaptureSession** s) { *s=new Camera_CaptureSession; return 0; }
int OH_CaptureSession_BeginConfig(Camera_CaptureSession*) { return 0; }
int OH_CaptureSession_AddInput(Camera_CaptureSession*,Camera_Input*) { return 0; }
int OH_CaptureSession_AddPreviewOutput(Camera_CaptureSession*,Camera_PreviewOutput*) { return 0; }
int OH_CaptureSession_CommitConfig(Camera_CaptureSession*) { if (commitsToFail) { --commitsToFail; return 1; } return 0; }
void OH_CaptureSession_Release(Camera_CaptureSession* s) { delete s; }
int OH_CameraManager_CreatePreviewOutput(Camera_Manager*,const Camera_Profile*,const char* id,Camera_PreviewOutput** p) {
  *p=new Camera_PreviewOutput{id}; return 0;
}
int OH_PreviewOutput_Start(Camera_PreviewOutput* p) {
  if (p->id!="1" && p->id!="2") { ui=p->id; if (uiStartsToFail) { --uiStartsToFail; return 1; } }
  return 0;
}
void OH_PreviewOutput_Stop(Camera_PreviewOutput*) {}
void OH_PreviewOutput_Release(Camera_PreviewOutput* p) { delete p; }
int OH_CameraManager_SetTorchMode(Camera_Manager*,int) { return 0; }
static Camera_FrameRateRange activeRate{15,30};
int OH_PreviewOutput_GetSupportedFrameRates(Camera_PreviewOutput*,Camera_FrameRateRange** r,uint32_t* n) {
  static Camera_FrameRateRange rates[]={{15,30},{30,30},{0,80},{15,30}};
  *r=rates; *n=4; ++outstandingLists; return 0;
}
void OH_PreviewOutput_DeleteFrameRates(Camera_PreviewOutput*,Camera_FrameRateRange*) { --outstandingLists; }
int OH_PreviewOutput_GetActiveFrameRate(Camera_PreviewOutput*,Camera_FrameRateRange* r) { *r=activeRate; return 0; }
int OH_PreviewOutput_SetFrameRate(Camera_PreviewOutput*,int min,int max) { activeRate={unsigned(min),unsigned(max)}; return 0; }

struct OHNativeWindow {};
int OH_NativeWindow_GetSurfaceId(OHNativeWindow*,uint64_t* id) { *id=1; return 0; }
void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow* w) { delete w; --windows; }
using OH_AVErrCode=int;
enum { AV_ERR_OK, AVCODEC_BUFFER_FLAGS_EOS, AVCODEC_BUFFER_FLAGS_NONE, OH_MD_KEY_WIDTH,
       OH_MD_KEY_HEIGHT, OH_MD_KEY_BITRATE, OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE, BITRATE_MODE_CBR,
       OH_MD_KEY_FRAME_RATE, OH_MD_KEY_I_FRAME_INTERVAL, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12, OH_MD_KEY_PROFILE };
enum { AVC_PROFILE_BASELINE=0, AVC_PROFILE_HIGH=4, AVC_PROFILE_MAIN=8 };
enum { OH_AVCODEC_TYPE_VIDEO_ENCODER };
struct OH_AVCapability { const char* name; bool hardware; };
struct OH_AVRange { int minVal,maxVal; };
static OH_AVCapability hw{"hw.enc",true}, sw{"sw.enc",false}, secure{"secure",true}, hevc{"hw.hevc",true};
OH_AVCapability** OH_AVCodec_GetCapabilityList(int type,uint32_t* count) {
  static OH_AVCapability* all[]{nullptr,&hw,&sw,&hw,&secure,&hevc};
  assert(type==OH_AVCODEC_TYPE_VIDEO_ENCODER); *count=6; return all;
}
bool OH_AVCapability_IsSecure(OH_AVCapability* cap) { return cap==&secure; }
bool OH_AVCapability_CheckMimeType(OH_AVCapability* cap,const char* mime) {
  return std::string(mime)==(cap==&hevc?"video/hevc":"video/avc");
}
OH_AVCapability* OH_AVCodec_GetCapability(const char*,bool enc) { assert(enc); return &hw; }
bool OH_AVCapability_IsHardware(OH_AVCapability* c) { return c->hardware; }
const char* OH_AVCapability_GetName(OH_AVCapability* c) { return c->name; }
int OH_AVCapability_GetSupportedProfiles(OH_AVCapability* c,const int32_t** p,uint32_t* n) {
  static int32_t h[]{AVC_PROFILE_BASELINE,AVC_PROFILE_HIGH,AVC_PROFILE_HIGH,123}, s[]{AVC_PROFILE_MAIN};
  *p=c==&hw?h:s; *n=c==&hw?4:1; return 0;
}
int OH_AVCapability_GetEncoderBitrateRange(OH_AVCapability*,OH_AVRange* r) { *r={1000,8000000}; return 0; }
bool OH_AVCapability_IsEncoderBitrateModeSupported(OH_AVCapability* c,int mode) { return c==&hw && mode==BITRATE_MODE_CBR; }
struct OH_AVFormat { int value[16]{}; };
OH_AVFormat* OH_AVFormat_Create() { return new OH_AVFormat; }
void OH_AVFormat_Destroy(OH_AVFormat* f) { delete f; }
bool OH_AVFormat_SetIntValue(OH_AVFormat* f,int k,int v) { assert(k!=OH_MD_KEY_BITRATE); f->value[k]=v; return true; }
bool OH_AVFormat_SetLongValue(OH_AVFormat* f,int k,int64_t v) { assert(k==OH_MD_KEY_BITRATE); f->value[k]=int(v); return true; }
void OH_AVFormat_SetDoubleValue(OH_AVFormat* f,int k,double v) { f->value[k]=int(v); }
struct OH_AVCodecBufferAttr { int flags=0,size=1; int64_t pts=1; };
struct OH_AVBuffer { uint8_t bytes[48]{}; OH_AVCodecBufferAttr attr; };
uint8_t* OH_AVBuffer_GetAddr(OH_AVBuffer* b) { return b->bytes; }
int OH_AVBuffer_GetCapacity(OH_AVBuffer* b) { return sizeof(b->bytes); }
void OH_AVBuffer_GetBufferAttr(OH_AVBuffer* b,OH_AVCodecBufferAttr* a) { *a=b->attr; }
void OH_AVBuffer_SetBufferAttr(OH_AVBuffer* b,OH_AVCodecBufferAttr* a) { b->attr=*a; }
struct OH_AVCodec;
struct OH_AVCodecCallback {
  void (*onError)(OH_AVCodec*,int32_t,void*);
  void (*onStreamChanged)(OH_AVCodec*,OH_AVFormat*,void*);
  void (*onNeedInputBuffer)(OH_AVCodec*,uint32_t,OH_AVBuffer*,void*);
  void (*onNewOutputBuffer)(OH_AVCodec*,uint32_t,OH_AVBuffer*,void*);
};
struct OH_AVCodec { OH_AVCodecCallback cb; void* ctx; std::string mime; OH_AVFormat fmt; bool prepared=false; };
static OH_AVCodec* live;
static std::string expectedMime;
static bool expectedOsd;
static std::function<bool(uint8_t*,size_t,int,int)> dualMockHook;
static bool dualProducers, compositeReady;
static int starts, destroys, pushed;
static Sizes formats;
OH_AVCodec* OH_VideoEncoder_CreateByMime(const char* mime) { assert(!live); live=new OH_AVCodec{}; live->mime=mime; return live; }
static std::string namedEncoder;
OH_AVCodec* OH_VideoEncoder_CreateByName(const char* name) {
  namedEncoder=name; return OH_VideoEncoder_CreateByMime(expectedMime.c_str());
}
int OH_VideoEncoder_RegisterCallback(OH_AVCodec* c,OH_AVCodecCallback cb,void* ctx) { c->cb=cb; c->ctx=ctx; return 0; }
int OH_VideoEncoder_Configure(OH_AVCodec* c,OH_AVFormat* f) { c->fmt=*f; return 0; }
int OH_VideoEncoder_GetSurface(OH_AVCodec* c,OHNativeWindow** w) { assert(!c->prepared); *w=new OHNativeWindow; ++windows; return 0; }
int OH_VideoEncoder_Prepare(OH_AVCodec* c) { c->prepared=true; return 0; }
void OH_VideoEncoder_PushInputBuffer(OH_AVCodec*,unsigned) { ++pushed; }
void OH_VideoEncoder_FreeOutputBuffer(OH_AVCodec*,unsigned) {}
static std::future<void> waitingInput;
void OH_VideoEncoder_Stop(OH_AVCodec*) { if (waitingInput.valid()) waitingInput.get(); }
void OH_VideoEncoder_Destroy(OH_AVCodec* c) { ++destroys; assert(c==live); live=nullptr; delete c; }
int OH_VideoEncoder_Start(OH_AVCodec* c) {
  ++starts;
  assert(formats.size()==static_cast<size_t>(starts));
  assert(formats.back()==Size(c->fmt.value[OH_MD_KEY_WIDTH],c->fmt.value[OH_MD_KEY_HEIGHT]));
  assert(c->mime==expectedMime && c->fmt.value[OH_MD_KEY_BITRATE]==1876000 && c->fmt.value[OH_MD_KEY_I_FRAME_INTERVAL]==1234);
  assert((c->fmt.value[OH_MD_KEY_PIXEL_FORMAT]==AV_PIXEL_FORMAT_NV12)==(expectedOsd || dualMockHook));
  assert(c->fmt.value[OH_MD_KEY_FRAME_RATE]==30 && c->fmt.value[OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE]==BITRATE_MODE_CBR);
  // Codec-owned input buffers outlive the callback; a queued index may be filled later.
  static OH_AVBuffer inputs[64];
  OH_AVBuffer& b=inputs[starts%64];
  b=OH_AVBuffer{};
  if (dualMockHook) {
    assert(compositeReady && dualProducers);
    const int before=pushed;
    c->cb.onNeedInputBuffer(c,0,&b,c->ctx);
    assert(pushed==before+1 && b.attr.size==48 && b.bytes[0]==0x42);
  } else if (expectedOsd) {
    // Before the camera has delivered a frame the index is queued, not pushed or lost.
    int before=pushed; c->cb.onNeedInputBuffer(c,0,&b,c->ctx); assert(pushed==before);
  }
  OH_AVBuffer out;
  out.bytes[0]=static_cast<uint8_t>(starts);  // epoch marker, NOT a decodable bitstream
  c->cb.onNewOutputBuffer(c,0,&out,c->ctx);
  return 0;
}

#include "capture_orientation.h"
#define IPCAMERA_OSD_PIPELINE_H
namespace ipcam {
struct OsdPipeline {
  using Sink=std::function<void(const uint8_t*,size_t)>;
  static inline Sink feed;
  int w=0,h=0,angle=0;
  bool Start(int width,int height,uint64_t id,uint64_t* surface,Sink sink,std::function<void(const std::string&)>,
             CaptureOrientation orientation = {}) {
    if (osdStartsToFail) { --osdStartsToFail; return false; }
    angle=orientation.Resolve(90); w=angle%180?height:width; h=angle%180?width:height;
    ui=std::to_string(id); *surface=2; feed=sink; return true;
  }
  int rotation() const { return angle; } int OutWidth() const { return w; } int OutHeight() const { return h; }
  bool SetUiSurface(uint64_t id) { ui=std::to_string(id); return true; }
  void SetPreOverlay(decltype(dualMockHook) cb) { assert(!feed); dualMockHook=std::move(cb); }
  void ClearUiSurface() { ui.clear(); }
  void Stop() { assert(!dualProducers); feed=nullptr; dualMockHook={}; }
};
}
#include "camera_streamer.cpp"
#include "camera_capabilities.cpp"
#include "camera_dual_integration_cases.h"

static void TestCameraSettings() {
  using namespace ipcam;
  expectedMime="video/avc";
  CameraStreamer camera;
  camera.SetVideoFormatCallback([](int w,int h){formats.emplace_back(w,h);});
  assert(CameraDevicesJson()==R"([{"id":"wide","front":false},{"id":"tele","front":false},{"id":"selfie","front":true}])");
  devices[1].cameraId="t\"e\\l\n";
  assert(CameraDevicesJson().find(R"(t\"e\\l\u000a)")!=std::string::npos);
  devices[1].cameraId="tele";
  assert(CameraResolutionsJson(false,"tele")==R"([{"width":1920,"height":1080}])");
  assert(CameraResolutionsJson(true,"tele")=="[]" && CameraResolutionsJson(false,"gone")=="[]");
  assert(EncoderOptionsJson("video/avc")==R"([{"name":"hw.enc","hardware":true,"minBitrate":1000,"maxBitrate":8000000,"profiles":[0,4]},{"name":"sw.enc","hardware":false,"minBitrate":1000,"maxBitrate":8000000,"profiles":[8]}])");
  assert(EncoderOptionsJson("other")=="[]" && EncoderOptionsJson("video/hevc").find("sw.enc")==std::string::npos);
  bool cbr=true;
  assert(CheckEncoderChoice("video/avc","sw.enc",8,2000000,cbr) && !cbr);
  assert(!CheckEncoderChoice("video/hevc","hw.enc",0,2000000,cbr));
  assert(!CheckEncoderChoice("video/avc","",0,2000000,cbr));
  auto start=[&](std::string id,std::string name,int profile,int bitrate=1876000) {
    return camera.Start(1920,1080,bitrate,"video/avc",false,1234,0,false,
      [](const uint8_t*,size_t,int64_t){},[](const std::string&){},id,name,profile);
  };
  assert(!start("wide","hw.enc",4) && !start("tele","unavailable",4));
  assert(!start("tele","hw.enc",8) && !start("tele","hw.enc",4,9000000));
  assert(starts==0 && namedEncoder.empty() && !outstandingLists);
  assert(camera.FocusInfoJson()==R"({"modes":[]})" && !camera.SetFocusMode(2));
  assert(start("tele","hw.enc",4) && opened=="tele" && namedEncoder=="hw.enc");
  assert(live->fmt.value[OH_MD_KEY_PROFILE]==AVC_PROFILE_HIGH);
  assert(camera.FocusInfoJson()==R"({"modes":[1,2],"active":1})");
  assert(!camera.FocusAt(.5,.5) && !camera.SetFocusMode(100) && !camera.SetFocusMode(0));
  rejectFocus=true; assert(!camera.SetFocusMode(2)); rejectFocus=false;
  assert(camera.SetFocusMode(2) && camera.FocusInfoJson()==R"({"modes":[1,2],"active":2})");
  assert(!camera.FocusAt(-.01,.5) && !camera.FocusAt(.5,std::numeric_limits<double>::quiet_NaN()));
  rejectFocus=true; assert(!camera.FocusAt(.5,.5)); rejectFocus=false;
  assert(camera.FocusAt(.25,.7) && lastPoint.x==.25 && lastPoint.y==.7);
  assert(camera.CaptureFpsJson()==R"({"ranges":[{"min":15,"max":30},{"min":30,"max":30}],"active":{"min":15,"max":30}})");
  assert(!camera.SetCaptureFps(60,60) && camera.SetCaptureFps(30,30) && activeRate.min==30);
  // A rebuild opens a new session/output with device defaults; user choices are re-applied.
  activeRate={15,30};
  assert(camera.Restart() && activeRate.min==30 && activeRate.max==30);
  assert(camera.FocusInfoJson()==R"({"modes":[1,2],"active":2})");
  camera.Stop(); assert(!camera.FocusAt(.5,.5) && !outstandingLists && !live);
}

static void TestCaptureOrientation() {
  using namespace ipcam;
  expectedMime="video/avc";
  CameraStreamer c;
  c.SetVideoFormatCallback([](int w,int h){formats.emplace_back(w,h);});
  auto start=[&](CaptureRotation r,bool mirror,bool text=false) {
    return c.Start(1280,720,1876000,"video/avc",false,1234,74,text,{}, {}, {}, {}, -1,{r,mirror});
  };
  assert(!start(static_cast<CaptureRotation>(1),false) && !live);
  expectedOsd=true;
  assert(start(CaptureRotation::Auto,false,true) && c.Orientation()==0);
  c.Stop();  // Auto fixture original rotation = 90, retained for raw metadata.
  for (auto r : {CaptureRotation::Deg0,CaptureRotation::Deg90,CaptureRotation::Deg180,CaptureRotation::Deg270}) {
    const bool swapped=static_cast<int>(r)%180!=0;
    assert(start(r,false) && c.Orientation()==0 && c.CaptureWidth()==1280 && c.CaptureHeight()==720);
    assert(formats.back()==(swapped?Size(720,1280):Size(1280,720)));
    assert(!start(CaptureRotation::Auto,false) && c.AttachPreview(75) && c.IsRunning());
    assert(c.ChangeResolution(640,480)==CameraStreamer::ResizeResult::Changed);
    assert(formats.back()==(swapped?Size(480,640):Size(640,480)) && c.Orientation()==0);
    c.Stop();
  }
  for (auto r : {CaptureRotation::Auto,CaptureRotation::Deg0}) {
    osdStartsToFail=1; int prev=starts;
    assert(!start(r,r==CaptureRotation::Auto) && !live && !c.IsRunning() && starts==prev);
  }
  expectedOsd=false;
  assert(start(CaptureRotation::Auto,false) && c.Orientation()==90 && formats.back()==Size(1280,720));
  c.Stop(); expectedOsd=true;
  assert(start(CaptureRotation::Auto,true) && c.Orientation()==0); c.Stop();
}

int main(int argc,char** argv) {
  assert(argc==2);
  if (std::string(argv[1])=="capture-orientation") { TestCaptureOrientation(); return 0; }
  if (std::string(argv[1])=="dual-surface" || std::string(argv[1])=="dual-osd") {
    TestDualIntegration(std::string(argv[1])=="dual-osd"); return 0;
  }
  if (std::string(argv[1])=="camera-settings") { TestCameraSettings(); return 0; }
  using Camera=ipcam::CameraStreamer;
  using Result=Camera::ResizeResult;
  Camera camera;
  expectedOsd=std::string(argv[1])=="osd-hevc";
  expectedMime=expectedOsd?"video/hevc":"video/avc";
  auto geometry=[&](int w,int h) { return expectedOsd?Size(h,w):Size(w,h); };
  int frames=0, errors=0;
  camera.SetVideoFormatCallback([&](int w,int h){ formats.emplace_back(w,h); });
  assert(camera.ChangeResolution(640,480)==Result::Stopped && camera.AvailableResolutions().empty());
  assert(camera.AttachPreview(74));
  assert(camera.Start(1280,720,1876000,expectedMime.c_str(),false,1234,0,expectedOsd,
    [&](const uint8_t* b,size_t n,int64_t){ assert(n==1 && b[0]==formats.size()); ++frames; },
    [&](const std::string&){ ++errors; }));
  Sizes rearSizes{{1280,720},{640,480}}, frontSizes{{1920,1080},{1280,720}};
  assert(camera.AvailableResolutions()==rearSizes && ui=="74");
  assert(ipcam::CameraResolutionsJson(false)=="[{\"width\":1280,\"height\":720},{\"width\":640,\"height\":480}]");
  std::swap(devices[0],devices[1]);  // same facing, different physical lens first now
  assert(camera.AvailableResolutions()==rearSizes);
  assert(camera.ChangeResolution(1280,720)==Result::Unchanged);
  assert(camera.ChangeResolution(333,222)==Result::Rejected);
  assert(camera.ChangeResolution(4000,3000)==Result::Rejected);
  assert(camera.ChangeResolution(1920,1080)==Result::Rejected);
  enumerateFails=true;
  assert(camera.AvailableResolutions().empty() && camera.ChangeResolution(640,480)==Result::Rejected);
  enumerateFails=false;
  assert(starts==1 && destroys==0 && errors==0 && frames==1);
  if (expectedOsd) {
    // The index queued during Start is filled by the first camera frame.
    static OH_AVBuffer b; ipcam::OsdPipeline::feed(b.bytes,1); assert(pushed==1);
    // A callback without a waiting frame returns immediately and keeps its index.
    live->cb.onNeedInputBuffer(live,0,&b,live->ctx); assert(pushed==1);
    ipcam::OsdPipeline::feed(b.bytes,1); assert(pushed==2 && b.attr.size==1);
    // No free index: this raw frame waits, and must not survive the rebuild below.
    ipcam::OsdPipeline::feed(b.bytes,1); assert(pushed==2);
  }
  assert(camera.ChangeResolution(640,480)==Result::Changed);
  assert(camera.CaptureWidth()==640 && camera.CaptureHeight()==480 && formats.back()==geometry(640,480));
  assert(opened=="wide" && ui=="74" && starts==2);
  assert(!camera.SwitchFacing() && starts==2);  // front lacks 640x480
  assert(camera.AttachPreview(85));
  if (expectedOsd) osdStartsToFail=1; else uiStartsToFail=1;
  assert(camera.ChangeResolution(1280,720)==Result::Restored);
  assert(camera.CaptureWidth()==640 && formats.back()==geometry(640,480) && opened=="wide" && ui=="85");
  live->cb.onError(live,13,live->ctx); assert(errors==2);  // error callback survived rollback
  assert(camera.ChangeResolution(1280,720)==Result::Changed);
  std::swap(devices[0],devices[1]);
  commitsToFail=1;
  assert(!camera.SwitchFacing() && opened=="wide" && camera.AvailableResolutions()==rearSizes);
  assert(camera.SwitchFacing() && opened=="selfie" && camera.AvailableResolutions()==frontSizes);
  int oldStarts=starts;
  assert(camera.ChangeResolution(640,480)==Result::Rejected && starts==oldStarts);
  commitsToFail=1;  // target emitted a callback, then failed; rollback must announce ANOTHER epoch
  assert(camera.ChangeResolution(1920,1080)==Result::Restored);
  assert(starts==oldStarts+2 && formats[oldStarts]==geometry(1920,1080) && formats.back()==geometry(1280,720));
  assert(opened=="selfie" && ui=="85" && camera.CaptureWidth()==1280 && camera.AvailableResolutions()==frontSizes);
  commitsToFail=2;
  assert(camera.ChangeResolution(1920,1080)==Result::Stopped && !camera.IsRunning());
  assert(camera.EncodedWidth()==0 && camera.EncodedHeight()==0 && camera.CaptureWidth()==0 && camera.CaptureHeight()==0);
  assert(camera.AvailableResolutions().empty() && frames==starts && !windows && !outstandingLists && !live);
}
