// Pure host tests: execute the real ArkTS helpers, replace picker/files with in-memory fakes.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
function load(file, name, mocks = {}) {
  const src = readFileSync(join(__dirname, '../entry/src/main/ets/util', file + '.ets'), 'utf8');
  return new Function(...Object.keys(mocks), stripTypeScriptTypes(src.replace(/^import .*;$/gm, '').replace(/^export /gm, '')) + `\nreturn ${name}`)(...Object.values(mocks));
}
const Ownership = load('RecordingOptions', 'RecordingOwnership');
const options = { includeAudio: true, segmentMinutes: 30, fourGbLimit: false };
function setupMic(running = false) {
  const log = [];
  const controls = { generation: 1, micGeneration: () => controls.generation, micRunning: () => running,
    startMic() {log.push('mic+'); running = true; controls.generation++; return true;},
    stopMic() {log.push('mic-'); running = false; controls.generation++;},
    startRecord(p, o) {log.push(['rec+', o]); return true;}, stopRecord() {log.push('rec-');} };
  return { controls, log, owner: new Ownership(controls) };
}
test('release only mic owned by successful ticket; never let stale Stop affect replacement', () => {
  const {controls, log, owner} = setupMic();
  const t = owner.reserve(); assert.equal(owner.reserve(), 0);
  assert(owner.start(t, 'a', options)); assert(!owner.start(t, 'twice', options));
  assert(owner.stop(t)); assert.deepEqual(log, ['mic+', ['rec+',options], 'rec-', 'mic-']);
  log.length = 0;
  const next = owner.reserve(); assert(!owner.stop(t)); assert(owner.start(next, 'b', options));
  controls.generation++; // user takes over the same running microphone
  assert(owner.stop(next)); assert.deepEqual(log, ['mic+', ['rec+',options], 'rec-']);
});
test('optional video overrides go to the host unchanged, not to the shared camera', () => {
  const {log, owner} = setupMic(true);
  const override = {...options, videoMime:'video/hevc', videoBitrate:4000000, gopMs:2000};
  const t = owner.reserve(); assert(owner.start(t, 'a', override)); owner.stop(t);
  assert.deepEqual(log, [['rec+', override], 'rec-']);
});
test('silent recording / borrowed mic / failed start / cancellation', () => {
  for (const alreadyRunning of [false,true]) {
    const {log, owner} = setupMic(alreadyRunning);
    const o = {...options, includeAudio:alreadyRunning}, t = owner.reserve();
    assert(owner.start(t,'a',o)); owner.stop(t); assert.deepEqual(log,[['rec+',o],'rec-']);
  }
  const {controls, log, owner} = setupMic();
  controls.startRecord = () => false; assert(!owner.start(owner.reserve(),'a',options));
  assert.deepEqual(log,['mic+','mic-']); log.length = 0;
  controls.startMic = () => false; assert(!owner.start(owner.reserve(),'a',options));
  const canceled = owner.reserve(); owner.stop(canceled); const next = owner.reserve();
  assert(!owner.start(canceled, 'old', options)); assert(!owner.stop(canceled));
  assert(!owner.start(next,'bad',{...options, segmentMinutes:NaN})); assert(owner.reserve()); assert.deepEqual(log,[]);
});
const ctx = {filesDir:'/private/files',cacheDir:'/private/cache',applicationInfo:{name:'com.example.test'}};
const directory = '/storage/Users/currentUser/Download/com.example.test';
const uri = 'file://docs'+directory, path = ctx.filesDir+'/MD/IPS_12_2.mp4', target = directory+'/IPS_12_2.mp4';
function setupDownloads() {
  let counter = 0;
  const cfg = {grant:[uri], bad:'', onCopy:null, selected:0};
  const files = new Map([[path,'synthetic sample']]), opened = new Map();
  const picker = {DocumentSaveOptions:class {}, DocumentPickerMode:{DOWNLOAD:1}, DocumentViewPicker:class {
    constructor(c){assert.equal(c,ctx);} async save(o){assert.deepEqual(Object.keys(o),['pickerMode']); assert.equal(o.pickerMode,1); cfg.selected++; return cfg.grant;}
  }};
  const fileUri = {FileUri:class {constructor(uri){this.path = new URL(uri).pathname;} isRemoteUri(){return false;}}};
  const fileIo = {OpenMode:{CREATE:1,READ_WRITE:2,READ_ONLY:4,NOFOLLOW:8},
    async mkdtemp(prefix){assert(prefix.startsWith(ctx.cacheDir+'/')); return prefix+(++counter);},
    async open(p, flags){if(flags===3){assert(p.startsWith(ctx.cacheDir+'/')); files.set(p,'');} else assert.equal(flags,12);
      assert(files.has(p)); const fd=++counter; opened.set(fd,p); return {fd};},
    async stat(fd){const p=opened.get(fd); assert(files.has(p));return {size:files.get(p).length,isFile:()=>true,mtime:1,mtimeNs:1n};},
    async copyFile(a,b){files.set(opened.get(b),files.get(opened.get(a))); if(cfg.onCopy)cfg.onCopy(); if(cfg.bad==='copy')throw Error('copy');},
    async fsync(){if(cfg.bad==='fsync')throw Error('fsync');},
    async close(f){assert(opened.delete(f.fd));if(cfg.bad==='close')throw Error('close');},
    async moveFile(src,dest,mode){assert.equal(mode,1);assert.equal(opened.size,0);assert.equal(dest,target);assert(src.startsWith(ctx.cacheDir+'/'));
      if(files.has(dest)||cfg.bad==='volume')throw Error('move'); files.set(dest,files.get(src));files.delete(src);},
    async rmdir(stage){assert(stage.startsWith(ctx.cacheDir+'/record-copy-'));files.delete(stage+'/copy.part');}
  };
  const Downloads = load('RecordingDownloads','RecordingDownloads',{fileIo,fileUri,picker});
  return {dest:new Downloads(),cfg,files,opened};
}
test('DOWNLOAD consent only; cancellation, stale picker and wrong provider fail closed', async () => {
  const {dest,cfg,files}=setupDownloads(); assert.equal(await dest.copyCompleted(ctx,path),false); assert.equal(cfg.selected,0);
  cfg.grant=[];assert.equal(await dest.authorize(ctx),false);
  for(const grant of ['https://example.invalid/a', 'file://else'+directory,'file://docs/arbitrary','file://docs/a/../x/Download/com.example.test',uri+'/foo.mp4',uri+'?x']) {
    cfg.grant=[grant]; await assert.rejects(dest.authorize(ctx)); assert.equal(await dest.copyCompleted(ctx,path),false);
  }
  let resume;cfg.grant=new Promise(r=>resume=r);const pending=dest.authorize(ctx);dest.disable();resume([uri]);
  assert.equal(await pending,false);assert.deepEqual([...files.keys()],[path]);
});
test('completed copy preserves source, respects revocation, never overwrites same name', async () => {
  for(const bad of ['copy','fsync','close','volume','collision','revoke','ok']) {
    const {dest,cfg,files,opened}=setupDownloads(); assert(await dest.authorize(ctx));cfg.bad=bad;
    await assert.rejects(dest.copyCompleted(ctx,ctx.filesDir+'/../MD/IPS_12_2.mp4'));
    if(bad==='collision')files.set(target,'existing');
    if(bad==='revoke')cfg.onCopy=()=>dest.disable();
    if(bad==='ok')assert(await dest.copyCompleted(ctx,path));
    else if(bad==='revoke')assert.equal(await dest.copyCompleted(ctx,path),false);
    else await assert.rejects(dest.copyCompleted(ctx,path));
    assert.equal(files.get(path),'synthetic sample'); assert.equal(opened.size,0); assert.equal(cfg.selected,1);
    assert.equal(files.get(target),bad==='ok'?'synthetic sample':bad==='collision'?'existing':undefined);
    assert.equal([...files.keys()].some(p=>p.startsWith(ctx.cacheDir)),false);
  }
});
