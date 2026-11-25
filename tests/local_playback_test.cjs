// Node >= 22.13. Drive the page's actual methods with deterministic async AVPlayer/filesystem doubles.
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const text = readFileSync(join(__dirname,'../entry/src/main/ets/pages/Playback.ets'),'utf8');
const logic = text.slice(0, text.indexOf('  build() {')).replace(/^import .*;$/gm,'').replace(/^@(Entry|Component)\s*$/gm,'')
  .replace('struct Playback', 'class Playback').replace(/@State /g,'') + '}';
const events = [], players = [], waits = [], rows = new Map([
  ['/data', ['ignore.mp4','ipc_old.mp4','ipc_gone.mp4','ipc_new.mp4']], ['/data/MD', ['IPS_motion-1.mp4','extra.mp4']]
]);
let busy = false, copied = 0, nextFD = 100;
const fileIo = {
  OpenMode: {READ_ONLY: 0}, listFileSync: dir => rows.get(dir),
  statSync: path => { if(path.includes('gone')) throw Error('file rotated');return { isFile:()=>true,size:80,mtime:path.includes('old')?1:path.includes('motion')?2:3 }; },
  openSync: path => { const obj={fd:nextFD++};events.push(['open',obj.fd]);return obj; },
  closeSync: file => events.push(['close',file.fd])
};
function deferred() { let resolve;const promise=new Promise(r=>resolve=r);return {resolve,promise}; }
const media = { SeekMode:{SEEK_CLOSEST:2}, createAVPlayer: async()=>{
  const p = { id:players.length, cb:{}, duration:33000, on(ev,fn){this.cb[ev]=fn;}, async prepare(){}, async play(){}, async pause(){},
    seek(ms){events.push(['seek',ms]);},async release(){events.push(['release-start',this.id]); if(this.block)await this.block.promise;events.push(['release-end',this.id]);} };
  players.push(p);const block=waits.shift();if(block)await block.promise;return p;
}};
const result={Saved:'saved',Cancelled:'cancelled',EmptyRecording:'empty',Failed:'failed'};
const zh = new Map(require('../entry/src/main/resources/zh_CN/element/playback_publish_scan.json').string
  .map(({name, value}) => [`app.string.${name}`, value]));
const Playback = new Function('$r','fileIo','media','XComponentController','getContext','SliderChangeMode','streaming','RecordingExportResult','exportRecording',
  stripTypeScriptTypes(logic)+'\nreturn Playback;')(key=>{assert.ok(zh.has(key),key);return zh.get(key);},fileIo,media,class{},()=>({filesDir:'/data'}),{Begin:0,Moving:1,End:2},
    {recordingFileBusy:()=>busy},result,async()=>{copied++;return result.Cancelled;});
(async()=>{
  const page=new Playback();page.refreshList();
  assert.deepEqual(page.archives.map(r=>r.name),['ipc_new.mp4','MD/IPS_motion-1.mp4','ipc_old.mp4']);
  const [a,b] = page.archives;
  page.play(a);await page.work;assert.equal(players.length,0); // no surface, no file descriptor
  page.surfaceId='1001';const boot=deferred();waits.push(boot);page.play(a);await new Promise(setImmediate);
  assert.equal(players.length,1);page.play(b);boot.resolve();await page.work;
  assert.deepEqual(events.slice(0,4),[['open',100],['release-start',0],['release-end',0],['close',100]]);
  assert.equal(players[0].fdSrc,undefined);assert.deepEqual(players[1].fdSrc,{fd:101});
  players[1].cb.stateChange('prepared');players[1].cb.stateChange('playing');
  assert.equal(page.duration,33000);players[1].cb.timeUpdate(1000);page.seek(6000,0);players[1].cb.timeUpdate(2300);assert.equal(page.elapsedMs,6000);
  page.seek(6000,2);assert.deepEqual(events.at(-1),['seek',6000]);
  const pauseRelease=deferred();players[1].block=pauseRelease;page.onPageHide();await new Promise(setImmediate);
  players[1].cb.error();assert.equal(page.message,'');assert(!events.some(e=>e[0]==='close'&&e[1]===101));
  pauseRelease.resolve();await page.work;assert.deepEqual(events.slice(-2),[['release-end',1],['close',101]]);
  busy=true;await page.saveArchive(a);assert.equal(copied,0);assert.match(page.message,/写入/);
  busy=false;await page.saveArchive(b);assert.equal(copied,1);assert.equal(page.message,'');
  // File browsing is an overlay. Back/reselect/tapping video must not recreate the active player.
  page.onPageShow();page.chooseFile(a);await page.work;const current=players.at(-1),gen=page.epoch;
  current.cb.stateChange('playing');page.showList();assert(page.listVisible);assert(page.onBackPress());assert(!page.listVisible);
  page.showList();page.chooseFile(a);assert(!page.listVisible);page.tapVideo();assert(!page.controlsVisible);
  assert(page.onBackPress());assert(page.controlsVisible);assert.equal(page.epoch,gen);assert.strictEqual(page.avPlayer,current);
  const navigations=[];page.getUIContext=()=>({getRouter:()=>({back:()=>navigations.push('back'),pushUrl:async x=>navigations.push(x)})});
  page.showList();page.toolbarBack();assert.deepEqual(navigations,[]);page.toolbarBack();await page.work;
  assert.deepEqual(navigations,['back']);assert.equal(page.playingName,'');assert(page.listVisible);
  page.chooseFile(b);await page.work;const late=players.at(-1);page.showList();page.openRemote();page.onPageHide();
  page.chooseFile(a);late.cb.stateChange('playing');late.cb.error();page.onPageShow();await page.work;
  assert.deepEqual(navigations,['back',{url:'pages/RemoteArchives'}]);assert.equal(page.avPlayer,undefined);
  assert.equal(page.playerState,'idle');assert(page.controlsVisible && page.listVisible);assert.equal(players.at(-1),late);
  console.log('PASS: MD/manual listing, rapid switches, late create/error callbacks, seek, stop/release ordering and export gate');
})().catch(e=>{console.error(e);process.exitCode=1;});
