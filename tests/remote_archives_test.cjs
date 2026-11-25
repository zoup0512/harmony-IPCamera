// node --test tests/remote_archives_test.cjs (Node >= 22.13, loopback only, no device)
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { randomUUID } = require('node:crypto');
const { createServer } = require('node:http');
const { pathToFileURL, fileURLToPath } = require('node:url');
const { stripTypeScriptTypes } = require('node:module');
const ets = path.join(__dirname, '../entry/src/main/ets');
const source = f => fs.readFileSync(path.join(ets, f), 'utf8').replace(/^import .*;$/gm, '');
const helper = stripTypeScriptTypes(source('util/RemoteArchiveClient.ets').replace(/^export /gm, ''));
const pageCode = stripTypeScriptTypes(source('pages/RemoteArchives.ets').split('  build() {')[0]
  .replace(/^@(Entry|Component)\s*$/gm, '').replace('struct RemoteArchives', 'class RemoteArchives').replace(/@State /g, '') + '}');
const resources = new Map(require('../entry/src/main/resources/zh_CN/element/addon_remote.json').string
  .map(({name, value}) => ['app.string.' + name, value]));
function $r(id, ...args) {
  assert.ok(resources.has(id), id);
  return resources.get(id).replace(/%[ds]/g, () => String(args.shift()));
}
const util = { generateRandomUUID: randomUUID, Base64Helper: class { encodeToStringSync(b) { return Buffer.from(b).toString('base64'); } },
  TextEncoder: { create: () => ({ encodeInto: s => Buffer.from(s) }) },
  TextDecoder: { create: () => ({ decodeToString: b => Buffer.from(b).toString('utf8') }) } };
function clientFor(createHttp) {
  return new Function('http','url','util', helper + '\nreturn { RemoteArchiveClient, archiveOrigin };')(
    { createHttp, RequestMethod: { GET: 'GET' } }, { URL: { parseURL: s => new URL(s) } }, util);
}
async function server(t, listener) {
  const srv = createServer(listener);
  await new Promise(resolve => srv.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => { srv.close(resolve); srv.closeAllConnections(); }));
  return `http://127.0.0.1:${srv.address().port}`;
}
// Mirror NetworkKit's callback contract. Use fetch's actual redirect behavior,
// driven by the production request options (the default here also follows).
function loopback() {
  const abort = new AbortController(), cb = {};
  return { on: (k, f) => cb[k] = f, destroy: () => abort.abort(), async requestInStream(dest, opts) {
    assert.equal(opts.usingCache, false); assert.equal(opts.usingProxy, false);
    const r = await fetch(dest, { method: opts.method, headers: opts.header,
      redirect: opts.maxRedirects === 0 ? 'manual' : 'follow', signal: abort.signal });
    cb.headersReceive(Object.fromEntries(r.headers));
    for await (const part of r.body) cb.dataReceive(Uint8Array.from(part).buffer);
    cb.dataEnd(); return r.status;
  } };
}
const android = (href, label = 'untrusted label') => `<li><a href="${href}" target="_blank">${label}</a></li>`;
const movie = Buffer.concat([Buffer.from('000000186674797069736F6D','hex'), Buffer.alloc(240000, 0xab)]);
const auth = 'Basic ' + Buffer.from('viewer:sample').toString('base64');

test('explicit origins only; no guessed ports, paths, userinfo or downgrade', () => {
  const {archiveOrigin, RemoteArchiveClient} = clientFor(() => assert.fail('network'));
  for (const bad of ['rtsp://127.0.0.1:8554', 'https://a/video','http://a@b','https://u:p@a','http://a:65536',
    'https://a:0','http://a?pw=secret','http://a#frag','http://a\\@b','http://bad%2fhost']) assert.throws(() => archiveOrigin(bad));
  assert.equal(archiveOrigin('HTTPS://Example.test:443/'), 'https://example.test');
  assert.equal(archiveOrigin('http://[::1]:8081'), 'http://[::1]:8081');
  assert.throws(() => new RemoteArchiveClient('https://example.test', 'a:b','x'));
});

test('actual page actions and file I/O: fetch only after selection; exact export and private cache cleanup', async t => {
  const calls = []; let listing = ''; let mode = 'ok'; let selected = [];
  const secondCalls = [];
  const other = await server(t, (q,r) => { secondCalls.push(q.headers); r.end('other'); });
  const origin = await server(t, (q,r) => {
    calls.push([q.url,q.headers.authorization]);
    if (mode === 'redirect') { r.writeHead(302, {Location: other + '/elsewhere'}); r.end('moved'); }
    else if (mode === 'deny') { r.writeHead(401); r.end('denied'); }
    else if (q.url === '/getarchives') r.end(listing);
    else if (mode === 'playlist') r.end('#EXTM3U\n' + other);
    else { r.write(movie.subarray(0, 4)); setImmediate(() => r.end(movie.subarray(4))); }
  });
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'remote-archives-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const picker = { DocumentSaveOptions: class {}, DocumentViewPicker: class { async save(opt) {
    selected.push(opt.newFileNames); return [path.join(dir,'chosen')];
  } } };
  const fileIo = { OpenMode: { CREATE:1, WRITE_ONLY:2, TRUNC:4 }, unlinkSync: fs.unlinkSync,
    openSync: (p,m) => ({fd:fs.openSync(p, (m & 1) ? 'wx' : 'w')}), closeSync: f => fs.closeSync(f.fd),
    // Exercise partial writes, not only full network buffers.
    writeSync: (fd,data) => fs.writeSync(fd, Buffer.from(data), 0, Math.min(data.byteLength,16001)),
    copyFile: async (p,fd) => fs.writeFileSync(fd, fs.readFileSync(p)), fsync: async fd => fs.fsyncSync(fd) };
  const { RemoteArchiveClient, archiveOrigin } = clientFor(loopback);
  const Page = new Function('getContext','util','fileIo','fileUri','picker','RemoteArchiveClient','archiveOrigin','$r', pageCode + '\nreturn RemoteArchives;')(
    () => ({cacheDir:dir}), util,fileIo,{getUriFromPath:p => pathToFileURL(p).href},picker,RemoteArchiveClient,archiveOrigin,$r);
  const page = new Page(); let params = {url:'rtsp://viewer:sample@127.0.0.1:8554'};
  page.getUIContext = () => ({getRouter:() => ({ getParams:() => params })});
  page.aboutToAppear(); assert.equal(page.origin,''); assert.deepEqual(calls,[]);
  params = {url:origin}; page.aboutToAppear(); assert.equal(page.origin,origin); assert.deepEqual(calls,[]);
  page.user='viewer'; page.password='sample';
  listing = `ipc_manual.mp4 ${movie.length}\nMD/IPS_motion.mp4 ${movie.length}\nother.mp4 10\nipc_a/escape.mp4 10\n`;
  await page.refresh(); assert.deepEqual(calls, [['/getarchives',auth]]);
  assert.deepEqual(page.rows.map(r=>[r.name,r.motion]),[['ipc_manual.mp4',false],['IPS_motion.mp4',true]]);
  await page.select(page.rows[0],false); assert.equal(calls.at(-1)[0],'/get/ipc_manual.mp4');
  assert.deepEqual(fs.readFileSync(fileURLToPath(page.video)),movie); assert.deepEqual(selected,[]);
  page.stopVideo(); assert.deepEqual(fs.readdirSync(dir),[]);
  const invalid = [other + '/media/IPS_x.mp4', '//other/media/IPS_x.mp4', '/media/MD/extra/IPS_x.mp4',
    '/media/IPS_%2foutside.mp4','/media/md/IPS_%252Foutside.mp4','/media/IPS_%5cx.mp4','/media/IPS_..%2Fx.mp4',
    '/media/IPS_%00bad.mp4','/media/IPS_q.mp4?host=x','/get/IPC_photo.jpg','javascript:void(0)'];
  listing = [android('/media/IPS_manual.mp4'), android('/media/md/IPS_%E9%9B%AA%20clip.mp4'), ...invalid.map(s=>android(s))].join('');
  await page.refresh(); assert.deepEqual(page.rows.map(r=>r.key),['IPS_manual.mp4','MD/IPS_雪 clip.mp4']);
  const row = page.rows[1];
  await assert.rejects(page.client.file('../IPS_unknown.mp4',()=>{}));
  await page.select(row,true); assert.deepEqual(calls.at(-1),['/get/MD/IPS_%E9%9B%AA%20clip.mp4',auth]);
  assert.deepEqual(selected,[['IPS_雪 clip.mp4']]); assert.deepEqual(fs.readFileSync(path.join(dir,'chosen')),movie);
  for (mode of ['redirect','deny','playlist']) {
    await page.select(row,true); assert.equal(selected.length,1); assert.equal(page.video,'');
    assert.deepEqual(fs.readdirSync(dir),['chosen']);
  }
  mode='redirect'; await page.refresh(); assert.deepEqual(page.rows,[]); assert.deepEqual(secondCalls,[]);
  mode='ok'; await page.refresh(); const stale = page.client;
  page.changeOrigin(other); assert.equal(page.user+page.password,''); assert.deepEqual(page.rows,[]);
  await assert.rejects(stale.file(row.key,()=>{})); assert.deepEqual(secondCalls,[]);
  await page.refresh(); assert.equal(secondCalls.length,1); assert.equal(secondCalls[0].authorization,undefined);
});

test('bounded listing/file, status before EOF, cancellation and late callbacks',async () => {
  let cb, fn, destroys=0;
  const {RemoteArchiveClient} = clientFor(() => { cb={}; return { on:(k,f)=>cb[k]=f, destroy:()=>destroys++, requestInStream:()=>fn() }; });
  const make=()=>new RemoteArchiveClient('https://camera.test','','');
  const data = s => cb.dataReceive(Uint8Array.from(Buffer.from(s)).buffer);
  fn=async()=>{ cb.dataReceive(new ArrayBuffer(512*1024+1)); return 200; };
  await assert.rejects(make().list(),/长度/);
  fn=async()=>{ data(Array.from({length:513},(_,i)=>`ipc_${i}.mp4 5`).join('\n')); cb.dataEnd(); return 200; };
  await assert.rejects(make().list(),/过多/);
  const c=make(); fn=async()=>{ data('ipc_small.mp4 5\nMD/IPS_big.mp4 536870913\n'); setImmediate(()=>cb.dataEnd()); return 200; };
  await c.list();
  await assert.rejects(c.file('MD/IPS_big.mp4',()=>{}),/512/);
  fn=async()=>{data('1234');cb.dataEnd();return 200;}; await assert.rejects(c.file('ipc_small.mp4',()=>{}),/不完整/);
  fn=async()=>{cb.headersReceive({'Content-Length':'999999999'});return 200;}; await assert.rejects(c.file('ipc_small.mp4',()=>{}),/超限/);
  fn=async()=>{data(android('/media/IPS_test.mkv'));cb.dataEnd();return 200;}; await c.list();
  let delivered=0; const chunk=new ArrayBuffer(1024*1024);
  fn=async()=>{for(let i=0;i<515;i++)cb.dataReceive(chunk);cb.dataEnd();return 200;};
  await assert.rejects(c.file('IPS_test.mkv',()=>delivered++),/超过/); assert.equal(delivered,512);
  fn=async()=>200; const pending=c.file('IPS_test.mkv',()=>assert.fail('late data')); const before=destroys; c.close();
  await assert.rejects(pending,/取消/); cb.dataReceive(chunk); cb.dataEnd(); assert.equal(destroys,before+1);
});
