// Node 22.18+ and local Python 3. Real session/validation/serialization code;
// only SDK boundaries substituted. Synthetic SOAP, no sockets (not even discovery).
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { stripTypeScriptTypes } = require('node:module');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { execFileSync } = require('node:child_process');
const { randomBytes, createHash } = require('node:crypto');
const SOAP = 'http://www.w3.org/2003/05/soap-envelope';
const TT = 'http://www.onvif.org/ver10/schema';
const MEDIA = 'http://www.onvif.org/ver10/media/wsdl';
const DEVICE = 'http://www.onvif.org/ver10/device/wsdl';
const origin = 'http://127.0.0.1:12001';
const dev = origin + '/device', media = origin + '/media';
const password = 'synthetic-password-only';
const user = 'unit<&';
const model = ['OnvifSoap', 'OnvifEncoder', 'OnvifManagedSession'].map(file => readFileSync(
  join(__dirname, '../entry/src/main/ets/model', file + '.ets'), 'utf8').replace(/^import .*;\r?$/gm, '').replace(/^export /gm, '')).join('\n');
const load = new Function('http','util','cryptoFramework','convertxml', stripTypeScriptTypes(model, {mode:'transform'}) + '\nreturn {OnvifManagedSession,endpoint,response};');
// Independent, standard XML parser behind the convertxml adapter. No DTD is accepted by production code.
const script = `import sys,json,xml.dom.minidom as dom
n=dom.parseString(sys.stdin.buffer.read()).documentElement
def to(e):
 if e.nodeType==1: return dict(type='element',name=e.tagName,attributes=dict(e.attributes.items()),elements=[to(c) for c in e.childNodes if c.nodeType in (1,3,4)])
 return dict(type='text',text=e.data)
print(json.dumps(dict(elements=[to(n)])))`;
const cache = new Map();
const convertxml = { ConvertXML: class { fastConvertToJSObject(xml) {
  if (!cache.has(xml)) cache.set(xml, JSON.parse(execFileSync('python',['-I','-c',script], {input:xml,encoding:'utf8'})));
  return cache.get(xml);
} } };
const util = {TextEncoder:{create:()=>({encodeInto:s=>new TextEncoder().encode(s)})}, Base64Helper:class {encodeToStringSync(x){return Buffer.from(x).toString('base64')}}};
const crypt = {createRandom:()=>({generateRandomSync:n=>({data:randomBytes(n)})}), createMd:(name)=>{
  assert.equal(name,'SHA1'); const h=createHash(name);return {updateSync:b=>h.update(b.data),digestSync:()=>({data:h.digest()})};
}};
function wrap(ns, action, content) {
  return `<s:Envelope xmlns:s="${SOAP}" xmlns:t="${TT}" xmlns:m="${ns}" xmlns:vendor="urn:synthetic"><s:Body><m:${action}Response>${content}</m:${action}Response></s:Body></s:Envelope>`;
}
const data = {
  GetCapabilities: `<m:Capabilities><t:Media><t:XAddr>${media}</t:XAddr></t:Media></m:Capabilities>`,
  GetProfiles: '<m:Profiles token="a"><t:Name>First</t:Name></m:Profiles><m:Profiles token="b&amp;c"><t:Name>Second</t:Name><t:VideoEncoderConfiguration token="enc2"/></m:Profiles>',
  GetStreamUri: '<m:MediaUri><t:Uri>rtsp://127.0.0.1:8554/second?secret=opaque&amp;channel=2</t:Uri></m:MediaUri>',
  GetVideoEncoderConfiguration: '<m:Configuration token="enc2" vendor:preserve="yes"><t:Encoding>H264</t:Encoding><t:Resolution><t:Width>640</t:Width><t:Height>480</t:Height></t:Resolution><t:RateControl><t:FrameRateLimit>10</t:FrameRateLimit><t:EncodingInterval>1</t:EncodingInterval><t:BitrateLimit>1024</t:BitrateLimit></t:RateControl><t:H264><t:GovLength>30</t:GovLength><t:H264Profile>Main</t:H264Profile></t:H264><vendor:Other>A&amp;B</vendor:Other></m:Configuration>',
  GetVideoEncoderConfigurationOptions: '<m:Options><t:H264><t:ResolutionsAvailable><t:Width>1280</t:Width><t:Height>720</t:Height></t:ResolutionsAvailable><t:FrameRateRange><t:Min>5</t:Min><t:Max>25</t:Max></t:FrameRateRange><t:H264ProfilesSupported>High</t:H264ProfilesSupported></t:H264><t:Extension><t:H264><t:BitrateRange><t:Min>512</t:Min><t:Max>2048</t:Max></t:BitrateRange></t:H264></t:Extension></m:Options>',
  SetVideoEncoderConfiguration:''
};
function fixture(override=()=>undefined) {
  const calls=[], nonces=new Set(); let destroyed=0;
  const http={RequestMethod:{POST:'POST'},HttpProtocol:{HTTP1_1:0},HttpDataType:{STRING:0},createHttp:()=>({
    destroy:()=>destroyed++, async request(url,opts){
      assert.ok([dev,media].includes(url)); assert.equal(opts.maxRedirects,0); assert.equal(opts.usingProxy,false);assert.equal(opts.usingCache,false);
      assert.equal(opts.method,'POST'); assert.equal(opts.header.Connection,'close');assert.ok(!opts.extraData.includes(password));
      const body=opts.extraData;
      assert.match(body,/<wsse:Username>unit&lt;&amp;<\/wsse:Username>/);
      const nonce=body.match(/<wsse:Nonce[^>]*>([^<]+)/)[1];const created=body.match(/<wsu:Created>([^<]+)/)[1];
      assert.ok(!nonces.has(nonce));nonces.add(nonce);
      assert.equal(body.match(/<wsse:Password[^>]*>([^<]+)/)[1],createHash('sha1').update(Buffer.from(nonce,'base64')).update(created).update(password).digest('base64'));
      const action=opts.header['Content-Type'].match(/\/([^/]+)"$/)[1];
      calls.push({url,action,body});assert.equal(url,action==='GetCapabilities'?dev:media);
      const value=override(action);
      return value ?? {responseCode:200,result:wrap(action==='GetCapabilities'?DEVICE:MEDIA,action,data[action])};
    }})};
  const lib=load(http,util,crypt,convertxml);const session=new lib.OnvifManagedSession(dev,user,password);
  return {session,calls,lib,destroyed:()=>destroyed};
}
const draft={size:'1280x720',fps:'25',bitrate:'2048',level:'High'};
async function ready(f){const profiles=await f.session.login();assert.equal(f.calls.length,2);assert.deepEqual(profiles.map(x=>x.name),['First','Second']);assert.match(await f.session.choose(1),/second\?secret=opaque&channel=2/);return await f.session.readEncoder();}
test('explicit selected second profile, device options, whole config round-trip, single confirmed Set',async()=>{
  const f=fixture(),e=await ready(f); assert.equal(e.original.fps,'10');assert.equal(e.original.level,'Main');
  assert.match(f.calls[2].body,/<ProfileToken>b&amp;c<\/ProfileToken>/);assert.match(f.calls[4].body,/<ConfigurationToken>enc2/);
  for(const bad of [{fps:'25.1'},{fps:'26'},{fps:'NaN'},{bitrate:'2049'},{size:'800x600'},{level:'Baseline'}])assert.throws(()=>f.session.preview({...draft,...bad}));
  assert.throws(()=>f.session.preview(e.original));const cancelled=f.session.preview(draft); f.session.cancel(cancelled);await assert.rejects(f.session.confirm(cancelled));
  const ticket=f.session.preview(draft);assert.equal(f.calls.length,5); const once=f.session.confirm(ticket); await assert.rejects(f.session.confirm(ticket));await once;
  assert.deepEqual(f.calls.map(c=>c.action),Object.keys(data));const body=f.calls.at(-1).body;
  for(const v of ['<t:Width>1280','<t:Height>720','<t:FrameRateLimit>25','<t:BitrateLimit>2048','<t:H264Profile>High','<t:GovLength>30','vendor:preserve="yes"','xmlns:vendor="urn:synthetic"','<vendor:Other>A&amp;B','<ForcePersistence>true'])assert.ok(body.includes(v));
  assert.equal(f.destroyed(),6);f.session.close();await assert.rejects(f.session.choose(0));
});
test('ambiguous outcome consumes approval; read again, do not replay',async()=>{
  const f=fixture(action=>{if(action==='SetVideoEncoderConfiguration')throw Error('simulated lost acknowledgement')});await ready(f);
  const ticket=f.session.preview(draft);await assert.rejects(f.session.confirm(ticket));await assert.rejects(f.session.confirm(ticket));
  assert.throws(()=>f.session.preview(draft));await f.session.readEncoder();assert.equal(f.calls.filter(c=>c.action.startsWith('Set')).length,1);
});
test('deny redirect, auth challenge, alien XAddr, XML fault, DTD, insecure address and wrong scheme',async()=>{
  for(const code of [301,302,307,308,401,403]){
    const f=fixture(()=>({responseCode:code,header:{Location:'http://127.0.0.2/else'},result:''}));await assert.rejects(f.session.login());assert.equal(f.calls.length,1);
  }
  for(const other of ['http://127.0.0.2:12001/media','http://127.0.0.1:12002/media','https://127.0.0.1:12001/media']){
    const f=fixture(()=>({responseCode:200,result:wrap(DEVICE,'GetCapabilities',data.GetCapabilities.replace(media,other))}));await assert.rejects(f.session.login());assert.equal(f.calls.length,1);
  }
  const {lib}=fixture();for(const url of ['http://192.168.1.1/media','https://u:p@192.168.1.1/x','https://192.168.1.1:99999','https://example.invalid/x','https://8.8.8.8/x','http://0177.0.0.1','https://127.0.0.1\\@other','https://127.0.0.1\n']) assert.throws(()=>lib.endpoint(url));
  for(const raw of ['<!DOCTYPE fake><x/>',`<s:Envelope xmlns:s="${SOAP}"><s:Body><s:Fault/></s:Body></s:Envelope>`,wrap(MEDIA,'GetProfiles','').replace(MEDIA,'urn:wrong')])assert.throws(()=>lib.response(raw,MEDIA,'GetProfiles'));
});
test('read alternate prefixes/default namespace; round-trip vendor QNames, entities and CDATA without rebinding',()=>{
  const {lib}=fixture();
  const raw=`<?xml version="1.0" encoding="UTF-8"?><soap:Envelope xmlns:soap="${SOAP}"><soap:Body>`+
    `<GetProfilesResponse xmlns="${MEDIA}" xmlns:v_c="urn:vendor"><Profiles token="p" v_c:extra="&lt;x&gt;">`+
    `<Name xmlns="${TT}"><![CDATA[Name <v:real>]]></Name><Other xmlns="">original</Other>`+
    `</Profiles></GetProfilesResponse></soap:Body></soap:Envelope>`;
  const p=lib.response(raw,MEDIA,'GetProfiles').one(MEDIA,'Profiles');assert.equal(p.one(TT,'Name').value(),'Name <v:real>');
  assert.equal(p.one('','Other').value(),'original'); const xml=p.serialize(true);
  for(const part of [`xmlns="${MEDIA}"`,'xmlns:v_c="urn:vendor"','v_c:extra="&lt;x&gt;"','Name &lt;v:real&gt;','<Other xmlns="">'])assert.ok(xml.includes(part));
});
test('deny unadvertised bitrate edits, mismatched configuration token and foreign RTSP URI',async()=>{
  const f=fixture(action=>action==='GetVideoEncoderConfigurationOptions'?{responseCode:200,result:wrap(MEDIA,action,data[action].replace(/<t:Extension>.*<\/t:Extension>/,''))}:undefined);
  await ready(f); assert.throws(()=>f.session.preview(draft));assert.ok(f.session.preview({...draft,bitrate:'1024'}));
  const g=fixture(a=>a==='GetVideoEncoderConfiguration'?{responseCode:200,result:wrap(MEDIA,a,data[a].replace('token="enc2"','token="other"'))}:undefined);
  await g.session.login();await g.session.choose(1);await assert.rejects(g.session.readEncoder());
  const h=fixture(a=>a==='GetStreamUri'?{responseCode:200,result:wrap(MEDIA,a,data[a].replace('127.0.0.1','127.0.0.2'))}:undefined);
  await h.session.login();await assert.rejects(h.session.choose(1));assert.equal(h.calls.length,3);
});
