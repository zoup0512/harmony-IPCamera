const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const profiles = require('./rtmp_host_modules.cjs');
const main = join(__dirname, '../entry/src/main');
const page = name => readFileSync(join(main, 'ets/pages', name + '.ets'), 'utf8');
function catalog(locale) {
  const { string } = JSON.parse(readFileSync(join(main, 'resources', locale, 'element/playback_publish_scan.json'), 'utf8'));
  const values = new Map(string.map(({ name, value }) => [name, value]));
  assert.equal(values.size, string.length, 'duplicate resource key');
  return values;
}
const en = catalog('base'), zh = catalog('zh_CN');

test('every scoped resource is present in both locales with matching format arguments', () => {
  const referenced = new Set();
  for (const [name, prefix] of [['Playback', 'arc_'], ['RtmpAddrs', 'pub_'], ['OnvifScan', 'scan_']]) {
    for (const [, key] of page(name).matchAll(/\$r\('app\.string\.(\w+)'/g)) {
      if (key.startsWith(prefix)) referenced.add(key);
    }
  }
  assert.deepEqual(new Set(en.keys()), referenced);
  assert.deepEqual(new Set(zh.keys()), referenced);
  for (const [key, english] of en) {
    assert.ok(english.trim() && zh.get(key).trim(), key);
    assert.deepEqual(english.match(/%[ds]/g), zh.get(key).match(/%[ds]/g), key);
  }
});

test('RTMP validation and masked endpoints use the selected locale without exposing private URLs', () => {
  const logic = page('RtmpAddrs').split('  build() {')[0].replace(/^import [^;]+;/gm, '')
    .replace(/^@(Entry|Component)\s*$/gm, '').replace(/@State /g, '').replace('struct RtmpAddrs', 'class RtmpAddrs') + '}';
  for (const strings of [en, zh]) {
    const notices = [];
    const $r = (id, arg) => {
      const value = strings.get(id.replace(/^app\.string\./, ''));
      assert.ok(value, id);
      return arg === undefined ? value : value.replace('%s', () => arg);
    };
    const mocks = { ...profiles, Scroller: class {}, $r, promptAction: { showToast: ({ message }) => notices.push(message) } };
    const Page = new Function(...Object.keys(mocks), stripTypeScriptTypes(logic) + '\nreturn RtmpAddrs;')(...Object.values(mocks));
    const p = new Page();
    const valid = 'rtmp://camera/app/private?token=secret', invalid = 'rtmp://user:secret@camera/app/private';
    p.renderConfig({ list: [{ url: invalid }, { url: valid }], current: valid });
    assert.equal(p.currentLabel, $r('app.string.pub_current', 'rtmp://camera/…'));
    assert.equal(p.rows[0].name, strings.get('pub_unnamed'));
    assert.equal(p.rows[0].endpoint, strings.get('pub_hidden'));
    assert.equal(p.rows[1].endpoint, 'rtmp://camera/…');
    for (const [name, address, key, expected] of [
      ['', valid, '', 'name_required'], ['A', 'rtmp://camera/app?token=secret', 's', 'full_url'],
      ['A', 'rtmp://camera/app//', 's', 'slash'], ['A', 'rtmp://camera/app', '/', 'key_path'],
      ['A', invalid, '', 'format'], ['A', valid, '', 'duplicate']
    ]) {
      p.name = name; p.address = address; p.keyInput = key; p.save();
      assert.equal(notices.pop(), strings.get('pub_' + expected));
    }
    const secure = 'rtmps://[::1]:8443/app/private?token=secret';
    p.renderConfig({ list: [{ url: secure }, { url: 'rtmps://user:password@cam/app/private' }], current: secure });
    assert.equal(p.currentLabel, $r('app.string.pub_current', 'rtmps://[::1]:8443/…'));
    assert.equal(p.rows[0].endpoint, 'rtmps://[::1]:8443/…');
    assert.equal(p.rows[1].endpoint, strings.get('pub_hidden'));
    p.renderConfig({ list: [], current: invalid });
    assert.equal(p.currentLabel, strings.get('pub_current_hidden'));
    p.renderConfig({ list: [], current: '' });
    assert.equal(p.currentLabel, strings.get('pub_current_none'));
  }
});
