// Node 22.13+. Execute the actual Player, FLV loader and voice session with host doubles only.
const assert = require('node:assert/strict');
const {test} = require('node:test');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const {stripTypeScriptTypes} = require('node:module');
const root = join(__dirname, '../entry/src/main');
function load(file, name, deps = {}) {
  let s = readFileSync(join(root, 'ets', file), 'utf8').replace(/^import .*;$/gm, '').replace(/^export /gm, '');
  if (name === 'Player') s = s.slice(0, s.indexOf('  build() {')).replace(/^@(Entry|Component)\s*$/gm, '').replace('struct Player','class Player').replace(/@State /g,'') + '}';
  return new Function(...Object.keys(deps), stripTypeScriptTypes(s) + '\nreturn ' + name + ';')(...Object.values(deps));
}
const tick = () => new Promise(setImmediate);
function deferred() { let resolve; const promise = new Promise(r=>resolve=r); return {promise,resolve}; }
const util = { generateRandomUUID:()=> 'uuid', Base64Helper:class {encodeToStringSync(b){return Buffer.from(b).toString('base64');}}, TextEncoder:{create:()=>({encodeInto:s=>new TextEncoder().encode(s)})} };
const liveEndpoint = load('util/LiveEndpoint.ets','liveEndpoint',{util,url:{URL:{parseURL:s=>new URL(s)}}});
const en = new Map(['viewer_strings','viewer_controls_extra'].flatMap(file=>require(join(root,`resources/base/element/${file}.json`)).string.map(x=>[x.name,x.value])));
const ctx = {cacheDir:'/private',windowStage:{getMainWindowSync:()=>({isWindowShowing:()=>true})},resourceManager:{getStringByNameSync:k=>{assert(en.has(k));return en.get(k);}}};
const retry = require('../entry/src/main/ets/util/ViewerReconnect.ts');
const mjpegBoundary = load('util/MjpegFrames.ets','mjpegBoundary');
const MjpegFrames = load('util/MjpegFrames.ets','MjpegFrames');
function env() {
  const log=[], players=[], recorders=[], requests=[], routes=[];
  const c={grant:0, uploadCode:200, size:12};
  const http={RequestMethod:{GET:'GET',PUT:'PUT'},createHttp:()=>{
    const r={cb:{},on(k,fn){this.cb[k]=fn;},destroy(){this.destroyed=true;},async request(url,options){Object.assign(this,{url,options});return c.uploadWait ? c.uploadWait.promise : {responseCode:c.uploadCode};},requestInStream(url,options){Object.assign(this,{url,options}); this.res=deferred();return this.res.promise;}};
    requests.push(r); return r;
  }};
  const media={AudioSourceType:{AUDIO_SOURCE_TYPE_MIC:1},CodecMimeType:{AUDIO_AAC:'aac'},ContainerFormatType:{CFT_AAC:'aac',CFT_MPEG_4A:'m4a'},LoadingRequestError:{LOADING_ERROR_SUCCESS:0,LOADING_ERROR_NO_RESOURCE:2},
    async createAVPlayer(){const p={cb:{},on(k,fn){this.cb[k]=fn;},async prepare(){log.push('prepare');},setVolume(n){log.push(['vol',n]);},async play(){log.push('play');},async release(){this.released=true;},async setMediaSource(src){this.src=src;}};players.push(p);if(c.playerWait)await c.playerWait.promise;return p;},
    createMediaSourceWithUrl(...args){assert.equal(args.length,1,'no headers to AVPlayer');return{url:args[0],setMediaResourceLoaderDelegate(l){this.loader=l;}};},
    async createAVRecorder(){const r={cb:{},state:'idle',on(k,fn){this.cb[k]=fn;},async prepare(config){this.config=config;if(c.prepareWait)await c.prepareWait.promise;},async start(){log.push('record');this.state='started';},async stop(){log.push('stop-record');this.state='stopped';this.cb.stateChange('stopped');},async release(){log.push('release-record');}};recorders.push(r);return r;}};
  const fileIo={OpenMode:{CREATE:1,READ_WRITE:2,TRUNC:4},openSync:(path)=>{log.push(['open',path]);return{fd:7};},unlinkSync:p=>log.push(['unlink',p]),statSync:()=>({size:c.size}),lseek(){},readSync(fd,b){new Uint8Array(b).fill(99);return b.byteLength;},closeSync(){log.push('close');}};
  const abilityAccessCtrl={createAtManager:()=>({requestPermissionsFromUser:async(context,p)=>{log.push(['permission',...p]);return c.permissionWait?c.permissionWait.promise:{authResults:[c.grant]};}})};
  const ViewerVoice=load('util/ViewerVoice.ets','ViewerVoice',{media,http,fileIo,util,abilityAccessCtrl});
  const LiveFlvLoader=load('util/LiveFlvLoader.ets','LiveFlvLoader',{http,media});
  const timers = new Map(), native = [], handles = [];
  let nextTimer = 0;
  function start(kind,args) { const h={kind,status:0,audio:0,stopped:false}; native.push([kind,...args]); handles.push(h); return h; }
  const streaming = {
    liveAvcStart:(...a)=>start('Basic',a),liveAvcStartDigest:(...a)=>start('Digest',a),rtmpViewerStart:(...a)=>start('RTMP',a),
    liveAvcStartUdp:(...a)=>start('UDP-Basic',a),liveAvcStartDigestUdp:(...a)=>start('UDP-Digest',a),
    liveAvcStatus:h=>{assert.notEqual(h.kind,'RTMP');return h.status;},rtmpViewerStatus:h=>{assert.equal(h.kind,'RTMP');return h.status;},
    liveVideoCodec:h=>{assert.notEqual(h.kind,'RTMP');return 'video/avc';},liveAvcAudioStatus:h=>{assert(!h.kind.startsWith('UDP'));return h.audio;},rtmpViewerAudioStatus:h=>h.audio,
    liveAvcSetAudio:(h,on)=>{assert.notEqual(h.kind,'RTMP');h.audio=on?1:0;},rtmpViewerSetAudio:(h,on)=>{assert.equal(h.kind,'RTMP');h.audio=on?1:0;},
    liveAvcStop:h=>{assert.notEqual(h.kind,'RTMP');h.stopped=true;},rtmpViewerStop:h=>{assert.equal(h.kind,'RTMP');h.stopped=true;}
  };
  const image = {createImageSource:()=>({getImageInfo:async()=>({size:{width:2,height:2}}),
    createPixelMap:async()=>({release:async()=>{}}),release:async()=>{}})};
  const readDevice = async id=>{assert.equal(id,'opaque-id');return c.deviceWait.promise;};
  const windows=[], restore=new Map();
  const PiPWindow={PiPState:{STARTED:2,ABOUT_TO_RESTORE:5,STOPPED:4},PiPTemplateType:{VIDEO_LIVE:3},isPiPEnabled:()=>c.pipEnabled!==false,
    async create(config){if(c.createWait)await c.createWait.promise;const w={config,on(_,fn){this.fn=fn;},off(){this.fn=null;},
      async startPiP(){this.starts=(this.starts||0)+1;},async stopPiP(){this.stops=(this.stops||0)+1;if(c.stopWait)await c.stopWait.promise;if(c.stopFail)throw Error();},
      emit(s,reason=''){this.fn?.(PiPWindow.PiPState[s],reason);}}; windows.push(w);return w;}};
  const pipFile='util/ViewerPipSession.ets', pipDeps={PiPWindow,canIUse:()=>true};
  const bars=[],window={AvoidAreaType:{TYPE_SYSTEM:'status',TYPE_NAVIGATION_INDICATOR:'nav'},getLastWindow:async()=>({
    getWindowProperties:()=>({isLayoutFullScreen:false}),getWindowAvoidArea:bar=>({visible:bar==='status'}),
    setWindowLayoutFullScreen:async value=>bars.push(['layout',value]),setSpecificSystemBarEnabled:async (bar,value)=>bars.push([bar,value])})};
  const Page=load('pages/Player.ets','Player',{...retry,...pipDeps,window,media,http,image,ViewerVoice,LiveFlvLoader,liveEndpoint,readDevice,
    ViewerPipSession:load(pipFile,'ViewerPipSession',pipDeps),viewerPipEligible:load(pipFile,'viewerPipEligible',pipDeps),
    mjpegBoundary,MjpegFrames,XComponentController:class{getXComponentSurfaceId(){return c.controllerId??'42';}},getContext:()=>ctx,
    setInterval:()=>1,clearInterval(){},setTimeout:fn=>{restore.set(++nextTimer,fn);return nextTimer;},clearTimeout:id=>restore.delete(id),streaming});
  const p=new Page();p.ctx=ctx;p.getUIContext=()=>({getRouter:()=>({getParams:()=>c.params,pushUrl:async x=>routes.push(x)})});
  p.reconnect = new retry.ViewerReconnect({after:(task,ms)=>{assert.equal(timers.size,0);timers.set(++nextTimer,{task,ms});return nextTimer;},cancel:id=>timers.delete(id)});
  function fire() {assert.equal(timers.size,1); const [[id,t]]=timers;timers.delete(id);t.task();return t.ms;}
  p.onPageShow();
  return {c,p,log,players,recorders,requests,routes,LiveFlvLoader,timers,native,handles,fire,windows,restore,bars};
}
test('system player uses the surface, mutes before play, does not fall back to Web or leak credentials',async()=>{
  const {p,players,log,requests,recorders,routes}=env();p.address='https://cam/public/index.m3u8';p.play();await p.avWork;assert.equal(players.length,0);
  p.surfaceId='s';p.startSystem();await p.avWork;const av=players[0];assert.equal(av.src.url,p.address);av.cb.stateChange('initialized');assert.equal(av.surfaceId,'s');av.cb.stateChange('prepared');assert.deepEqual(log,['prepare',['vol',0],'play']);av.cb.stateChange('playing');p.toggleAudio();assert.deepEqual(log.at(-1),['vol',1]);
  p.address='https://u:p@cam/private.m3u8';p.play();await p.avWork;assert(av.released);assert(p.failed);av.cb.error();assert.equal(players.length,1);
  p.openArchives();assert.deepEqual(routes.at(-1),{url:'pages/RemoteArchives',params:{url:'https://cam'}});
  for(const addr of ['https://cam/live.m3u8?token=secret','http://cam/live.flv?key=x','rtsps://cam/video']){p.address=addr;p.play();await p.avWork;assert(p.failed);assert.equal(players.length,1);assert(!p.status.includes('secret'));}
  p.openArchives();assert.deepEqual(routes.at(-1).params,{});assert.equal(requests.length,0);assert.equal(recorders.length,0);
  p.address='https://cam/public.flv';p.play();await p.avWork;assert.equal(players[1].src.url,p.address);p.onPageHide();await p.avWork;assert(players[1].released);
});
test('stale create after page hide is released before it can set a source',async()=>{
  const {c,p,players}=env();c.playerWait=deferred();p.surfaceId='s';p.address='https://cam/public.m3u8';p.play();await tick();p.onPageHide();c.playerWait.resolve();await p.avWork;assert(players[0].released);assert(!players[0].src);
});
test('Basic FLV opens exactly the selected resource, rejects redirects, bounds backpressure and read offsets',async()=>{
  for(const mode of ['redirect','overflow','seek','eof']){
    const {p,players,requests}=env();p.surfaceId='s';p.address='https://u:p@cam/live.flv';p.play();await p.avWork;
    const src=players[0].src,l=src.loader,out=[],finish=[];assert.equal(src.url,'https://cam/live.flv');
    const req={url:src.url,header:{Authorization:'untrusted',Range:'bytes=12-'},respondHeader(id,h){assert.deepEqual(h,{'Content-Type':'video/x-flv'});},respondData(id,off,b){out.push([off,b.byteLength]);return out.length===1?-3:b.byteLength;},finishLoading(id,result){finish.push(result);}};
    assert(l.open({...req,url:'https://cam.bad/live.flv'})<0);assert(l.open({...req,url:'https://cam/secret'})<0);const id=l.open(req);await tick();const h=requests[0];assert.equal(h.url,src.url);assert.equal(h.options.maxRedirects,0);assert.equal(h.options.usingProxy,false);assert.deepEqual(h.options.header,{Accept:'video/x-flv',Authorization:'Basic dTpw'});
    if(mode==='redirect'){h.cb.headersReceive({Location:'https://evil/capture'});assert.match(p.status,/redirect/i);} else {
      h.cb.headersReceive({'Content-Type':'video/x-flv','Set-Cookie':'do not forward'});l.read(id,0,-1);h.cb.dataReceive(new ArrayBuffer(5));assert.deepEqual(out,[[0,5]]);
      if(mode==='overflow')h.cb.dataReceive(new ArrayBuffer(2*1024*1024));
      if(mode==='seek')l.read(id,10,5);
      if(mode==='eof'){l.read(id,0,2);l.read(id,2,-1);h.cb.dataEnd();assert.deepEqual(out,[[0,5],[0,2],[2,3]]);}
    }
    assert.deepEqual(finish,[mode==='eof'?0:2]);assert(h.destroyed);assert.equal(requests.length,1);p.stop();await p.avWork;
  }
});
test('explicit grant and send: Android M4A vs legacy Harmony ADTS; timeout alone does not upload',async()=>{
  for(const adts of [false,true]){
    const {p,log,recorders,requests}=env();p.select(liveEndpoint('https://alice:pw@camera:9443/stream/video?private=query'));p.adts=adts;assert(!log.length);p.beginVoice();p.beginVoice();await tick();const r=recorders[0];assert.equal(recorders.length,1);assert.deepEqual(log.slice(0,3),[['permission','ohos.permission.MICROPHONE'],['open','/private/talk-uuid'],['unlink','/private/talk-uuid']]);assert.equal(r.config.profile.fileFormat,adts?'aac':'m4a');assert.equal(r.config.maxDuration,adts?3:15);
    r.state='stopped';r.cb.stateChange('stopped');assert.equal(p.voicePhase,'ready');assert.equal(requests.length,0);const v=p.voice;v.send();v.send();await tick();assert.equal(requests.length,1);const req=requests[0];assert.equal(req.url,'https://camera:9443/put_voice');assert.equal(req.options.method,'PUT');assert.equal(req.options.header['Content-Type'],adts?'audio/aac':'audio/m4a');assert.equal(req.options.header.Authorization,'Basic YWxpY2U6cHc=');assert.equal(req.options.maxRedirects,0);assert.equal(req.options.usingProxy,false);assert.deepEqual([...new Uint8Array(req.options.extraData)],Array(12).fill(99));assert(req.destroyed);assert.match(p.voiceMessage,/accepted/);assert.equal(log.filter(x=>x==='close').length,1);
  }
});
test('deny, revoke, cancel, background, wrong protocol, size and HTTP failures never silently retry',async()=>{
  for(const mode of ['denied','late-grant','late-prepare','background','rtsp','oversized','redirect','bad-auth','during-upload']){
    const {c,p,log,recorders,requests}=env();p.select(liveEndpoint(mode==='rtsp'?'rtsp://cam/stream':'http://cam:8081/video'));if(mode==='denied')c.grant=-1;if(mode==='late-grant')c.permissionWait=deferred();if(mode==='late-prepare')c.prepareWait=deferred();p.beginVoice();await tick();
    if(mode==='late-grant'){p.onPageHide();c.permissionWait.resolve({authResults:[0]});}
    if(mode==='late-prepare'){p.cancelVoice();c.prepareWait.resolve();}
    if(mode==='background')p.onPageHide();
    if(['oversized','redirect','bad-auth','during-upload'].includes(mode)){if(mode==='oversized')c.size=1024*1024;if(mode==='redirect')c.uploadCode=307;if(mode==='bad-auth')c.uploadCode=401;if(mode==='during-upload')c.uploadWait=deferred();p.voice.send();}
    await tick();
    if(mode==='during-upload'){p.onPageHide();assert(requests[0].destroyed);c.uploadWait.resolve({responseCode:200});await tick();assert.equal(p.voiceMessage,'');}
    assert.equal(requests.length,['redirect','bad-auth','during-upload'].includes(mode)?1:0);if(['denied','late-grant','rtsp'].includes(mode))assert.equal(recorders.length,0);if(mode==='late-prepare')assert(!log.includes('record'));if(recorders.length)assert(log.includes('close'));if(mode==='bad-auth')assert.match(p.voiceMessage,/authentication/);if(mode==='redirect')assert.match(p.voiceMessage,/redirect/);
  }
});

test('RTMP starts only on a surface, reports actual render, uses its own audio/stop and never retries', async()=>{
  const {p,native,handles,requests,players,routes,timers}=env();
  const url='rtmp://cam:1935/live/private-key?sig=private-query';
  p.address=url;p.autoReconnect=true;p.play();await p.avWork;assert.equal(native.length,0);
  p.surfaceId='42';p.queueNative();p.queueNative();await p.avWork;
  assert.deepEqual(native,[['RTMP',url,'42']]);const h=handles[0];p.poll(p.epoch);
  assert.match(p.status,/Connecting/);assert.equal(p.endpointLabel,'rtmp://cam:1935');
  h.status=1;p.poll(p.epoch);assert.match(p.status,/Playing.*RTMP.*H.264.*Muted/);
  p.toggleAudio();assert.equal(h.audio,1);h.audio=-3;p.poll(p.epoch);assert.match(p.status,/AAC-LC only/);
  p.beginVoice();assert(!p.voice);
  h.status=-12;p.poll(p.epoch);assert.match(p.status,/HEVC.*not supported/);assert(h.stopped);
  p.openArchives();assert.deepEqual(routes.at(-1).params,{});
  assert.equal(timers.size,0);assert.equal(requests.length+players.length,0);
  assert.doesNotMatch([p.endpointLabel,p.status,JSON.stringify(routes)].join(''),/private/);
});
test('RTMPS, userinfo, separate RTMP logins and Digest never downgrade or use other transports',async()=>{
  for(const [address,user,password,digest] of [ ['rtmps://cam/app/key','','',false], ['rtmp://@cam/app/key','','',false],
    ['rtmp://alice:pw@cam/app/key','','',false], ['rtmp://cam/app/key','alice','',false],
    ['rtmp://cam/app/key','','pw',false], ['rtmp://cam/app/key','','',true] ]) {
    const {p,native,requests,players}=env();Object.assign(p,{address,user,password,digest,surfaceId:'42'});p.play();await p.avWork;
    assert(p.failed);assert.equal(p.endpointLabel,'');assert.doesNotMatch(p.status,/cam|alice|pw|key/);
    assert.equal(native.length+requests.length+players.length,0);
  }
});
test('saved credentialed RTSP waits for user choice; Basic and Digest never probe or fall back to each other',async()=>{
  for(const mode of ['Basic','Digest']) for(const status of [-2,-8]) {
    const {p,c,native,handles,timers}=env();p.surfaceId='77';p.autoReconnect=true;
    c.params={deviceId:'opaque-id'};c.deviceWait=deferred();p.aboutToAppear();p.onPageShow();
    c.deviceWait.resolve({url:'rtsp://alice:p%40ss@cam/live'});await tick();await p.avWork;
    assert.deepEqual(native,[]);assert.match(p.status,/Select Basic or Digest/);
    p.digest=mode==='Digest';p.play();await p.avWork;
    assert.deepEqual(native,[mode==='Basic'?['Basic','rtsp://cam/live','YWxpY2U6cEBzcw==','77',true]:['Digest','rtsp://cam/live','alice','p@ss','77',true]]);
    handles[0].status=1;p.poll(p.epoch);handles[0].status=status;p.poll(p.epoch);
    assert.match(p.status,status===-2?/permission denied/:/incompatible/);assert(!p.active);assert.equal(timers.size,0);assert.equal(native.length,1);
  }
  const {p,native}=env();p.digest=true;p.surfaceId='7';p.address='rtsp://cam/source';p.play();await p.avWork;assert.equal(native.length,0);
  p.user='chosen';p.password='selected';p.play();await p.avWork;assert.deepEqual(native[0],['Digest',p.address,'chosen','selected','7',true]);
});
test('RTSP reconnect budget spans sessions; stale polls and dequeued timers cannot revive old playback',async()=>{
  for(const action of ['exhaust','hide','select','stop','edit']) {
    const {p,native,handles,timers,fire}=env();p.surfaceId='17';p.address='rtsp://cam/live';p.autoReconnect=true;p.play();await p.avWork;
    const original=p.epoch;handles[0].status=1;p.poll(original);handles[0].status=-1;p.poll(original);
    assert(p.waiting);assert(handles[0].stopped);const queued=[...timers.values()][0];
    if(action==='exhaust') {
      for(const delay of [1000,2000,4000]) { assert.equal(fire(),delay);await p.avWork;p.poll(original);assert(p.active);
        const h=handles.at(-1);h.status=-7;p.poll(p.epoch);assert(h.stopped); }
      assert.equal(native.length,4);assert(!p.waiting);assert(p.failed);
    } else {
      if(action==='hide'){p.onPageHide();p.play();p.onPageShow();}
      if(action==='select')p.select(liveEndpoint('https://other/video'));
      if(action==='stop')p.stop();
      if(action==='edit'){p.user='old';p.password='private';p.editAddress('rtmp://other/app/key');assert.equal(p.user+p.password,'');}
      queued.task();await p.avWork;assert.equal(native.length,1);assert(!p.waiting);
    }
    assert.equal(timers.size,0);
  }
});
test('MJPEG retries only after a decoded frame; stale HTTP error and auth on retry cannot open another request',async()=>{
  for(const optedIn of [false,true]) {
    const {p,requests,timers,fire}=env();p.address='https://u:p@cam/video?private=x';p.autoReconnect=optedIn;p.play();
    const req=requests[0];req.res.resolve(200);req.cb.headersReceive({'Content-Type':'multipart/x-mixed-replace;boundary=cam'});
    const crlf=String.fromCharCode(13,10);
    const packet=Buffer.concat([Buffer.from(['--cam','Content-Type: image/jpeg','Content-Length: 4','',''].join(crlf)),Buffer.from([255,216,255,217]),Buffer.from(crlf+'--cam--'+crlf)]);
    req.cb.dataReceive(Uint8Array.from(packet).buffer);await tick();assert.match(p.status,/Playing.*MJPEG/);
    req.cb.dataEnd();assert(req.destroyed);assert.equal(p.waiting,optedIn);
    if(optedIn){assert.equal(fire(),1000);assert.equal(requests.length,2);req.cb.dataEnd();assert(p.active);
      assert.deepEqual(requests[1].options.header,req.options.header);assert.equal(requests[1].url,req.url);
      requests[1].res.resolve(401);await tick();assert.match(p.status,/401/);assert(!p.waiting);}
    assert.equal(timers.size,0);assert.doesNotMatch(p.endpointLabel+p.status,/private|u:p/);
  }
});
test('leaving while the saved URL is loading or before native surface work runs does not start playback',async()=>{
  const {p,c,native}=env();p.surfaceId='4';c.params={deviceId:'opaque-id'};c.deviceWait=deferred();p.aboutToAppear();p.onPageShow();p.onPageHide();p.onPageShow();
  c.deviceWait.resolve({url:'rtmp://cam/app/private'});await tick();assert(!p.active);assert.deepEqual(native,[]);
  p.openRoute('rtsp://cam/live');p.onPageHide();await p.avWork;assert.deepEqual(native,[]);
  p.onPageShow();p.openRoute('rtsp://cam/live');await p.avWork;assert.deepEqual(native,[['Basic','rtsp://cam/live','','4',true]]);
});

test('explicit UDP Basic and Digest are video-only, retain mode on retry and never probe TCP',async()=>{
  for(const digest of [false,true]){
    const {p,native,handles,timers,fire}=env();assert.equal(p.udp,false);
    Object.assign(p,{udp:true,digest,address:'rtsp://u:p@cam/live',autoReconnect:true,surfaceId:'42'});p.play();await p.avWork;
    const call=digest?['UDP-Digest','rtsp://cam/live','u','p','42']:['UDP-Basic','rtsp://cam/live','dTpw','42'];
    assert.deepEqual(native,[call]);const h=handles[0];h.status=1;p.poll(p.epoch);assert.equal(p.audioState,-1);p.toggleAudio();assert.equal(h.audio,0);assert.match(p.status,/UDP unicast.*Video only.*no AAC/);
    h.status=-1;p.poll(p.epoch);assert(h.stopped);assert.equal(fire(),1000);await p.avWork;assert.deepEqual(native,[call,call]);
    handles[1].status=-4;p.poll(p.epoch);assert.match(p.status,/UDP unicast.*not fall back to TCP/);assert(handles[1].stopped);assert.equal(timers.size,0);
  }
});

test('PiP requires the first rendered frame on the original surface and an explicit click',async()=>{
  for(const address of ['https://cam/video.m3u8','https://u:p@cam/live.flv','rtsp://cam/live','rtmp://cam/app/stream','http://cam/video']){
    const {p,c,players,handles,windows,requests,log}=env();Object.assign(p,{address,surfaceId:'42'});p.play();await p.avWork;
    const av=players[0],h=handles[0];if(av){av.cb.stateChange('initialized');av.cb.stateChange('playing');}
    p.startPip();await tick();assert.deepEqual(windows,[]);assert(!p.rendered);
    if(!av&&!h){await p.decode(p.epoch,new ArrayBuffer(2));assert(p.frame);p.startPip();await tick();assert.deepEqual(windows,[]);assert.equal(requests.length,1);p.stop();continue;}
    if(av)av.cb.startRenderFrame();else {h.status=1;p.poll(p.epoch);} await tick();assert.deepEqual(windows,[]);
    c.controllerId='43';p.startPip();await tick();assert.deepEqual(windows,[]);c.controllerId='42';
    p.startPip();p.startPip();await tick();const w=windows[0];assert.equal(windows.length,1);assert.equal(w.starts,1);assert.strictEqual(w.config.componentController,p.controller);assert.equal(p.pipPhase,'starting');
    w.emit('STARTED');assert.equal(p.pipPhase,'active');p.beginVoice();p.onPageHide();p.beginVoice();assert(p.active);assert(!log.some(x=>Array.isArray(x)&&x[0]==='permission'));
    p.stop();await tick();if(av){av.cb.startRenderFrame();assert(av.released);}assert(!p.rendered);assert(!p.active);assert.equal(w.stops,1);
  }
});

async function renderedRtsp(){
  const f=env(),{p,handles}=f;Object.assign(p,{surfaceId:'42',address:'rtsp://cam/live',autoReconnect:true});
  p.play();await p.avWork;handles[0].status=1;p.poll(p.epoch);p.startPip();await tick();f.windows[0].emit('STARTED');return f;
}
test('PiP close/interruption, background failure, restore order and missed restore never reconnect',async()=>{
  for(const action of ['close','takeover','disconnect','restoreBeforeShow','showBeforeRestore','neverShown']){
    const {p,native,handles,windows,restore,timers}=await renderedRtsp(),h=handles[0],w=windows[0];
    p.onPageHide();assert(!h.stopped);p.play();assert.equal(native.length,1);
    if(action==='disconnect'){h.status=-1;p.poll(p.epoch);assert.equal(p.status,en.get('viewer_rtsp_error'));}
    else if(action==='close'||action==='takeover')w.emit('STOPPED',action==='close'?'dragDelete':'other');
    else {
      w.emit('ABOUT_TO_RESTORE');if(action==='showBeforeRestore')p.onPageShow();w.emit('STOPPED','panelActionRestore');
      assert(!h.stopped);if(action==='neverShown'){assert.equal(restore.size,1);[...restore.values()][0]();}
      else {p.onPageShow();assert.strictEqual(p.nativeHandle,h);assert(p.rendered);p.stop();}
    }
    await tick();p.onPageShow();assert.equal(native.length,1);assert(h.stopped);assert.equal(restore.size+timers.size,0);
  }
});
test('navigation, back, destruction, surface loss and stop cancel PiP before the page is hidden',async()=>{
  for(const action of ['openArchives','onBackPress','aboutToDisappear','surfaceLost','stop']){
    const {p,native,handles,windows,timers}=await renderedRtsp(),gen=p.epoch,cb=windows[0].fn;
    p[action]();assert(handles[0].stopped);p.onPageHide();await tick();cb(2,'');p.poll(gen);p.onPageShow();
    assert.equal(windows[0].stops,1);assert.equal(windows[0].fn,null);assert(!p.active);assert.equal(native.length,1);assert.equal(timers.size,0);
  }
});
test('pending window create is cancelled on hide; late microphone grants cannot record into PiP',async()=>{
  const {p,c,windows,recorders,players,log}=env();Object.assign(p,{surfaceId:'42',address:'http://cam/live.flv'});p.play();await p.avWork;
  const av=players[0];av.cb.stateChange('initialized');av.cb.startRenderFrame();c.permissionWait=deferred();p.beginVoice();await tick();
  c.createWait=deferred();p.startPip();await tick();c.permissionWait.resolve({authResults:[0]});p.beginVoice();p.onPageHide();c.createWait.resolve();await tick();
  assert(!windows[0].starts);assert(av.released);assert.equal(recorders.length,0);assert.equal(log.filter(x=>Array.isArray(x)&&x[0]==='permission').length,1);
});
test('view pages keep one permanent surface, hide credentials in settings, and localize their new controls',()=>{
  const views=['Player','Playback'].map(name=>readFileSync(join(root,`ets/pages/${name}.ets`),'utf8').split('  build() {')[1]);
  for(const source of views)assert.equal((source.match(/\bXComponent\(/g)||[]).length,1);
  const live=views[0],settings=live.split('  settings() {')[1].split('  @Builder')[0];
  assert.equal((live.match(/\bTextInput\(/g)||[]).length,3);assert.equal((settings.match(/\bTextInput\(/g)||[]).length,3);
  assert.doesNotMatch(live,/Text\(this\.(?:address|draft|password|user)\)|rtspTakeSnapshot|rtspStartRecord/);
  assert.match(live,/if \(this\.selectedHttp && !this\.draft\).*this\.openArchives\(\)/);
  assert.match(live,/if \(this\.canOfferPip\(\)\)/);
  const keys=new Set(views.join('\n').match(/view_ui_\w+/g));
  for(const locale of ['base','zh_CN']){const entries=require(join(root,`resources/${locale}/element/view_ui_strings.json`)).string;
    assert.deepEqual(new Set(entries.map(x=>x.name)),keys);assert(entries.every(x=>x.value.trim()));}
});

test('settings, information, hidden chrome and fullscreen back do not replace or release the decoder',async()=>{
  const {p,c,handles,bars,recorders,requests}=env();p.connectFromControls();assert.equal(p.panel,'settings');
  assert.equal(p.onBackPress(),true);assert.equal(p.panel,'none');assert(!p.active);
  p.surfaceId='42';p.openRoute('rtsp://user:private@cam/live');assert.equal(p.panel,'settings');
  p.onBackPress();p.connectFromControls();assert.equal(p.panel,'settings');assert.equal(handles.length,0);
  p.digest=true;p.play();await p.avWork;const h=handles[0],gen=p.epoch;assert.equal(p.panel,'none');
  assert(!p.canOfferPip());h.status=1;p.poll(gen);assert(p.canOfferPip());c.pipEnabled=false;assert(!p.canOfferPip());
  for(const panel of ['menu','settings','info']){p.showPanel(panel);assert(p.onBackPress());assert.equal(p.panel,'none');}
  p.tapVideo();assert(!p.controlsVisible);assert(p.onBackPress());assert(p.controlsVisible);
  p.setFullscreen(true);await p.barQueue;assert(!p.controlsVisible);p.tapVideo();p.showPanel('info');
  assert(p.onBackPress());assert(p.fullscreen);assert(p.onBackPress());await p.barQueue;assert(!p.fullscreen);
  assert.deepEqual(bars,[['layout',true],['status',false],['navigationIndicator',false],['status',true],['navigationIndicator',false],['layout',false]]);
  assert.equal(p.epoch,gen);assert.strictEqual(p.nativeHandle,h);assert.equal(recorders.length+requests.length,0);
  let backs=0;p.getUIContext=()=>({getRouter:()=>({back:()=>backs++})});p.showPanel('menu');p.toolbarBack();assert.equal(backs,0);
  p.toolbarBack();assert.equal(backs,1);assert(h.stopped);p.onPageHide();p.onPageShow();assert.equal(handles.length,1);
});

test('background dismisses editing without resending or reviving media; hiding talk controls cancels without upload',async()=>{
  const {p,recorders,requests}=env();p.openRoute('http://camera/video');const req=requests[0];
  p.beginVoice();await tick();assert.equal(recorders.length,1);p.tapVideo();await tick();assert.equal(p.voice,undefined);
  assert.equal(requests.length,1);p.showPanel('settings');p.onPageHide();p.onPageShow();assert.equal(p.panel,'none');assert(p.controlsVisible);
  assert(req.destroyed);assert(!p.active);assert.equal(requests.length,1);
});

test('a different source cannot bind before PiP releases the surface, including a rejected stop',async()=>{
  for(const action of ['release','stopFailed','cancel']){
    const {p,c,native,handles}=await renderedRtsp();c.stopWait=deferred();c.stopFail=action==='stopFailed';p.address='rtmp://cam/app/next';p.play();await tick();
    assert.equal(native.length,1);assert(handles[0].stopped);assert(p.waiting);if(action==='cancel')p.stop();c.stopWait.resolve();await tick();
    if(action==='stopFailed'){assert.equal(p.pipPhase,'stopFailed');assert.match(p.status,/Exit picture-in-picture/);assert.equal(native.length,1);c.stopFail=false;await p.pip.stop();assert.equal(p.pipPhase,'idle');}
    else if(action==='cancel'){assert(!p.waiting);assert(!p.active);assert.equal(native.length,1);}
    else {assert.equal(p.pipPhase,'idle');assert.equal(native.at(-1)[0],'RTMP');}p.stop();
  }
});
