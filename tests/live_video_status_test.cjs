// Poll the actual page with native status doubles; no device or network required.
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const { test } = require('node:test');

const source = readFileSync(join(__dirname, '../entry/src/main/ets/pages/Player.ets'), 'utf8');
const logic = source.slice(0, source.indexOf('  build() {')).replace(/^import .*;$/gm, '')
  .replace(/^@(Entry|Component)\s*$/gm, '').replace('struct Player', 'class Player').replace(/@State /g, '') + '}';
const { ViewerReconnect, rtspViewerFailure } = require('../entry/src/main/ets/util/ViewerReconnect.ts');
const createPlayer = new Function('streaming', 'XComponentController', 'ViewerReconnect', 'rtspViewerFailure', stripTypeScriptTypes(logic) + '\nreturn new Player();');

function setup(mime) {
  const session = { status: 0, mime, audio: 0 };
  const page = createPlayer({ liveAvcStatus: h => h.status, liveVideoCodec: h => h.mime,
    liveAvcAudioStatus: h => h.audio, liveAvcStop: () => {} }, class {}, ViewerReconnect, rtspViewerFailure);
  page.active = true;
  page.nativeHandle = session;
  page.lastFrame = Date.now();
  page.status = '正在连接…';
  return { page, session };
}

for (const [mime, label] of [['video/hevc', 'H.265'], ['video/avc', 'H.264']]) {
  test(`${label} is shown only after the native decoder reports output`, () => {
    const { page, session } = setup(mime);
    page.poll(page.epoch);
    assert.equal(page.status, '正在连接…');
    session.status = 1;
    page.poll(page.epoch);
    assert.equal(page.status, `正在播放 · ${label} · 已静音`);
    const last = page.epoch;
    page.stop();
    page.poll(last);
    assert.equal(page.status, '已停止');
  });
}

test('HEVC decoder failure is not mislabeled as AVC or as successful playback', () => {
  const { page, session } = setup('video/hevc');
  session.status = -6;
  page.poll(page.epoch);
  assert.match(page.status, /无法解码.*H\.265/);
  assert.equal(page.active, false);
  assert.equal(page.failed, true);
});
