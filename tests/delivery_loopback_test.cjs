// node --test tests/delivery_loopback_test.cjs (Node 22.18+, local openssl for a throwaway test certificate).
// All listeners and connections are hard-pinned to 127.0.0.1; .invalid mailboxes never leave the fixture.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const net = require('node:net');
const tls = require('node:tls');
const { EventEmitter, once } = require('node:events');
const { execFileSync } = require('node:child_process');
const { stripTypeScriptTypes } = require('node:module');
const P = require('../entry/src/main/ets/util/DeliveryProtocol.ts');
const host = '127.0.0.1';
const stamp = '2026-11-24.12.00.01.0001';
const util = { Base64Helper: class { encodeToStringSync(b) { return Buffer.from(b).toString('base64'); } },
  TextEncoder: { create: () => ({ encodeInto: s => Buffer.from(s) }) },
  TextDecoder: { create: () => ({ decodeToString: b => Buffer.from(b).toString() }) } };
function ets(name, injected, exports) {
  const file = path.join(__dirname, '../entry/src/main/ets/util/', name + '.ets');
  const code = fs.readFileSync(file, 'utf8').replace(/^import .*?;$/gm, '').replace(/^export /gm, '');
  return new Function(...Object.keys(injected), stripTypeScriptTypes(code) + '\nreturn {' + exports + '};')(...Object.values(injected));
}
// Exercise actual PlatformWire, including its CRLF queue, timeout handling, certificate and numeric peer checks.
function transport(ca, calls = []) {
  class Adapter extends EventEmitter {
    constructor(encrypted) { super(); this.encrypted = encrypted; }
    async bind(addr) { assert.equal(addr.port, 0); }
    async connect({address, skipRemoteValidation, secureOptions}) {
      assert.equal(address.address, host, 'no fixture may dial any other address');
      calls.push([address.address, address.port, this.encrypted]);
      const opts = { host, port: address.port };
      if (this.encrypted) {
        assert.equal(skipRemoteValidation, false);
        assert.deepEqual(secureOptions.protocols, ['TLSv1.2', 'TLSv1.3']);
      }
      const s = this.encrypted ? tls.connect({...opts, ca, rejectUnauthorized: true}) : net.connect(opts);
      this.sock = s;
      s.on('data', b => this.emit('message', {message: b.buffer.slice(b.byteOffset, b.byteOffset+b.length)}));
      s.on('close', () => this.emit('close')); s.on('error', () => this.emit('error'));
      await once(s, this.encrypted ? 'secureConnect' : 'connect');
    }
    async getRemoteCertificate() { return { encodingFormat: 0, data: this.sock.getPeerCertificate().raw }; }
    async getRemoteAddress() { return { address: this.sock.remoteAddress }; }
    async send(x) {
      const data = this.encrypted ? x : x.data;
      await new Promise((ok, no) => this.sock.write(typeof data === 'string' ? data : Buffer.from(data), e => e ? no(e) : ok()));
    }
    async close() { if (this.sock && !this.sock.destroyed) await new Promise(ok => this.sock.end(ok)); }
  }
  const socket = { Protocol: {TLSv12: 'TLSv1.2', TLSv13: 'TLSv1.3'}, constructTLSSocketInstance: () => new Adapter(true),
    constructTCPSocketInstance: () => new Adapter(false) };
  const cert = { EncodingFormat: { FORMAT_DER: 0 }, GeneralNameType: { GENERAL_NAME_TYPE_IP_ADDRESS: 7, GENERAL_NAME_TYPE_DNS_NAME: 2 },
    async createX509Cert(peer) {
      const x509 = new (require('node:crypto').X509Certificate)(peer.data);
      return {match: ({subjectAlternativeNames: [san]}) => san.type === 7 && san.name[0] === 135 && san.name[1] === 4 &&
        !!x509.checkIP(Array.from(san.name.slice(2)).join('.'))};
    } };
  return ets('DeliverySocket', { ...P, util, cert, socket }, 'PlatformDialer,matchesSan');
}
async function listen(t, server) {
  const sockets = new Set();
  server.on('connection', s => { sockets.add(s); s.on('error', () => {}); s.on('close', () => sockets.delete(s)); });
  server.listen(0, host);
  await once(server, 'listening');
  t.after(() => { for (const s of sockets) s.destroy(); return new Promise(ok => server.close(ok)); });
  return String(server.address().port);
}
function lines(s, handler) {
  let buf = '';
  s.on('data', b => { buf += b.toString(); let i;
    while ((i = buf.indexOf('\r\n')) !== -1) { const line = buf.slice(0, i); buf = buf.slice(i+2); handler(line); }
  });
}
function part(name, buf) {
  let at = 0, calls = 0;
  return { name, size: buf.length, async read(n) { assert.ok(n <= 65536); calls++; const b = buf.subarray(at, at+n); at += b.length; return b; },
    get calls() { return calls; } };
}
function smtpConfig(port) { return Object.assign(new P.MailSettings(), {enabled:true, host, port, user:'mock', pass:'fixture', boundTo:'smtps://'+host+':'+port, from:'cam@camera.invalid', to:'sink@camera.invalid'}); }
function ftpConfig(port) { return Object.assign(new P.FtpSettings(), {enabled:true, host, port, user:'mock', pass:'fixture', dir:'clips', boundTo:'ftp://'+host+':'+port, consent:host+':'+port}); }
test('default off; wrong consent, path, size, or command injection rejected before dial', async () => {
  const d = {dial() { assert.fail('network forbidden'); }};
  const c = smtpConfig('465'), f = ftpConfig('21');
  for (const o of [new P.MailSettings(), {...c, host:'x/y'}, {...c, to:'a@a.invalid\r\nDATA'}, {...c, user:'x\0bad'}]) await assert.rejects(P.mail(d,o), P.DeliveryError);
  for (const o of [new P.FtpSettings(), {...f, consent:''}, {...f, port:'22'}, {...f, dir:'../logs'}, {...f, pass:'x\r\nNOOP'}]) await assert.rejects(P.ftp(d,o), P.DeliveryError);
  const name = 'IPS_'+stamp+'.bmp', mp4 = 'ipc_'+stamp+'_2.mp4';
  for (const p of ['/tmp/'+name,'/files/../MD/'+name,'/files/'+name,'file:///files/MD/'+name,'/files/MD/'+name+'\n']) assert.equal(P.artifactName('/files',p),'');
  assert.equal(P.artifactName('/files','/files/'+mp4),mp4);
  await assert.rejects(P.mail(d,c,{name,size:P.MAIL_LIMIT+1}), /12 MiB/);
  await assert.rejects(P.ftp(d,f,{name:mp4,size:P.FTP_LIMIT+1}), /512 MiB/);
});
test('destination edits invalidate both inputs, old child callbacks, enable and FTP consent even across a round trip', async () => {
  const src=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/pages/Delivery.ets'),'utf8');
  const methods=src.slice(src.indexOf('  private destination('),src.indexOf('  private async run('));
  const Controller=new Function(...Object.keys(P),stripTypeScriptTypes('class Controller {\n'+ methods +'\n}')+'\nreturn Controller;')(...Object.values(P));
  for(const isMail of [true,false]) {
    const ctl=new Controller();ctl.busy=false;ctl.mailEpoch=0;ctl.ftpEpoch=0;ctl.smtp=smtpConfig('465');ctl.upload=ftpConfig('21');
    const c=isMail?ctl.smtp:ctl.upload,epoch=()=>isMail?ctl.mailEpoch:ctl.ftpEpoch,check=isMail?P.checkMail:P.checkFtp;
    const port=c.port; ctl.destination(isMail,'camera.invalid',port);
    ctl.accountInput(isMail,epoch(),true,'original-user');ctl.accountInput(isMail,epoch(),false,'original-password');
    c.enabled=true;if(!isMail)c.consent=P.endpoint(c.host,c.port); check(c);
    const old=epoch(); ctl.destination(isMail,' Camera.Invalid ',port);assert.equal(epoch(),old);check(c);
    ctl.destination(isMail,c.host,'9');ctl.destination(isMail,'other.invalid',port);ctl.destination(isMail,'camera.invalid',port);
    assert.equal(c.enabled,false);assert.equal(c.user,'');assert.equal(c.pass,'');assert.equal(c.boundTo,'');if(!isMail)assert.equal(c.consent,'');
    for(const isUser of [true,false])ctl.accountInput(isMail,old,isUser,'delayed old component text');
    assert.equal(c.user+c.pass,'');c.enabled=true;if(!isMail)c.consent=P.endpoint(c.host,c.port);assert.throws(()=>check(c));
    ctl.accountInput(isMail,epoch(),true,'new-user');assert.equal(c.pass,'');
    ctl.accountInput(isMail,epoch(),false,'new-password');c.enabled=true;if(!isMail)c.consent=P.endpoint(c.host,c.port);check(c);
    const stale={...c,boundTo:(isMail?'ftp://':'smtps://')+P.endpoint(c.host,c.port)};
    await assert.rejects((isMail?P.mail:P.ftp)({dial(){assert.fail('must reject before dialing');}},stale),P.DeliveryError);
    delete stale.boundTo;P.restoreBinding(isMail,stale);assert.equal(stale.user+stale.pass,'');assert.equal(stale.enabled,false);
  }
  for(const [kind,label] of [['true','mailEpoch'],['false','ftpEpoch']]) {
    assert.ok(src.includes('ForEach([this.'+label+']'));
    for(const isUser of ['true','false']) assert.ok(src.includes('this.accountInput('+kind+', epoch, '+isUser+', v)'));
  }
});
test('certificate SAN matches only exact DNS, one-label wildcard, or IPv4 iPAddress', () => {
  const validate = transport().matchesSan;
  const matchesSan = (h,b) => validate(h,b,() => true);
  const san = (t,s) => {const v = Buffer.from(s); return Buffer.concat([Buffer.from([48,v.length+2,t,v.length]),v]);};
  assert.ok(matchesSan('Mail.example.invalid',san(130,'*.example.invalid')));
  for (const h of ['a.b.example.invalid','example.invalid','example.invalid.evil']) assert.ok(!matchesSan(h,san(130,'*.example.invalid')));
  for (const t of [129,134]) assert.ok(!matchesSan('mail.invalid',san(t,'mail.invalid')));
  assert.ok(!matchesSan(host,san(130,host)));
  const plain=san(130,'mail.invalid');
  assert.ok(!matchesSan('mail.invalid',Buffer.concat([Buffer.from([4,plain.length]),plain])),'reject double-wrapped extnValue');
  assert.ok(!validate('mail.invalid',plain,() => false),'platform typed SAN rejection must fail closed');
  assert.throws(() => matchesSan('mail.invalid',Buffer.from([48,128])));
});
test('implicit TLS SMTP: untrusted certificate blocked, valid TLS accepted; streamed MIME roundtrip and test without file', {timeout:20000}, async t => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(),'ipcam-mock-tls-'));
  t.after(() => fs.rmSync(tmp,{recursive:true,force:true}));
  const key = path.join(tmp,'key.pem'), crt = path.join(tmp,'cert.pem');
  execFileSync('openssl',['req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=fixture.invalid','-addext','subjectAltName=IP:127.0.0.1','-keyout',key,'-out',crt],{stdio:'ignore'});
  const ca = fs.readFileSync(crt); const bodies=[]; const commands=[];
  const server=tls.createServer({cert:ca,key:fs.readFileSync(key)},s => {
    let data=false,body=[];
    s.write('220 mock'); setImmediate(() => s.write('\r\n'));
    lines(s,line=>{
      if (data) { if (line!=='.') {body.push(line); return;} data=false; bodies.push(body); s.write('250 accepted\r\n'); return; }
      commands.push(line.split(' ')[0]);
      if (line.startsWith('EHLO ')) s.write('250-fixture\r\n250 AUTH PLAIN\r\n');
      else if (line.startsWith('AUTH PLAIN ')) s.write('235 ok\r\n');
      else if (line.startsWith('MAIL FROM:') || line.startsWith('RCPT TO:')) s.write('250 ok\r\n');
      else if (line==='DATA') {data=true;body=[];s.write('354 send\r\n');}
      else s.write('500 error\r\n');
    });
  });
  const c=smtpConfig(await listen(t,server));
  await assert.rejects(P.mail(new (transport().PlatformDialer)(() => true), c), /TLS|证书/);
  assert.deepEqual(commands,[]);
  const d=new (transport(ca).PlatformDialer)(() => true);
  await P.mail(d,c);
  const bytes=Buffer.alloc(150007); for(let i=0;i<bytes.length;i++) bytes[i]=i%251;
  const f=part('IPS_'+stamp+'.bmp',bytes);
  await P.mail(d,c,f);
  assert.ok(f.calls>1); assert.doesNotMatch(bodies[0].join('\n'), /multipart|filename=/);
  const headers=bodies[1]; assert.ok(headers.includes('Content-Type: image/bmp'));
  const at=headers.indexOf('Content-Transfer-Encoding: base64')+2;
  const encoded=headers.slice(at,-1); assert.ok(encoded.every(s=>s.length<=76));
  assert.deepEqual(Buffer.from(encoded.join(''),'base64'),bytes);
});
test('FTP loopback: login-only test, PASV foreign host ignored; exact bytes and publish only after 226', {timeout:10000}, async t=>{
  const calls=[],cmds=[],received=[]; let ctrl;
  const dataPort=Number(await listen(t,net.createServer(s=>{s.on('data',b=>received.push(b)); s.on('end',()=>ctrl.write('226 stored\r\n'));})));
  const port=await listen(t,net.createServer(s=>{ctrl=s;s.write('220 mock\r\n');lines(s,line=>{
    const k=line.split(' ')[0]; cmds.push(k); const reply={USER:'331 pass',PASS:'230 ok',CWD:'250 ok',TYPE:'200 ok',PWD:'257 "/clips"',EPSV:'502 unavailable',
      PASV:`227 Entering Passive Mode (203,0,113,88,${dataPort>>8},${dataPort&255})`,STOR:'150 go',RNFR:'350 ok',RNTO:'250 ok'}[k];
    s.write((reply||'500 rejected')+'\r\n');
  });}));
  const d=new (transport(undefined,calls).PlatformDialer)(() => true), c=ftpConfig(port);
  await P.ftp(d,c); assert.deepEqual(cmds,['USER','PASS','CWD','TYPE','PWD']);
  const bytes=Buffer.alloc(200001,41), f=part('ipc_'+stamp+'_2.mp4',bytes); await P.ftp(d,c,f);
  assert.ok(f.calls>1);assert.deepEqual(Buffer.concat(received),bytes);assert.deepEqual(cmds.slice(-3),['STOR','RNFR','RNTO']);
  assert.deepEqual(calls.map(x=>x[0]),[host,host,host]);
});
test('ambiguous transfer failure never promotes remote partial; response text and credentials not exposed',async()=>{
  const sent=[]; const replies=['220 hi','331 password','230 ok','250 cwd','200 binary','229 (|||4011|)','150 go','451 SECRET_REMOTE_REPLY'];
  const wire={ line:async()=>replies.shift(),write:async x=>sent.push(x),close:async()=>{},peer:async()=>host};
  const d={dial:async()=>wire};
  await assert.rejects(P.ftp(d,ftpConfig('21'),part('ipc_'+stamp+'.mp4',Buffer.from('foo'))),e=>!e.message.includes('SECRET')&&e.message.includes('451'));
  assert.ok(!sent.some(x=>typeof x==='string' && /^(RNFR|RNTO|DELE)/.test(x)));
});

test('ASSET-only settings: off by default, non-sync unlocked records, no credentials in labels; consent changes fail closed', async()=>{
  const records = new Map();
  const asset = {Tag: Object.fromEntries(['ALIAS','SECRET','RETURN_TYPE','SYNC_TYPE','ACCESSIBILITY','CONFLICT_RESOLUTION'].map(k=>[k,k])),
    ReturnType:{ALL:1}, SyncType:{NEVER:0}, Accessibility:{DEVICE_UNLOCKED:2}, ConflictResolution:{OVERWRITE:0}, ErrorCode:{NOT_FOUND:1},
    async query(q) {const row=records.get(Buffer.from(q.get('ALIAS')).toString()); if (!row) throw {code:1}; return [row];},
    async add(row) { assert.equal(row.get('ACCESSIBILITY'),2); assert.equal(row.get('SYNC_TYPE'),0);
      assert.equal(row.get('CONFLICT_RESOLUTION'),0); records.set(Buffer.from(row.get('ALIAS')).toString(),row); }};
  const s=ets('DeliveryStore',{...P,asset,util},'loadMail,loadFtp,saveMail,saveFtp,revision');
  assert.equal((await s.loadMail()).enabled,false); assert.equal((await s.loadFtp()).consent,'');
  await s.saveFtp(ftpConfig('21')); const c=await s.loadFtp(); assert.equal(c.pass,'fixture'); assert.equal(s.revision(false),1);
  for (const row of records.values()) for(const [tag,v] of row) if(tag!=='SECRET') assert.doesNotMatch(String(Buffer.from(String(v))), /fixture|mock/);
  c.host='different.invalid'; await assert.rejects(s.saveFtp(c),/确认/); assert.equal(s.revision(false),1);
  c.enabled=false; await assert.rejects(s.saveFtp(c),P.DeliveryError); // never persist misbound secrets even disabled
  P.restoreBinding(false,c); await s.saveFtp(c); assert.equal((await s.loadFtp()).consent,'');
  await assert.rejects(s.saveMail({...smtpConfig('465'),pass:'密'.repeat(400)}),P.DeliveryError);
});

test('service consumes only closed app media; rejects live, unissued, linked, changed and repeated files without removing anything',async()=>{
  const ctx={filesDir:'/private'}, files=new Map(), handles=new Map(), sent=[];
  const md='/private/MD', snapshot=md+'/IPS_'+stamp+'.bmp', recording='/private/ipc_'+stamp+'.mp4';
  const mkstat=(isDir=false, buf=Buffer.alloc(0))=>({ isDirectory:()=>isDir,isFile:()=>!isDir,ino:123n,size:buf.length,mtime:42,ctimeNs:42n });
  let ftpOn=false,busy=false,links=false,changed=false,closed=0;
  const io={OpenMode:{READ_ONLY:0,NOFOLLOW:1,NONBLOCK:2},WhenceType:{SEEK_SET:0},accessSync:p=>p===md||files.has(p),
    lstatSync(p){ if(p===md)return {isDirectory:()=>!links}; if(!files.has(p))throw {code:13900002}; return mkstat(false,files.get(p)); },
    openSync(p,flags){assert.equal(flags,3);const fd=handles.size+1;handles.set(fd,{p,pos:0});return {fd,path:p};},
    statSync(fd){return this.lstatSync(handles.get(fd).p);},readSync(fd,b){ const h=handles.get(fd),n=files.get(h.p).copy(Buffer.from(b),0,h.pos,h.pos+b.byteLength);h.pos+=n;return n;},
    async read(fd,b){ const n=this.readSync(fd,b); if(changed)files.set(handles.get(fd).p,Buffer.alloc(200));return n;},
    lseek(fd,offset){handles.get(fd).pos=offset;},closeSync(file){assert.ok(handles.delete(file.fd));closed++;} };
  const consume=async(_d,_c,p)=>{const b=[];for(let pos=0;pos<p.size;pos+=2048)b.push(await p.read(Math.min(2048,p.size-pos)));sent.push(Buffer.concat(b));};
  const service=ets('DeliveryService',{...P, fileIo:io,streaming:{recordingFileBusy:()=>busy},revision:()=>0,
    loadMail:async()=>smtpConfig('465'),loadFtp:async()=>Object.assign(ftpConfig('21'),{enabled:ftpOn}),PlatformDialer:class {},mail:consume,ftp:consume},
    'deliveryMotionSnapshotPath,onDeliveryCompleted,lastDeliveryResult');
  const send=p=>service.onDeliveryCompleted(ctx,p);
  assert.equal(await send(snapshot),''); assert.equal(await send(recording),''); assert.equal(await send('/etc/random.mp4'),'');
  assert.equal(closed,0); assert.equal(await service.deliveryMotionSnapshotPath(ctx,stamp),snapshot);
  const img=Buffer.alloc(7000,16);img.write('BM');img.writeUInt32LE(img.length,2); files.set(snapshot,img);
  assert.match(await send(snapshot),/已由服务器接受/);assert.deepEqual(sent[0],img);assert.equal(await send(snapshot),'');
  ftpOn=true; busy=true; assert.match(await send(recording),/仍在写入/);busy=false;
  const vid=Buffer.alloc(7001,88);vid.write('ftyp',4);files.set(recording,vid);
  const [a,b]=await Promise.all([send(recording),send(recording)]);assert.match(a,/已上传/);assert.equal(b,'');assert.deepEqual(sent[1],vid);
  const segment=recording.replace('.mp4','_2.mp4');files.set(segment,vid); changed=true;assert.match(await send(segment),/已更改/); changed=false;
  links=true; assert.match(await send(snapshot.replace('.bmp','.mp4')),/符号链接/);
  assert.equal(closed,3);assert.deepEqual(files.get(recording),vid);assert.deepEqual(files.get(snapshot),img);
});
