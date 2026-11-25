// Node >= 22.13. Executes the actual ArkTS helper functions; no server or npm install.
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const util = {
  Base64Helper: class { encodeToStringSync(bytes) { return Buffer.from(bytes).toString('base64'); } },
  TextEncoder: { create: () => ({ encodeInto: s => new TextEncoder().encode(s) }) }
};
function load(file, exports) {
  const source = readFileSync(join(__dirname, '../entry/src/main/ets/util', file), 'utf8').replace(/^import .*?;$/gm, '').replace(/^export /gm, '');
  return new Function('url', 'util', stripTypeScriptTypes(source) + '\nreturn {' + exports.join(',') + '}')({ URL: { parseURL: s => new URL(s) } }, util);
}
const { liveEndpoint } = load('LiveEndpoint.ets', ['liveEndpoint']);
const { mjpegBoundary, MjpegFrames } = load('MjpegFrames.ets', ['mjpegBoundary', 'MjpegFrames']);
let t = liveEndpoint('HTTPS://viewer:p%40ss%3A1@cam:443/live%2Fmedia?pw=a%2Bb');
assert.equal(t.address, 'https://cam:443/live%2Fmedia?pw=a%2Bb');
assert.equal(t.origin, 'https://cam:443');
assert.equal(Buffer.from(t.token, 'base64').toString(), 'viewer:p@ss:1');
assert.equal(liveEndpoint('rtsp://[::1]:8554?view=main').address, 'rtsp://[::1]:8554/?view=main');
assert.equal(Buffer.from(liveEndpoint('http://old:p@cam/video', 'new', 'newpw').token, 'base64').toString(), 'new:newpw');
for (const raw of ['file:///private', 'http://a\\@b/secret', 'rtsp://a\r\nX:1', 'http://%65vil/video', 'http://u@x@y', 'http://a/#fragment', 'http://u:%0apass@a']) assert.throws(() => liveEndpoint(raw));
assert.equal(mjpegBoundary('Multipart/X-Mixed-Replace; boundary="cam-01"'), 'cam-01');
for (const ct of ['text/html; boundary=x', 'image/jpeg', 'multipart/x-mixed-replace;boundary="a\rb"']) assert.equal(mjpegBoundary(ct),'');
const jpg = Buffer.from([255,216, 0,2,255,217, 0,255,217]); // embedded EOI must not truncate JPEG
const part = (known) => Buffer.concat([Buffer.from('--cam\r\nContent-Type: image/jpeg\r\n' + (known ? `Content-Length: ${jpg.length}\r\n` : '') + '\r\n'), jpg, Buffer.from('\r\n')]);
const input = Buffer.concat([part(false), part(true), part(false), Buffer.from('--cam--\r\n')]);
for (const step of [1,2,3,7,37,input.length]) {
  const parser = new MjpegFrames('cam'), out = [];
  for(let i = 0; i < input.length; i += step) parser.feed(input.subarray(i,i+step), b=>out.push(Buffer.from(b)));
  assert.deepEqual(out,[jpg,jpg,jpg]);
}
for(const s of ['Content-Type: text/html\r\n', 'Content-Type: image/jpeg\r\nContent-Length: 2147483647\r\n', 'Content-Type: image/jpeg\r\nContent-Length: -1\r\n']) {
  assert.throws(()=>new MjpegFrames('cam').feed(Buffer.from('--cam\r\n'+s+'\r\n'),()=>{}));
}
assert.throws(()=>new MjpegFrames('cam').feed(Buffer.alloc(9000,65),()=>{}));
assert.throws(()=>new MjpegFrames('cam').feed(Buffer.concat([Buffer.from('--cam\r\nContent-Type: image/jpeg\r\n\r\n'), Buffer.alloc(2200000,65)]),()=>{}));
console.log('PASS: endpoint credentials/redaction, MIME and JPEG boundaries, fragmented chunks, embedded EOI, size limits');
