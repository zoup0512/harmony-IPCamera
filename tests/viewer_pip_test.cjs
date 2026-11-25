// Node 22.13+. Real session logic, synthetic window/controller only; never loads a system module.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { stripTypeScriptTypes } = require('node:module');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const root = join(__dirname, '../entry/src/main');
const src = readFileSync(join(root, 'ets/util/ViewerPipSession.ets'), 'utf8');
const load = new Function('canIUse', 'PiPWindow', stripTypeScriptTypes(src.replace(/^import .*;$/gm, '').replace(/^export /gm, '')) + '\nreturn {ViewerPipSession,viewerPipEligible};');
const states = {ABOUT_TO_START:1, STARTED:2, ABOUT_TO_STOP:3, STOPPED:4, ABOUT_TO_RESTORE:5, ERROR:6};
function deferred() { let resolve, reject; const promise = new Promise((a,b)=>{resolve=a;reject=b;}); return {promise,resolve,reject}; }
const tick = () => new Promise(setImmediate);
function fixture(o={}) {
  const history=[], native=[], windows=[], caps=[];
  const surface={surfaceId:'777', controller:{ getXComponentSurfaceId:()=>surface.liveId }, liveId:'777', isRendering:()=>!o.stale};
  let showing=true;
  const ctx={windowStage:{getMainWindowSync:()=>({isWindowShowing:()=>showing && !o.hidden})}};
  const api={PiPState:states, PiPTemplateType:{VIDEO_LIVE:3},isPiPEnabled:()=>!o.disabled, create:async(config)=>{
    assert.strictEqual(config.context,ctx);assert.strictEqual(config.componentController,surface.controller);
    assert.equal(config.templateType,3);assert.deepEqual(config.controlGroups,[]);native.push('create');
    if(o.createFail)throw Error(); if(o.createWait)await o.createWait.promise;
    const w={fn:null, on(type,fn){assert.equal(type,'stateChange');this.fn=fn;},off(type){assert.equal(type,'stateChange');this.fn=null;},
      async startPiP(){native.push('start');if(o.startWait)await o.startWait.promise;if(o.startFail)throw Error();},
      async stopPiP(){native.push('stop');if(o.stopWait)await o.stopWait.promise;if(o.stopFail)throw Error();},emit(s,r='0'){this.fn?.(states[s],r);}};
    windows.push(w);return w;
  }};
  const {ViewerPipSession,viewerPipEligible}=load(cap=>{caps.push(cap);return cap!==o.noCap;},api);
  const session=new ViewerPipSession((phase,end)=>history.push([phase,end]));
  return {session,surface,ctx,windows,caps,native,history,viewerPipEligible,hide(){showing=false;},start:()=>session.start(ctx,surface)};
}
test('only a current rendering decoder bound to this exact controller and surface is eligible',async()=>{
  const f=fixture();assert(!f.viewerPipEligible());
  for(const id of ['', '0', '123']){f.surface.liveId=id;await f.start();assert.deepEqual(f.history.at(-1),['idle','surfaceLost']);}
  f.surface.controller.getXComponentSurfaceId=()=>{throw Error('destroyed');};await f.start();assert.deepEqual(f.native,[]);
  const g=fixture({stale:true});await g.start();assert.deepEqual(g.native,[]);
});
test('missing capability, disabled system, missing main window and native failure never report active',async()=>{
  for(const o of [{noCap:'SystemCapability.Window.SessionManager'},{noCap:'SystemCapability.WindowManager.WindowManager.Core'},{disabled:true},{hidden:true},{noStage:true},{createFail:true},{startFail:true}]){
    const f=fixture(o); if(o.noStage)delete f.ctx.windowStage;await f.start();assert.deepEqual(f.history.at(-1),['idle','unavailable']);assert(!f.session.keepsSurface());
    assert(!f.history.some(([p])=>p==='active'));assert(!f.windows[0]?.fn);if(!o.createFail && !o.startFail)assert.deepEqual(f.native,[]);
  }
});
test('only confirmed STARTED advertises PiP; panel restore, close and takeover detach exactly that generation',async()=>{
  const f=fixture();assert.deepEqual(f.native,[]);
  for(const [reason,end] of [['panelActionRestore','restored'],['panelActionDelete','closed'],['dragDelete','closed'],['other','interrupted']]){
    await Promise.all([f.start(),f.start()]);const w=f.windows.at(-1),stale=w.fn;
    assert.deepEqual(f.history.at(-1),['starting','']);assert(f.session.keepsSurface()); w.emit('STARTED');assert.deepEqual(f.history.at(-1),['active','']);
    if(end==='restored')w.emit('ABOUT_TO_RESTORE');w.emit('ABOUT_TO_STOP');w.emit('STOPPED',reason);
    assert.deepEqual(f.history.at(-1),['idle',end]);assert(!w.fn);assert(!f.session.keepsSurface());stale(states.STARTED,'0');assert.deepEqual(f.history.at(-1),['idle',end]);
  }
  assert.deepEqual(f.native,Array.from({length:4},()=>['create','start']).flat());
});
test('cancel/dispose, stale binding and hidden source while create is pending never transfer a surface',async()=>{
  for(const mode of ['stop','dispose','epoch','surface','hide']){
    const o={createWait:deferred()}, f=fixture(o); const start=f.start();await tick();assert(!f.session.keepsSurface());
    let closing; if(mode==='stop')closing=f.session.stop();if(mode==='dispose')closing=f.session.dispose();if(mode==='epoch')o.stale=true;if(mode==='surface')f.surface.liveId='888';if(mode==='hide')f.hide();
    o.createWait.resolve();await Promise.all([start,closing]);assert.deepEqual(f.native,['create']);assert(!f.windows[0].fn);
    if(mode==='dispose'){await f.start();assert.deepEqual(f.native,['create']);}else assert.deepEqual(f.history.at(-1),['idle',mode==='stop'?'stopped':mode==='hide'?'unavailable':'surfaceLost']);
  }
});
test('explicit stop serializes with a pending start, ignores late events and never implicitly starts again',async()=>{
  for(const reject of [false,true]){
    const o={startWait:deferred()},f=fixture(o);const start=f.start();await tick();const w=f.windows[0];assert(f.session.keepsSurface());
    const stop=f.session.stop();assert.strictEqual(f.session.stop(),stop);if(reject)o.startWait.reject(Error());else{w.emit('STARTED');o.startWait.resolve();}
    await Promise.all([start,stop]);assert.deepEqual(f.native,reject?['create','start']:['create','start','stop']);assert(!f.history.some(([p])=>p==='active'));assert.deepEqual(f.history.at(-1),['idle','stopped']);assert(!w.fn);
  }
});
test('native error during start cannot orphan a controller that opens late',async()=>{
  const o={startWait:deferred()},f=fixture(o);const start=f.start();await tick();const w=f.windows[0];w.emit('ERROR');w.emit('STARTED');o.startWait.resolve();await start;await tick();
  assert.deepEqual(f.native,['create','start','stop']);assert(!f.history.some(([p])=>p==='active'));assert.deepEqual(f.history.at(-1),['idle','error']);assert(!w.fn);
});
test('failed stop does not claim inline playback: retain ownership for retry or native terminal callback',async()=>{
  const o={stopFail:true},f=fixture(o);await f.start();const w=f.windows[0];w.emit('STARTED');await f.session.stop();
  assert.deepEqual(f.history.at(-1),['stopFailed','error']);assert(f.session.keepsSurface());await f.start();assert.equal(f.windows.length,1);
  o.stopFail=false;await f.session.stop();assert.deepEqual(f.history.at(-1),['idle','stopped']);assert(!w.fn);
});
test('late start or stop completion from an old instance cannot tear down a newer controller',async()=>{
  const o={stopWait:deferred()},f=fixture(o);await f.start();const old=f.windows[0], callback=old.fn;old.emit('STARTED');const stop=f.session.stop();await tick();
  old.emit('STOPPED','requestDelete');await f.start();const current=f.windows[1];current.emit('STARTED');o.stopWait.resolve();await stop;callback(states.STOPPED,'other');assert.deepEqual(f.history.at(-1),['active','']);assert(current.fn);
  current.emit('ERROR');await tick();assert.deepEqual(f.history.at(-1),['idle','error']);assert(!current.fn);
});
test('dispose releases the system window and listener even on failure; no callbacks into dead page',async()=>{
  for(const stopFail of [false,true]) {const f=fixture({stopFail});await f.start();const w=f.windows[0];w.emit('STARTED');const count=f.history.length;await f.session.dispose();assert(!w.fn);assert(!f.session.keepsSurface());await f.start();assert.equal(f.history.length,count);assert.deepEqual(f.native,['create','start','stop']);}
});
test('standalone controls have both locales, an explicit fullscreen fallback and no capture/settings entry',()=>{
  const c=readFileSync(join(root,'ets/components/ViewerPipControls.ets'),'utf8');const used=[...c.matchAll(/app\.string\.(pip_\w+)/g)].map(x=>x[1]);
  for(const locale of ['base','zh_CN']){const strings=require(join(root,'resources',locale,'element/viewer_pip.json')).string;assert.deepEqual([...new Set(used)].sort(),strings.map(x=>x.name).sort());}
  assert.match(c,/\.onClick\(\(\) => this\.onFullscreen\(!this\.fullscreen\)\)/);assert.match(c,/this\.eligible && this\.phase === 'idle'/);
  assert.doesNotMatch(src+c,/requestPermissions|abilityAccessCtrl|requestPermission|startAbility\(|setAutoStartEnabled\(true\)|createAVPlayer|createAVRecorder/);
});
