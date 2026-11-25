// Run with Node 22.18+ (built-in TypeScript type stripping), no Harmony runtime.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readProfiles, makeProfile, editAddress, editKey, endpointLabel, canPublish,
  saveProfile, selectProfile, deleteProfile } = require('./rtmp_host_modules.cjs');

const external = 'rtmp://manual/app/current';
const empty = { list: [], current: external };
const full = 'rtmp://camera:1935/live/private%2Fstream?auth=example';

test('legacy URL is not split on load or edit; bad lists fail closed', () => {
  const [legacy] = readProfiles(JSON.stringify([{ url: full }]));
  assert.deepEqual(legacy, { url: full });
  assert.equal(editAddress(legacy), full);
  assert.equal(editKey(legacy), '');
  assert.deepEqual(makeProfile('旧配置', editAddress(legacy), ''), { name: '旧配置', url: full });
  for (const raw of ['invalid', '{}', '[null]', '[{"url":123}]', '[{"url":"x","keyStart":9}]']) {
    assert.throws(() => readProfiles(raw));
  }
});

test('join at a slash without rewriting opaque keys or storing a second secret field', () => {
  for (const base of ['rtmp://camera:1935/live/', 'rtmp://camera:1935/live']) {
    for (const key of ['private%2Fstream?auth=example', '/private%2Fstream?auth=example']) {
      const p = makeProfile(' 工作室 ', base, key);
      assert.equal(p.url, full);
      const raw = JSON.stringify([p]);
      assert.equal(raw.split('private%2Fstream').length, 2);
      const [loaded] = readProfiles(raw);
      assert.equal(loaded.name, '工作室');
      assert.equal(editAddress(loaded), 'rtmp://camera:1935/live');
      assert.equal(editKey(loaded), 'private%2Fstream?auth=example');
    }
  }
});

test('only supported RTMP shapes; never display userinfo, app, stream or query', () => {
  assert.equal(endpointLabel(full), 'rtmp://camera:1935/…');
  for (const u of ['https://camera/a/s', 'rtmp://user:password@camera/a/s', 'rtmp://camera/a',
    'rtmp://camera//s', 'rtmp://camera/a/', 'rtmp://camera:0/a/s', 'rtmp://camera:65536/a/s',
    'rtmp://[:::1]/a/s', 'rtmp://::1/a/s', 'rtmp://[1:2:3:4:5:6:7:8:9]/a/s', 'rtmp://camera/a/s#fragment', 'rtmp://camera/a/s\n']) {
    assert.equal(canPublish(u), false);
    assert.equal(endpointLabel(u), '（地址已隐藏）');
    // Legacy records can still be opened for correction/removal, not selected.
    const state = { list: readProfiles(JSON.stringify([{ url: u }])), current: external };
    assert.throws(() => selectProfile(state, 0));
    assert.deepEqual(deleteProfile(state, 0), empty);
  }
  assert.throws(() => makeProfile('test', 'rtmp://camera/live?token=example', 'stream'));
  assert.throws(() => makeProfile('', full, ''));
});

test('RTMPS uses exactly the TLS identity authority, preserves scheme/key and masks credentials', () => {
  const { rtmpTlsEndpoint } = require('../entry/src/main/ets/util/RtmpTlsIdentity.ts');
  for (const [authority, port] of [['Example.TEST', 443], ['127.0.0.1:8443', 8443], ['[::1]', 443],
    ['[::ffff:127.0.0.1]:00443', 443]]) {
    const base = `rtmps://${authority}/live`, key = 'private%2Fstream?token=secret&x=%23';
    const p = makeProfile('TLS', base, key);
    assert.equal(p.url, base + '/' + key);
    const endpoint = rtmpTlsEndpoint(p.url);
    assert.equal(endpoint.port, port);
    assert.equal(endpoint.authority, authority);
    assert.equal(selectProfile({ list: [p], current: '' }, 0).current, p.url);
    assert.equal(endpointLabel(p.url), `rtmps://${authority}/…`);
    assert.equal(editAddress(p), base);
    assert.equal(editKey(p), key);
  }
  for (const authority of ['user:pass@cam', '@cam', 'cam%40evil', 'cam:443@evil', 'cam.', 'bad..host',
    '-bad', '127.1', '127.00.0.1', '256.0.0.1', '0x7f000001', 'cam:0', 'cam:65536', 'cam:000443',
    '::1', '[:::1]', '[::1]:x', '[::1]evil', '[fe80::1%25eth0]']) {
    const u = `rtmps://${authority}/private/key?auth=secret`;
    assert.throws(() => rtmpTlsEndpoint(u));
    assert.equal(canPublish(u), false);
    assert.equal(endpointLabel(u), '（地址已隐藏）');
    assert.throws(() => makeProfile('TLS', u, ''), /format/);
    assert.throws(() => selectProfile({ list: [{ url: u }], current: '' }, 0));
  }
});

test('IPv6 authority stays bracketed and the opaque key is hidden', () => {
  for (const host of ['[::1]', '[2001:db8::7]:1935', '[::ffff:127.0.0.1]', '[1:2:3:4:5:6:7:8]']) {
    const p = makeProfile('IPv6', `rtmp://${host}/live`, 'key%2Fpart?token=secret');
    assert.equal(canPublish(p.url), true);
    assert.equal(endpointLabel(p.url), `rtmp://${host}/…`);
    assert.equal(editKey(p), 'key%2Fpart?token=secret');
  }
});

test('selection, editing, duplicate rejection and deletion use the same effective URL as Index', () => {
  const p = makeProfile('A', 'rtmp://camera:1935/live', 'private%2Fstream?auth=example');
  const q = makeProfile('B', 'rtmp://camera/live/second', '');
  const added = saveProfile(saveProfile(empty, -1, p), -1, q);
  assert.equal(added.current, external);
  assert.equal(deleteProfile(added, 0).current, external);
  assert.throws(() => saveProfile(added, -1, makeProfile('duplicate', full, '')));
  const picked = selectProfile(added, 1);
  assert.equal(picked.current, full);
  assert.equal(picked.list, added.list);
  assert.throws(() => saveProfile(picked, 1, q));
  assert.equal(deleteProfile(picked, 0).current, full);
  const changed = makeProfile('renamed', 'rtmp://camera/live', 'changed');
  assert.equal(saveProfile(picked, 0, changed).current, full);
  const updated = saveProfile(picked, 1, changed);
  assert.equal(updated.current, changed.url);
  assert.equal(picked.list[1], p);
  assert.deepEqual(deleteProfile(updated, 1), { list: [q], current: '' });
});
