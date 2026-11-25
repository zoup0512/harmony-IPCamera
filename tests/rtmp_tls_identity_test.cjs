const test = require('node:test');
const assert = require('node:assert/strict');
const { rtmpTlsEndpoint: parse } = require('../entry/src/main/ets/util/RtmpTlsIdentity.ts');
const url = authority => `rtmps://${authority}/live/key?private=example`;
test('bind exact authority and derive a typed DER DNS or binary IP SAN', () => {
  const dns = parse(url('LOCALHOST:443'));
  assert.equal(dns.authority, 'LOCALHOST:443'); assert.equal(dns.host, 'localhost'); assert.equal(dns.port, 443);
  assert.deepEqual([...dns.generalName], [0x82, 9, ...Buffer.from('localhost')]);
  assert.deepEqual([...parse(url('127.0.0.1')).generalName], [0x87,4,127,0,0,1]);
  assert.deepEqual([...parse(url('[::ffff:127.0.0.1]')).generalName],
    [0x87,16,...Array(10).fill(0),255,255,127,0,0,1]);
  assert.deepEqual([...parse(url('[0:0:0:0:0:0:0:1]')).generalName], [...parse(url('[::1]')).generalName]);
  const long = 'a'.repeat(63)+'.'+'b'.repeat(63)+'.c';
  assert.deepEqual([...parse(url(long)).generalName.slice(0,3)], [0x82,0x81,long.length]);
});
test('reject ambiguous numeric hosts, wildcard input, zone IDs, URL delimiters and invalid ports', () => {
  for (const host of ['127.1','0177.0.0.1','127.0.0.256','0x7f.0.0.1','0x7f000001','*','cam.','u@cam',
    '[::1%25lo]','[1::2::3]','[12345::1]','[1:2:3:4:5:6:7]','[0:0:0:0:0:0:0:0::]', '[::1]:65536','host:0',
    '.cam','cam..example','cam#x','cam\\x','host\u0000.test','bücher.test']) assert.throws(()=>parse(url(host)),host);
  assert.throws(()=>parse('rtmp://localhost/live/key')); assert.throws(()=>parse('rtmps://localhost/app/'));
});
