// Synthetic SDK, interface, certificate, gateway and UI only. No network APIs or device tools.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const {stripTypeScriptTypes} = require('node:module');
const {createHash} = require('node:crypto');
const main = join(__dirname, '../entry/src/main');
const source = f => readFileSync(join(main, 'ets', f+'.ets'), 'utf8').replace(/^import .*;$/gm,'').replace(/^export /gm,'');
const code = ['model/IgdXml','model/IgdLease','service/IgdGateway'].map(source).join('\n');
const load = new Function('connection','socket','cert','cryptoFramework','setTimeout','clearTimeout',stripTypeScriptTypes(code) +
  '\nreturn {LeaseRequest,IgdGateway,leaseBindings,managedProfile,leaseCredentials,wifiRoute,igdEndpoint,igdXml,xmlReply};');
const host = '192.168.51.1', local = '192.168.51.45', origin = 'https://'+host+':8443';
const der = Buffer.from('synthetic-leaf-der');
const profile = {description:origin+'/root.xml', control:origin+'/control',leafSha256:createHash('sha256').update(der).digest('hex')};
const challenge = 'ab'.repeat(16), wan = 'urn:schemas-upnp-org:service:WANIPConnection:2';
const service = `<service><serviceType>${wan}</serviceType><controlURL>/control</controlURL><m:createOnlyChallenge xmlns:m="urn:ipcamera:igd:create-only:1">${challenge}</m:createOnlyChallenge></service>`;
const device = (n,child) => `<device><deviceType>urn:schemas-upnp-org:device:${n}:2</deviceType>${child}</device>`;
const devices = d => `<deviceList>${d}</deviceList>`;
const desc = `<root xmlns="urn:schemas-upnp-org:device-1-0">${device('InternetGatewayDevice',devices(device('WANDevice',devices(device('WANConnectionDevice','<serviceList>'+service+'</serviceList>')))))}</root>`;
const success = `<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:AddAnyPortMappingResponse xmlns:u="${wan}"><NewReservedPort>62033</NewReservedPort></u:AddAnyPortMappingResponse></s:Body></s:Envelope>`;
const framed = s => `HTTP/1.1 200 OK\r\nContent-Type: text/xml; charset=utf-8\r\nContent-Length: ${s.length}\r\n\r\n${s}`;
const addr=(address,prefixLength)=>({address:{address},prefixLength});
function rig() {
  const calls=[], writes=[], deadlines=new Map(); let seq=0;
  const props = {interfaceName:'wlan0',linkAddresses:[addr(local,24)],routes:[
    {interface:'wlan0',destination:addr('0.0.0.0',0),gateway:{address:host},isDefaultRoute:true,hasGateway:true},
    {interface:'wlan0',destination:addr('192.168.51.0',24),hasGateway:false}]};
  const caps = {bearerTypes:[1]}, net={netId:5,async bindSocket(s){calls.push('bind-net');assert.ok(s.fake)}};
  const os={NetBearType:{BEARER_WIFI:1},getDefaultNetSync:()=>net,getNetCapabilitiesSync:()=>caps,getConnectionPropertiesSync:()=>props};
  const cfg={desc:framed(desc),reply:framed(success),der};
  const socket={Protocol:{TLSv12:12,TLSv13:13},constructTCPSocketInstance(){ calls.push('socket'); return {fake:true,
    async bind(a){assert.equal(a.address,local);calls.push('bind-local')},async connect(o){assert.deepEqual(o.address,{address:host,port:8443,family:1});calls.push('tcp');},
    async getLocalAddress(){return {address:local}}, async close(){}}; },constructTLSSocketInstance(t){assert.ok(t.fake);const events={}; return {
    on(n,cb){events[n]=cb},async connect(o){assert.equal(o.skipRemoteValidation,false);assert.deepEqual(o.secureOptions.protocols,[12,13]);},
    async getRemoteCertificate(){await cfg.beforePin?.(writes.length);return {encodingFormat:1,data:cfg.der}},async close(){},async send(w){
      writes.push(w); const post=w.startsWith('POST'); cfg.onWrite?.(post); const r=post?cfg.reply:cfg.desc;
      if (r===null) { [...deadlines.values()].at(-1)(); return; }
      for(const chunk of [r.slice(0,21),r.slice(21)]) events.message({message:Uint8Array.from(Buffer.from(chunk)).buffer});
    }}; }};
  const cert={EncodingFormat:{FORMAT_DER:1},async createX509Cert(c){return {getEncoded:async()=>c}}};
  const crypto={createMd(n){assert.equal(n,'SHA256');const h=createHash(n);return {updateSync:b=>h.update(b.data),digestSync:()=>({data:h.digest()})}}};
  const lib=load(os,socket,cert,crypto,fn=>{deadlines.set(++seq,fn);return seq},id=>deadlines.delete(id));
  const wire=new lib.IgdGateway(), p=lib.managedProfile(JSON.stringify(profile));
  const bind=()=>lib.leaseBindings.set('HTTP',39222,true,()=>true);bind();
  const draft=()=>new lib.LeaseRequest('HTTP',p.description,p,challenge,wire);
  return {lib,p,wire,draft,bind,calls,writes,props,caps,net,cfg};
}
test('actual adapter with synthetic Wi-Fi/TLS: review is passive; exactly one GET and one bounded fresh-only SOAP write',async()=>{
  const r=rig(), d=r.draft();await assert.rejects(d.finalConfirmation());d.firstConfirmation();assert.deepEqual(r.calls,[]);
  const result=d.finalConfirmation();await assert.rejects(d.finalConfirmation());assert.equal(await result,62033);
  assert.deepEqual(r.writes.map(w=>w.split('\r\n')[0]),['GET /root.xml HTTP/1.1','POST /control HTTP/1.1']);
  assert.ok(!r.writes[0].includes(challenge));assert.equal(r.calls.filter(x=>x==='bind-net').length,2);
  const post=r.writes[1]; for(const s of [`SOAPAction: "${wan}#AddAnyPortMapping"`,'<NewExternalPort>0<','<NewProtocol>TCP<','<NewInternalPort>39222<',`<NewInternalClient>${local}<`,'<NewLeaseDuration>60<',`IPCamera-${challenge}<`]) assert.ok(post.includes(s),s);
  assert.throws(r.draft);r.lib.leaseBindings.stopAll();assert.equal(r.writes.length,2);
});
test('wrong cert or managed URL, missing profile, untrusted description or redirect never dispatches a mutation',async()=>{
  const bad=[desc.replace(challenge,'bad'),desc.replace(/WANIPConnection:2/g,'WANIPConnection:1'),desc.replace('InternetGatewayDevice:2','InternetGatewayDevice:1'),
    desc.replace('/control','//other/control'),desc.replace('/control',origin.replace('8443','8444')+'/control'),desc.replace('/control','/unreviewed'),desc.replace('/control',origin.replace(host,'192.168.51.2')+'/control'),
    desc.replace(service,service+service),desc.replace('<device>','<URLBase>https://bad/</URLBase><device>'), '<!DOCTYPE x>'+desc,desc.replace('<device>','<device xml:base="/x">'),desc.replace('<deviceType>','<deviceType>&x;')];
  for(const raw of [...bad.map(framed),'HTTP/1.1 307 Redirect\r\nLocation: https://example.invalid\r\n\r\n']) {
    const r=rig();r.cfg.desc=raw;const d=r.draft();d.firstConfirmation();await assert.rejects(d.finalConfirmation());assert.equal(d.attempted,false);assert.equal(r.writes.length,1);
  }
  const r=rig();r.cfg.der=Buffer.from('other');const d=r.draft();d.firstConfirmation();await assert.rejects(d.finalConfirmation());assert.equal(r.writes.length,0);
  assert.throws(()=>r.lib.managedProfile('{}'));assert.throws(()=>new r.lib.LeaseRequest('HTTP',origin+'/other',profile,challenge,r.wire));
});
test('route, address and credential checks fail closed before dial',()=>{
  for(const change of [r=>r.caps.bearerTypes=[1,3],r=>r.caps.bearerTypes=[0],r=>r.props.routes.pop(),r=>r.props.routes[0].gateway.address=local,
    r=>r.props.routes.push(r.props.routes[0]),r=>r.props.routes[1].isExcludedRoute=true,r=>r.props.routes[1].interface='eth0',r=>r.props.linkAddresses=[addr('192.168.52.1',24)],
    r=>r.props.linkAddresses=[addr('192.168.51.255',24)],r=>r.props.routes.push({...r.props.routes[1],hasGateway:true,destination:addr(host,32)})]){
    const r=rig();change(r);assert.throws(r.draft);assert.deepEqual(r.calls,[]);
  }
  const {lib}=rig();for(const u of ['http://192.168.51.1/x','https://127.0.0.1/x','https://name.test/x','https://192.168.51.01/x','https://192.168.51.1:65536/x',origin+'/a/%2f',origin+'/a/../b',origin+'//x',origin+'/x?q=1',origin+'/x\n']) assert.throws(()=>lib.igdEndpoint(u));
  for(const pw of ['admin','Password123456','aaaaaa123456','Aa1!'.repeat(20),'AaBC1234\x00zzz'])assert.equal(lib.leaseCredentials('operator',pw),false);
  assert.equal(lib.leaseCredentials('operator','Unique-Fake12345'),true);
  for(const port of [0,65536,1.5,NaN]){lib.leaseBindings.set('HTTP',port,true,()=>true);assert.equal(lib.leaseBindings.current('HTTP'),undefined)}
  lib.leaseBindings.set('HTTP',12345,false,()=>true);assert.equal(lib.leaseBindings.current('HTTP'),undefined);
});
test('cancel, stop, native listener loss, same-port restart, network change or pin change during second handshake suppresses POST',async()=>{
  for(const how of ['cancel','stop','restart','network','pin','dead']) {
    const r=rig();let live=true;r.lib.leaseBindings.set('HTTP',39222,true,()=>live);const d=r.draft();d.firstConfirmation();
    r.cfg.beforePin=n=>{if(n!==1)return; if(how==='cancel')d.cancel();if(how==='stop')r.lib.leaseBindings.stop('HTTP');if(how==='restart')r.bind();if(how==='network')r.net.netId++;if(how==='pin')r.cfg.der=Buffer.from('other');if(how==='dead')live=false;};
    await assert.rejects(d.finalConfirmation());assert.equal(d.attempted,false);assert.equal(r.writes.length,1);
  }
});
test('timeout after send, malformed acknowledgement, fault, bogus reserved port and redirects remain unknown; never resend/delete',async()=>{
  for (const reply of [null,framed(success.replace('62033','0')),framed(success.replace('62033','65536')),framed(success.replace('AddAnyPortMappingResponse','Fault')),framed('<bad/>'),'HTTP/1.1 302 Found\r\n\r\n']) {
    const r=rig();r.cfg.reply=reply;const d=r.draft();d.firstConfirmation();await assert.rejects(d.finalConfirmation());assert.equal(d.attempted,true);await assert.rejects(d.finalConfirmation());assert.throws(r.draft);r.lib.leaseBindings.stopAll();assert.equal(r.writes.length,2);
  }
});
test('page is passive until both dialogs; missing signed profile, cancel, hide or stale dialog never contacts the gateway',async()=>{
  const r=rig(), dialogs=[]; let installed=false;
  const pageSrc=source('pages/IgdLeaseSettings').split('\n  build()')[0].replace(/^@(Entry|Component)\s*$/gm,'').replace('struct IgdLeaseSettings','class IgdLeaseSettings').replaceAll('@State ','')+'}';
  const page=new Function('getContext','util','cryptoFramework','managedProfile','LeaseRequest','IgdGateway','$r',stripTypeScriptTypes(pageSrc)+';return new IgdLeaseSettings()')(
    ()=>({resourceManager:{getRawFileContentSync(name){assert.equal(name,'igd-create-only.json');if(!installed)throw Error('missing');return Buffer.from(JSON.stringify(profile));}}}),
    {TextDecoder:{create:()=>({decodeToString:b=>b.toString()})}},{createRandom:()=>({generateRandomSync:n=>({data:Buffer.alloc(n,0xab)})})},r.lib.managedProfile,r.lib.LeaseRequest,r.lib.IgdGateway,(id,...args)=>({id,args}));
  page.getUIContext=()=>({showAlertDialog:d=>dialogs.push(d)});page.aboutToAppear();page.enable(true);page.review('HTTP');assert.equal(page.optedIn,false);assert.equal(dialogs.length,0);
  installed=true;page.aboutToAppear();assert.equal(page.optedIn,false);page.enable(true);page.url=profile.description;page.review('HTTP');const stale=dialogs.pop();stale.primaryButton.action();stale.secondaryButton.action();assert.equal(dialogs.length,0);
  page.review('HTTP');dialogs.pop().secondaryButton.action();const second=dialogs.pop();page.onPageHide();second.secondaryButton.action();assert.deepEqual(r.calls,[]);
  page.enable(true);page.url=profile.description;page.review('HTTP');dialogs.pop().secondaryButton.action();const final=dialogs.pop();assert.deepEqual(final.message.args,['HTTP',local,39222,host,8443]);
  assert.deepEqual(r.calls,[]);await Promise.all([final.secondaryButton.action(),final.secondaryButton.action()]);assert.equal(r.writes.length,2);assert.equal(page.optedIn,false);assert.deepEqual(page.status.args,[62033]);
});
test('IGD page resources agree in both locales; route is registered and separate from mDNS',()=>{
  const all=source('pages/IgdLeaseSettings');const keys=new Set([...all.matchAll(/app\.string\.(igd_\w+)/g)].map(m=>m[1]));
  const cat=l=>new Map(JSON.parse(readFileSync(join(main,'resources',l,'element','igd.json'))).string.map(e=>[e.name,e.value]));
  const en=cat('base'),zh=cat('zh_CN');for(const c of [en,zh])assert.deepEqual(new Set(c.keys()),keys);
  for(const [k,v] of en)assert.deepEqual(v.match(/%[sd]/g),zh.get(k).match(/%[sd]/g));
  assert.ok(JSON.parse(readFileSync(join(main,'resources/base/profile/main_pages.json'))).src.includes('pages/IgdLeaseSettings'));
  assert.match(source('pages/LanServiceSettings'),/pushUrl\(\{ url: 'pages\/IgdLeaseSettings'/);
});
test('restricted XML and fixed HTTP framing reject ambiguous representations',()=>{
  const {lib}=rig();for(const xml of ['<a/><b/>','<a>','<u:a/>','<a x="1" x="2"/>','<a xmlns:u="urn:1" xmlns:u="urn:2"/>','<?include evil?><a/>','<!DOCTYPE x><a/>','<a>&amp;</a>','<a xmlns="http://www.w3.org/2001/XInclude"/>','<a><b/>text</a>','<a>'.repeat(26)+'</a>'.repeat(26)])assert.throws(()=>lib.igdXml(xml));
  for(const h of ['Content-Length: 5','Content-Encoding: gzip','Transfer-Encoding: chunked','Location: /other']) assert.throws(()=>lib.xmlReply(framed('<a/>').replace('Content-Type:',h+'\r\nContent-Type:')));
});
