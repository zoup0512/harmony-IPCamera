// Node 22.18+. Synthetic bytes and credentials only. No DNS lookup or connection to vendor hosts.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const nodeHttp = require('node:http');
const { once } = require('node:events');
const { randomUUID } = require('node:crypto');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const protocol = require('../entry/src/main/ets/util/CloudArchiveProtocol.ts');
const { deviceLogin, archive, finalized, recordingName, CHUNK } = protocol;
const client = randomUUID(), login = { client, token: 'synthetic.token', expires: 86400000 };
const uri = 'https://microsoft.com/devicelogin', code = 'MOCK-CODE';
const challenge = { verification_uri: uri, user_code: code, device_code: '+/mock=&', expires_in: 900, interval: 5 };
const token = { scope: 'Files.ReadWrite.AppFolder', access_token: login.token, token_type: 'Bearer', expires_in: 3600 };
const clock = () => ({ t: 0, delays: [], now() { return this.t; }, async sleep(n) { this.delays.push(n); this.t += n; } });
const noop = () => {};
function scripted(replies, hook = noop) {
  const calls = [];
  return { calls, async send(method, url, headers, body) {
    calls.push({ method, url, headers, body }); hook(calls.length);
    const next = replies.shift();
    if (next instanceof Error) throw next;
    assert.ok(next, 'unexpected HTTP operation');
    return { status: next[0], text: JSON.stringify(next[1]) };
  } };
}
function source(bytes) {
  return { name: 'ipc_2026-11-24.12.00.00.0001_2.mp4', size: bytes.length, check: noop,
    reads: [], async read(pos, n) { this.reads.push([pos, n]); return Uint8Array.from(bytes.subarray(pos, pos+n)).buffer; } };
}
const session = { uploadUrl: 'https://unit.up.1drv.com/upload?opaque=fixture' };
const root = { id: 'app!folder', folder: {} };
const metadata = src => ({ id: 'unit!item', name: src.name, size: src.size, file: {} });

test('synthetic loopback OAuth + Graph: explicit app-only scope, grant polling, aligned ranges, no bearer to session URL, independent readback', async t => {
  const data = Buffer.alloc(CHUNK * 2 + 23, 0x5a), src = source(data), item = metadata(src);
  const requests = [], replies = [[200, challenge], [400, {error:'authorization_pending'}], [400, {error:'slow_down'}], [200, token],
    [200, root], [200, session], [202, {nextExpectedRanges:[`${CHUNK}-`]}], [202, {nextExpectedRanges:[`${CHUNK*2}-`]}], [201, item], [200, item]];
  const server = nodeHttp.createServer(async (req, res) => {
    const chunks = []; for await (const c of req) chunks.push(c);
    requests.push({ method:req.method, path:req.url, headers:req.headers, body:Buffer.concat(chunks) });
    const [status, obj] = replies.shift() ?? [500, {}];
    res.writeHead(status, {'Content-Type':'application/json'}); res.end(JSON.stringify(obj));
  });
  server.listen(0, '127.0.0.1'); await once(server, 'listening');
  t.after(() => new Promise(resolve => server.close(resolve)));
  const net = { async send(method, url, headers, body) {
    const u = new URL(url);
    assert.ok(['login.microsoftonline.com','graph.microsoft.com','unit.up.1drv.com'].includes(u.host));
    // Ignore real authority unconditionally. All requests terminate at this test's ephemeral local port.
    return await new Promise((resolve,reject) => {
      const req = nodeHttp.request({ hostname:'127.0.0.1', port:server.address().port, method,
        path:'/'+u.host+u.pathname+u.search, headers }, res => {
        let text=''; res.on('data', c => {text+=c}); res.on('end', () => resolve({status:res.statusCode,text}));
      });
      req.on('error', reject); req.end(body instanceof ArrayBuffer ? Buffer.from(body) : body);
    });
  } };
  const time = clock(), shown = [];
  const connected = await deviceLogin(net, time, client, noop, d => shown.push(d));
  assert.deepEqual(shown, [{uri,code}]); assert.deepEqual(time.delays, [5000,5000,10000]);
  assert.equal(requests.length, 4); // connect never uploads
  assert.equal(requests[0].path, '/login.microsoftonline.com/common/oauth2/v2.0/devicecode');
  assert.deepEqual(Object.fromEntries(new URLSearchParams(requests[0].body.toString())), {
    client_id:client, scope:'https://graph.microsoft.com/Files.ReadWrite.AppFolder'});
  for (const r of requests.slice(1,4)) {
    assert.equal(r.method,'POST'); assert.equal(r.path, '/login.microsoftonline.com/common/oauth2/v2.0/token');
    assert.deepEqual(Object.fromEntries(new URLSearchParams(r.body.toString())),
      {grant_type:'urn:ietf:params:oauth:grant-type:device_code',client_id:client,device_code:challenge.device_code});
  }
  const progress=[]; await archive(net, connected, time, src, noop, n=>progress.push(n));
  assert.deepEqual(progress,[CHUNK,2*CHUNK,src.size]);
  assert.deepEqual(requests.slice(4).map(r=>r.method), ['GET','POST','PUT','PUT','PUT','GET']);
  assert.match(requests[4].path,/\/special\/approot\?/); assert.match(requests[5].path, /:\/createUploadSession$/);
  assert.equal(JSON.parse(requests[5].body).item['@microsoft.graph.conflictBehavior'],'fail');
  for (const r of [requests[4],requests[5],requests[9]]) assert.equal(r.headers.authorization,'Bearer '+login.token);
  const puts=requests.slice(6,9); assert.deepEqual(Buffer.concat(puts.map(r=>r.body)),data);
  for (let i=0;i<puts.length;i++) {
    const r=puts[i]; assert.equal(r.headers.authorization,undefined); assert.equal(r.path,'/unit.up.1drv.com/upload?opaque=fixture');
    assert.equal(r.headers['content-range'],`bytes ${i*CHUNK}-${i*CHUNK+r.body.length-1}/${data.length}`);
  }
  assert.match(requests[9].path, /\/items\/app!folder:\/ipc_.*\?\$select=id,name,size,file,folder,eTag,parentReference,remoteItem,deleted,package$/);
});

test('device-code validation, denial, cancellation and timeout fail closed', async () => {
  for (const id of ['', 'not-a-guid', '00000000-0000-0000-0000-000000000000']) {
    const net=scripted([]); await assert.rejects(deviceLogin(net,clock(),id,noop,noop)); assert.equal(net.calls.length,0);
  }
  const invalidTenant=scripted([]); await assert.rejects(deviceLogin(invalidTenant,clock(),client,noop,noop,'https://invalid.test'));assert.equal(invalidTenant.calls.length,0);
  const tenant=randomUUID(), single=scripted([[200,challenge],[200,token]]);
  await deviceLogin(single,clock(),client,noop,noop,tenant);assert.ok(single.calls.every(c=>c.url.startsWith(`https://login.microsoftonline.com/${tenant}/oauth2/v2.0/`)));
  const evil=scripted([[200,{...challenge,verification_uri:'https://example.invalid/login'}]]);
  await assert.rejects(deviceLogin(evil,clock(),client,noop,()=>assert.fail('unsafe URI shown')), /challenge/);
  for (const reply of [[400,{error:'authorization_declined'}],[200,{...token,scope:'Files.ReadWrite'}],[500,token]]) {
    await assert.rejects(deviceLogin(scripted([[200,challenge],reply]),clock(),client,noop,noop));
  }
  const timed=scripted([[200,{...challenge,expires_in:3}]]);
  await assert.rejects(deviceLogin(timed,clock(),client,noop,noop),/expired/); assert.equal(timed.calls.length,1);
  let stopped=false; const net=scripted([[200,challenge]]);
  await assert.rejects(deviceLogin(net,clock(),client,()=>{if(stopped)throw Error('stop')},()=>{stopped=true}),/stop/);
  assert.equal(net.calls.length,1);
});

test('remote mismatch, conflicts, unexpected ranges/status, disconnect, changed source and malicious session URL do not succeed or remove source', async () => {
  const bytes=Buffer.alloc(CHUNK+9,5), src=source(bytes), item=metadata(src);
  const prefix=[[200,root],[200,session]], ack=[202,{nextExpectedRanges:[`${CHUNK}-`]}];
  for (const responses of [[...prefix,new Error('network')],[...prefix,[202,{nextExpectedRanges:['0-']}]],
    [...prefix,ack,[409,{}]],[...prefix,ack,[201,{...item,size:1}]], [...prefix,ack,[201,item],[404,{}]],
    [...prefix,ack,[201,item],[200,{...item,id:'other'}]],[[401,{}]]]) {
    const net=scripted(responses); const progress=[];
    await assert.rejects(archive(net,login,clock(),src,noop,n=>progress.push(n)));
    assert.ok(!progress.includes(src.size)); assert.ok(net.calls.every(c=>c.method!=='DELETE'));
  }
  for (const url of ['http://unit.up.1drv.com/upload','https://off.invalid/u','https://unit.up.1drv.com@off.invalid/u','https://unit.up.1drv.com\\@off.invalid/u']) {
    const net=scripted([[200,root],[200,{uploadUrl:url}]]); await assert.rejects(archive(net,login,clock(),src,noop,noop));
    assert.equal(net.calls.length,2);
  }
  let changed=false;
  src.check=()=>{if(changed)throw Error('changed')};
  const net=scripted([...prefix,ack],n=>{changed=n===3}); await assert.rejects(archive(net,login,clock(),src,noop,noop),/changed/);
  assert.equal(net.calls.length,3); assert.equal(bytes.length,CHUNK+9); assert.ok(bytes.every(b=>b===5));
});

test('only real naming conventions and completed top-level MP4 layouts are eligible', async () => {
  const box=(kind,size)=>{const b=Buffer.alloc(size); b.writeUInt32BE(size); b.write(kind,4);return b};
  const f=box('ftyp',16), m=box('mdat',1024), v=box('moov',24);
  const src=source(Buffer.concat([f,m,v])); assert.equal(await finalized(src),true);
  assert.deepEqual(src.reads,[[0,8],[16,8],[1040,8]]);
  for(const data of [Buffer.concat([f,m]),Buffer.concat([f,m,v]).subarray(0,-1), Buffer.from([0,0,0,4,102,114,101,101])])
    assert.equal(await finalized(source(data)),false);
  for(const s of ['ipc_120000.mp4',src.name,'IPS_2026-11-24.12.03.45.0123.mp4'])assert.equal(recordingName(s),true);
  for(const s of ['../ipc_122222.mp4','/MD/IPS_x.mp4','misc.mp4','ipc_120000.mp4\n','IPS_121212.mp4'])assert.equal(recordingName(s),false);
});

// Exercise the production vault and source adapter with synthetic SDK contracts. Never load a device runtime.
const ets = readFileSync(join(__dirname,'../entry/src/main/ets/service/CloudArchiveService.ets'),'utf8');
const sdkModule = new Function('asset','util','fileIo','streaming','cryptoFramework', ...Object.keys(protocol),
  stripTypeScriptTypes(ets.replace(/^import .*;$/gm,'').replace(/^export /gm,'')) + '\nreturn {save,load,forget,recordings,Recording,CloudArchiveService};');
function fixtureFs(fileIo={}, streaming={}) {
  const records=new Map(), Tag=Object.fromEntries(['ALIAS','SECRET','DATA_LABEL_CRITICAL_1','RETURN_LIMIT','RETURN_TYPE','ACCESSIBILITY','SYNC_TYPE','IS_PERSISTENT'].map(k=>[k,k]));
  const sdk={Tag, ReturnType:{ALL:0,ATTRIBUTES:1}, ErrorCode:{NOT_FOUND:24000002}, Accessibility:{DEVICE_UNLOCKED:2}, SyncType:{NEVER:0},
    addSync(row){assert.ok(row.get(Tag.SECRET).length<=1024);assert.equal(row.get(Tag.ACCESSIBILITY),2);assert.equal(row.get(Tag.SYNC_TYPE),0);
      assert.equal(row.get(Tag.IS_PERSISTENT),false);records.set(Buffer.from(row.get(Tag.ALIAS)).toString(),row)},
    querySync(q){const rows=[...records.values()].filter(r=>Buffer.from(r.get(Tag.DATA_LABEL_CRITICAL_1)).equals(q.get(Tag.DATA_LABEL_CRITICAL_1)) &&
      (!q.has(Tag.ALIAS)||Buffer.from(q.get(Tag.ALIAS)).equals(r.get(Tag.ALIAS))));
      if(!rows.length) throw Object.assign(Error('absent'),{code:24000002});
      return q.get(Tag.RETURN_TYPE)===0?rows:rows.slice(0,q.get(Tag.RETURN_LIMIT)).map(r=>new Map([[Tag.ALIAS,r.get(Tag.ALIAS)]]));},
    removeSync(q){assert.deepEqual([...q.keys()],[Tag.ALIAS]);records.delete(Buffer.from(q.get(Tag.ALIAS)).toString())}};
  const u={TextEncoder:class {encodeInto(s){return new TextEncoder().encode(s)}},TextDecoder:{create:()=>({decodeToString:b=>new TextDecoder().decode(b)})}};
  const crypto = {createMd(algorithm) {assert.equal(algorithm,'SHA256'); const h=require('node:crypto').createHash('sha256');
    return {async update({data}) {h.update(data)}, async digest() {return {data:h.digest()}}};}};
  return {api:sdkModule(sdk,u,fileIo,streaming,crypto,...Object.values(protocol)),records,sdk};
}
test('vault handles long tokens, partial writes, expiry, and local revocation without plaintext fallback',()=>{
  const {api,records,sdk}=fixtureFs(), value={...login,expires:Date.now()+100000,token:'a'.repeat(4000)};
  assert.equal(api.load(),undefined); api.save(value);assert.deepEqual(api.load(),value);assert.ok(records.size>4);
  new api.CloudArchiveService().disconnect();assert.equal(records.size,0);assert.equal(api.load(),undefined);
  api.save({...value,expires:1});assert.equal(api.load(),undefined);assert.equal(records.size,0);
  const add=sdk.addSync;sdk.addSync=row=>{if(records.size===2)throw Error('unavailable');add(row)};
  assert.throws(()=>api.save(value));assert.equal(records.size,0);
});
test('production source adapter rejects active, switched, linked and incomplete recordings before HTTP', async()=>{
  let busy=true, inode=1n, link=false, closed=0;
  const name='ipc_123456.mp4';const st=()=>({isFile:()=>!link,ino:inode,size:16,mtime:1,ctime:1});
  const fs={lstatSync:path=>path==='/private'?{isDirectory:()=>true}:path==='/private/MD'?{isDirectory:()=>false}:st(),
    listFileSync:()=>[name,'irrelevant.mp4'],statSync:()=>st(),OpenMode:{READ_ONLY:0},openSync:()=>({fd:3}),
    lseek(){},WhenceType:{SEEK_SET:0},read:async(_fd,b)=>b.byteLength,closeSync(){closed++}};
  const {api}=fixtureFs(fs,{recordingFileBusy:()=>busy}); assert.deepEqual(api.recordings('/private'),[]);
  busy=false;const [row]=api.recordings('/private'); assert.equal(row.name,name); link=true; assert.deepEqual(api.recordings('/private'),[]);link=false;
  inode++;assert.throws(()=>new api.Recording('/private',row));inode--;busy=true;assert.throws(()=>new api.Recording('/private',row));busy=false;
  api.save({...login,expires:Date.now()+60000});await assert.rejects(new api.CloudArchiveService().upload('/private',row,noop),/source/);assert.equal(closed,1);
});

// Full production service state machine with a mock fd, real SHA256 over synthetic MP4 bytes,
// mock vault, and a scripted transport (no loopback, DNS, vendor host or actual recording).
function cleanupFixture() {
  const box=(kind,n)=>{const b=Buffer.alloc(n,7);b.writeUInt32BE(n);b.write(kind,4);return b};
  const data=Buffer.concat([box('ftyp',16),box('mdat',CHUNK+4),box('moov',16)]);
  const sha=require('node:crypto').createHash('sha256').update(data).digest('hex');
  const name='ipc_010203.mp4', st={ino:2,size:data.length,mtime:9,ctime:9,isFile:()=>true};
  const row={name,size:data.length,identity:'2/'+data.length+'/9/9'};
  const state={pos:0,removed:0,current:true,release:0,available:true};
  const fs={lstatSync:()=>st,statSync:()=>st,openSync:()=>({fd:7}),closeSync:noop,OpenMode:{READ_ONLY:0,NOFOLLOW:1},WhenceType:{SEEK_SET:0},
    lseek(fd,pos){assert.equal(fd,7);state.pos=pos}, async read(fd,buf){return data.copy(new Uint8Array(buf),0,state.pos,state.pos+buf.byteLength)}};
  const {api}=fixtureFs(fs,{recordingFileBusy:()=>false,pinCloudRecording(dir,n,fd){assert.deepEqual([dir,n,fd],['/private',name,7]);
    return state.available?{current:()=>state.current,remove:()=>{if(!state.current)return false;state.removed++;return true},close(){state.release++}}:undefined;}});
  const folder={...root,parentReference:{driveId:'drive!one'}}, item={...metadata(row),eTag:'"v1"',parentReference:{driveId:'drive!one',id:root.id},file:{hashes:{sha256Hash:sha.toUpperCase()}}};
  const svc=new api.CloudArchiveService(), grant={...login,expires:Date.now()+60000};api.save(grant);
  const upload=async (readback=item,commit=item)=>{svc.net=scripted([[200,folder],[200,session],[202,{nextExpectedRanges:[`${CHUNK}-`]}],[201,commit],[200,readback]]);svc.net.cancel=noop;
    return svc.upload('/private',row,noop)};
  const probe=(remote=item,app=folder,hook=noop)=>{svc.net=scripted([[200,app],[200,remote]],hook);svc.net.cancel=noop};
  return {svc,row,state,item,folder,grant,api,upload,probe};
}
test('cleanup receipt exists only after strong same-item verification; no deletion on upload', async()=>{
  const f=cleanupFixture();
  for(const hashes of [{},{sha1Hash:'9'.repeat(40)},{quickXorHash:'weak'},{sha256Hash:'0'.repeat(64)},{sha256Hash:'F'.repeat(63)+'\n'}]) {
    assert.equal(await f.upload({...f.item,file:{hashes}}),false); assert.equal(f.svc.canClean(f.row),false);
  }
  for(const patch of [{remoteItem:{}},{deleted:{}},{folder:{}},{package:{}},{eTag:'different'},
    {parentReference:{driveId:'other',id:root.id}},{parentReference:{driveId:'drive!one',id:'other'}}])
    assert.equal(await f.upload({...f.item,...patch}),false);
  f.state.available=false;assert.equal(await f.upload(),false);f.state.available=true;
  assert.equal(await f.upload(),true);assert.equal(f.state.removed,0);assert.equal(f.svc.canClean({...f.row,identity:'stale'}),false);
  f.probe();await f.svc.cleanConfirmed(f.row);assert.equal(f.state.removed,1);
  assert.deepEqual(f.svc.net.calls.map(c=>new URL(c.url).pathname),['/v1.0/me/drive/special/approot','/v1.0/me/drive/items/unit!item']);
  assert.ok(f.svc.net.calls.every(c=>c.method==='GET'&&new URL(c.url).origin==='https://graph.microsoft.com'&&c.headers.Authorization==='Bearer '+login.token));
  await assert.rejects(f.svc.cleanConfirmed(f.row));assert.equal(f.state.removed,1);
});
test('final recheck is fail-closed on replaced item, changed grant, expired auth, cancellation, unavailable/replaced fd or Graph failure',async()=>{
  for(const mode of ['item','tag','sha','size','name','folder','drive','link','grant','expired','cancel','local','status']) {
    const f=cleanupFixture();assert.equal(await f.upload(),true);
    const patch={item:{id:'another'},tag:{eTag:'"v2"'},size:{size:23},sha:{file:{hashes:{sha256Hash:'e'.repeat(64)}}},name:{name:'ipc_000000.mp4'},link:{remoteItem:{}}}[mode]??{};
    f.probe({...f.item,...patch},{...f.folder,...(mode==='folder'?{id:'other'}:mode==='drive'?{parentReference:{driveId:'other'}}:{})},n=>{
      if(n===2) {if(mode==='grant')f.api.save({...f.grant,token:'other.token'});if(mode==='expired')f.api.save({...f.grant,expires:1});
        if(mode==='cancel')f.svc.cancel();if(mode==='local')f.state.current=false;}
    });
    if(mode==='status'){f.svc.net=scripted([[302,{location:'https://evil.invalid'}]]);f.svc.net.cancel=noop;}
    await assert.rejects(f.svc.cleanConfirmed(f.row),mode);assert.equal(f.state.removed,0,mode);assert.equal(f.svc.canClean(f.row),false,mode);
  }
});
