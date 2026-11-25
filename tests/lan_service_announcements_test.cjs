// Node 22.18+. Execute the ArkTS module with only a DNS-SD registration double (no network).
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { stripTypeScriptTypes } = require('node:module');
const file = path.join(__dirname, '../entry/src/main/ets/model/LanServiceAnnouncements.ets');
const moduleCode = stripTypeScriptTypes(fs.readFileSync(file, 'utf8')
  .replace(/^import .* from '@kit\.(AbilityKit|NetworkKit)';$/gm, '').replaceAll('export ', '') + '\n;lanServiceAnnouncements');
const next = () => new Promise(resolve => setImmediate(resolve));
function rig() {
  const strings = require('../entry/src/main/resources/zh_CN/element/addon_lan.json').string;
  const ctx = { resourceManager: { getStringSync(id, port) {
    return strings.find(s => 'app.string.' + s.name === id).value.replace('%d', port);
  } } }, live = new Set(), calls = [];
  const sdk = {
    async addLocalService(c, info) {
      assert.equal(c, ctx);
      assert.deepEqual(Object.keys(info).sort(), ['port', 'serviceName', 'serviceType']);
      assert.equal(info.serviceName, 'IPCamera');
      assert.ok(['_rtsp._tcp', '_http._tcp'].includes(info.serviceType));
      assert.ok(!Array.from(live).some(s => s.serviceType === info.serviceType));
      const record = { ...info, serviceName: 'IPCamera (2)' };
      calls.push(['add', info.serviceType, info.port]); live.add(record); return record;
    },
    async removeLocalService(c, record) {
      assert.equal(c, ctx); assert.ok(live.delete(record), 'must remove the precise returned record');
      calls.push(['remove', record.serviceType, record.port]); return record;
    }
  };
  const get = vm.runInNewContext(moduleCode, { mdns: sdk, $r: id => ({ id }) });
  const services = get({ getApplicationContext: () => ctx });
  assert.equal(get({ getApplicationContext: () => ctx }), services);
  return { sdk, live, calls, services, http: services.http, rtsp: services.rtsp };
}

test('requires both opt-in and a real listener; only advertises the selected bound ports', async () => {
  const { services, http, rtsp, calls, live } = rig();
  await http.setBoundPort(31415);
  await rtsp.setEnabled(true);
  for (const port of [0, -1, 65536, NaN, 1.5, Infinity, '8554']) await rtsp.setBoundPort(port);
  assert.deepEqual(calls, []); assert.equal(http.enabled, false);
  await rtsp.setBoundPort(65535); await http.setEnabled(true); await http.setBoundPort(31415);
  await rtsp.setEnabled(false); await services.stopAll(); await services.stopAll();
  assert.deepEqual(calls, [['add', '_rtsp._tcp', 65535], ['add', '_http._tcp', 31415],
    ['remove', '_rtsp._tcp', 65535], ['remove', '_http._tcp', 31415]]);
  assert.equal(live.size, 0); assert.equal(http.failed, false); assert.match(http.status, /等待/);
});

test('stop during SDK registration removes the late result; the other listener is independent', async () => {
  const { sdk, services, http, rtsp, live, calls } = rig();
  const add = sdk.addLocalService; let release;
  const wait = new Promise(r => { release = r; });
  sdk.addLocalService = async (c, info) => {
    if (info.serviceType === '_http._tcp') await wait;
    return add(c, info);
  };
  await http.setEnabled(true); await rtsp.setEnabled(true);
  const starting = http.setBoundPort(1234); await next();
  await rtsp.setBoundPort(4321); const stopped = services.stopAll(); await next();
  assert.deepEqual(calls, [['add', '_rtsp._tcp', 4321], ['remove', '_rtsp._tcp', 4321]]);
  release(); await Promise.all([starting, stopped]);
  assert.deepEqual(calls.slice(2), [['add', '_http._tcp', 1234], ['remove', '_http._tcp', 1234]]);
  assert.equal(live.size, 0);
});

test('same-port restart withdraws old generation; disable during removal cancels replacement', async () => {
  const { sdk, http, live, calls } = rig();
  await http.setEnabled(true); await http.setBoundPort(17231);
  await Promise.all([http.setBoundPort(0), http.setBoundPort(17231)]);
  assert.deepEqual(calls.map(c => c[0]), ['add', 'remove', 'add']);
  const remove = sdk.removeLocalService; let release;
  const wait = new Promise(r => { release = r; });
  sdk.removeLocalService = async (c, rec) => { await wait; return remove(c, rec); };
  const replacing = http.setBoundPort(17232); await next();
  const disabling = http.setEnabled(false); release(); await Promise.all([replacing, disabling]);
  assert.equal(live.size, 0); assert.equal(calls.length, 4);
});

test('failure is visible and retryable without duplicates or raw SDK diagnostics', async () => {
  const { sdk, http, services, live, calls } = rig();
  const changes = []; const observe = () => changes.push(http.status);
  http.observe(observe); await http.setEnabled(true);
  const add = sdk.addLocalService, remove = sdk.removeLocalService;
  sdk.addLocalService = () => { throw Error('secret'); };
  await http.setBoundPort(40123); assert.ok(http.failed); assert.doesNotMatch(http.status, /secret/);
  sdk.addLocalService = add; await http.retry(); assert.equal(http.failed, false);
  sdk.removeLocalService = async () => { throw Error('secret'); };
  await http.setBoundPort(40124); assert.ok(http.failed); assert.match(http.status, /40123/);
  assert.equal(calls.length, 1); assert.equal(live.size, 1);
  sdk.removeLocalService = remove; await http.retry(); assert.match(http.status, /40124/);
  assert.deepEqual(calls, [['add', '_http._tcp', 40123], ['remove', '_http._tcp', 40123], ['add', '_http._tcp', 40124]]);
  http.unobserve(observe); const count = changes.length;
  http.observe(() => { throw Error('detached view'); });
  sdk.removeLocalService = async () => { throw Error('cleanup failure'); };
  assert.equal(await services.stopAll(), false); assert.equal(live.size, 1);
  sdk.removeLocalService = remove;
  assert.equal(await services.stopAll(), true); assert.equal(live.size, 0); assert.equal(changes.length, count);
});
