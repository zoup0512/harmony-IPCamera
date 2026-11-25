// Node 22.18+: node --test tests/device_store_test.cjs. No Harmony SDK or device required.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const { randomUUID } = require('node:crypto');

// Exercise the actual .ets store. Only replace its SDK imports; unknown imports fail to load.
const file = join(__dirname, '../entry/src/main/ets/util/DeviceStore.ets');
const source = readFileSync(file, 'utf8')
  .replace("import { asset } from '@kit.AssetStoreKit';", '')
  .replace("import { url, util } from '@kit.ArkTS';", '')
  .replace("import { BusinessError } from '@kit.BasicServicesKit';", '')
  .replace("import { liveEndpoint } from './LiveEndpoint';", '')
  .replace(/^export /gm, '');
const endpoint = readFileSync(join(__dirname, '../entry/src/main/ets/util/LiveEndpoint.ets'), 'utf8').replace(/^import .*;$/gm, '').replace(/^export /gm, '');
const loadStore = new Function('asset', 'url', 'util', stripTypeScriptTypes(endpoint) + stripTypeScriptTypes(source) +
  '\nreturn { readDeviceQr, publicEndpoint, fitsDeviceStore, saveDevice, listDevices, readDevice, deleteDevice };');
const util = {
  generateRandomUUID: randomUUID,
  TextEncoder: { create: () => ({ encodeInto: s => new TextEncoder().encode(s) }) },
  TextDecoder: { create: enc => ({ decodeToString: b => new TextDecoder(enc).decode(b) }) },
};

// ASSET returns attributes (never SECRET) for batch queries, and throws NOT_FOUND for empty results.
// This fake checks the SDK contract used here, not the OS's actual encryption or lock state.
function fixture() {
  const records = new Map();
  const tagNames = ['ALIAS', 'SECRET', 'DATA_LABEL_CRITICAL_1', 'RETURN_LIMIT', 'RETURN_OFFSET',
    'RETURN_TYPE', 'ACCESSIBILITY', 'SYNC_TYPE'];
  const Tag = Object.fromEntries(tagNames.map(name => [name, name]));
  const notFound = () => Object.assign(new Error('asset not found'), { code: 24000002 });
  const decode = (row, tag) => Buffer.from(row.get(tag)).toString('utf8');
  function matches(query) {
    return [...records.values()].filter(row =>
      decode(row, Tag.DATA_LABEL_CRITICAL_1) === decode(query, Tag.DATA_LABEL_CRITICAL_1) &&
      (!query.has(Tag.ALIAS) || decode(row, Tag.ALIAS) === decode(query, Tag.ALIAS)));
  }
  function one(query) {
    assert.ok(query.has(Tag.ALIAS), 'single-record operations require an alias');
    const [row] = matches(query);
    if (!row) throw notFound();
    return row;
  }
  const sdk = {
    Tag, Accessibility: { DEVICE_UNLOCKED: 2 }, SyncType: { NEVER: 0 },
    ReturnType: { ALL: 0, ATTRIBUTES: 1 }, ErrorCode: { NOT_FOUND: 24000002 },
    async add(record) {
      const alias = decode(record, Tag.ALIAS);
      assert.ok(!records.has(alias));
      assert.equal(record.get(Tag.ACCESSIBILITY), this.Accessibility.DEVICE_UNLOCKED);
      assert.equal(record.get(Tag.SYNC_TYPE), this.SyncType.NEVER);
      assert.ok(record.get(Tag.SECRET).length <= 1024);
      records.set(alias, new Map(record));
    },
    async update(query, change) {
      one(query).set(Tag.SECRET, change.get(Tag.SECRET));
    },
    async remove(query) {
      // Device NAPI rejects group labels for remove even though query accepts them.
      assert.deepEqual([...query.keys()], [Tag.ALIAS]);
      if (!records.delete(decode(query, Tag.ALIAS))) throw notFound();
    },
    async query(query) {
      if (query.get(Tag.RETURN_TYPE) === this.ReturnType.ALL) return [one(query)];
      assert.equal(query.get(Tag.RETURN_TYPE), this.ReturnType.ATTRIBUTES);
      const offset = query.get(Tag.RETURN_OFFSET) ?? 0;
      const limit = query.get(Tag.RETURN_LIMIT) ?? 1;
      const rows = matches(query).slice(offset, offset + limit);
      if (!rows.length) throw notFound();
      return rows.map(row => new Map([...row].filter(([tag]) => tag !== Tag.SECRET)));
    },
  };
  return { sdk, records, store: loadStore(sdk, { URL: { parseURL: value => new URL(value) } }, util) };
}

const full = 'https://user:p%40ss@[2001:db8::7]:8081/private%2fpath/video?pw=A+B%2f&token=opaque#fragment';

test('plain and Android JSON QR: preserve query auth and encode separate userinfo only once', () => {
  const { store } = fixture();
  assert.deepEqual(store.readDeviceQr(` ${full} `), { name: '', url: full });
  const qr = { title: ' 门口 ', url: 'rtsp://camera:8554/live?key=%2f+@a', user: 'a@b', password: 'x:/#?' };
  assert.deepEqual(store.readDeviceQr(JSON.stringify(qr)), {
    name: '门口', url: 'rtsp://a%40b:x%3A%2F%23%3F@camera:8554/live?key=%2f+@a' });
  assert.equal(store.readDeviceQr(JSON.stringify({ ...qr, url: full })).url, full);
  assert.equal(store.readDeviceQr(JSON.stringify({ ...qr, user: '', password: '' })).url, qr.url);
  for (const invalid of ['{}', 'null', 'not a camera', 'file:///sdcard/camera',
    JSON.stringify({ ...qr, password: 42 }), JSON.stringify({ ...qr, url: 'javascript:alert(1)' })]) {
    assert.throws(() => store.readDeviceQr(invalid));
  }
});

test('public label contains only the scheme, host and port; malformed URLs fail closed', () => {
  const { store } = fixture();
  assert.equal(store.publicEndpoint(full), 'https://[2001:db8::7]:8081');
  assert.equal(store.publicEndpoint('rtsps://a:p@host:322/stream?token=demo'), 'rtsps://host:322');
  for (const invalid of ['file:///tmp', 'https:////evil', 'http://', 'rtsp://:bad',
    'http://host\\@other/stream', 'rtsp://host/live\n', 'rtsp://host/has space']) {
    assert.equal(store.publicEndpoint(invalid), '');
  }
});

test('CRUD survives reopening the store; secret URL never enters labels or list items', async () => {
  const { store, records, sdk } = fixture();
  assert.deepEqual(await store.listDevices(), []);
  await store.saveDevice('', '', full);
  const reopened = loadStore(sdk, { URL: { parseURL: value => new URL(value) } }, util);
  const [item] = await reopened.listDevices();
  assert.deepEqual(item, { id: item.id, name: 'https://[2001:db8::7]:8081', endpoint: 'https://[2001:db8::7]:8081' });
  const record = records.get(item.id);
  for (const [tag, value] of record) {
    if (tag !== sdk.Tag.SECRET && value instanceof Uint8Array) {
      assert.doesNotMatch(Buffer.from(value).toString(), /user|p%40ss|private|pw=|token=|fragment/);
    }
  }
  assert.deepEqual(await reopened.readDevice(item.id), { name: '', url: full });
  await reopened.saveDevice(item.id, ' Entry ', 'rtsp://new/next?pw=token');
  assert.deepEqual(await store.readDevice(item.id), { name: 'Entry', url: 'rtsp://new/next?pw=token' });
  assert.deepEqual(await store.listDevices(), [{ id: item.id, name: 'Entry', endpoint: 'rtsp://new' }]);
  await assert.rejects(store.deleteDevice(''));
  await store.deleteDevice(item.id);
  assert.deepEqual(await store.listDevices(), []);
  await assert.rejects(store.readDevice(item.id), { code: sdk.ErrorCode.NOT_FOUND });
});

test('paging includes boundary records and excludes unrelated assets', async () => {
  const { store, records, sdk } = fixture();
  const foreign = new Map([[sdk.Tag.ALIAS, Buffer.from('foreign')],
    [sdk.Tag.DATA_LABEL_CRITICAL_1, Buffer.from('unrelated')]]);
  records.set('foreign', foreign);
  for (let i = 0; i < 33; ++i) await store.saveDevice('', `camera ${i}`, `http://host/live?pw=${i}`);
  const devices = await store.listDevices();
  assert.equal(devices.length, 33);
  assert.equal(new Set(devices.map(item => item.id)).size, 33);
  assert.equal(devices[0].name, 'camera 32');
  assert.equal(devices.at(-1).name, 'camera 0');
  await store.deleteDevice(devices[0].id);
  assert.equal((await store.listDevices()).length, 32);
  await assert.rejects(store.deleteDevice('foreign'), { code: sdk.ErrorCode.NOT_FOUND });
  assert.ok(records.has('foreign'));
});

test('byte limit, invalid address, and storage failure never downgrade to plaintext or an empty list', async () => {
  const { store, records, sdk } = fixture();
  assert.equal(store.fitsDeviceStore({ name: '门'.repeat(350), url: full }), false);
  await assert.rejects(store.saveDevice('', '门'.repeat(350), full));
  await assert.rejects(store.saveDevice('', '', 'file:///tmp'));
  assert.equal(records.size, 0);
  const locked = Object.assign(new Error('device locked'), { code: 24000014 });
  sdk.add = sdk.query = async () => { throw locked; };
  await assert.rejects(store.saveDevice('', '', full), locked);
  await assert.rejects(store.listDevices(), locked);
  assert.equal(records.size, 0);
});

test('RTMP stores the exact stream key in the asset only, rejects unsupported variants and QR credentials',async()=>{
  const {store}=fixture(), url='rtmp://cam:1935/live/private%2Fkey?sig=private';
  await store.saveDevice('','',url);const [item]=await store.listDevices();
  assert.equal(item.name,'rtmp://cam:1935');assert.equal(item.endpoint,item.name);assert.doesNotMatch(JSON.stringify(item),/private/);
  assert.equal((await store.readDevice(item.id)).url,url);
  for(const bad of ['rtmps://cam/app/key','rtmp://u:p@cam/app/key','rtmp://@cam/app/key','rtmp://cam/live','rtmp://cam/live/?x','rtmp://cam:0/live/s']) {
    assert.equal(store.publicEndpoint(bad),'');await assert.rejects(store.saveDevice('','',bad));
  }
  assert.throws(()=>store.readDeviceQr(JSON.stringify({title:'RTMP',url,user:'u',password:'p'})));
});
