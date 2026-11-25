// Exercise the actual page handlers with a dialog double; no real devices are removed.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');

const pageFile = join(__dirname, '../entry/src/main/ets/pages/Devices.ets');
const source = readFileSync(pageFile, 'utf8').split('  @Builder')[0]
  .replace(/^import [^;]+;/gm, '').replace(/^@(Entry|Component)\s*$/gm, '')
  .replace('struct Devices', 'class Devices').replace(/@State /g, '') + '}';
const makePage = new Function('listDevices', 'deleteDevice', '$r',
  stripTypeScriptTypes(source) + '\nreturn new Devices();');

function fixture() {
  // Two entries can legitimately have the same display name and public endpoint.
  const rows = ['first', 'second'].map(id => ({ id, name: 'Camera', endpoint: 'rtsp://camera.test' }));
  const calls = [], notices = [];
  const f = { dialog: undefined, fail: false };
  f.page = makePage(async () => [...rows], async id => {
    calls.push(id);
    if (f.fail) throw new Error('store unavailable');
    const index = rows.findIndex(row => row.id === id);
    assert.notEqual(index, -1);
    rows.splice(index, 1);
  }, key => key);
  f.page.getUIContext = () => ({ showAlertDialog: d => { f.dialog = d; } });
  f.page.notice = message => notices.push(message);
  return Object.assign(f, { calls, notices });
}

async function click(f, key) {
  const button = [f.dialog.primaryButton, f.dialog.secondaryButton].find(b => b.value === key);
  assert.ok(button, `missing dialog button: ${key}`);
  await button.action();
}

test('cancel is non-destructive; confirming deletes only the selected ID and refreshes the list', async () => {
  const f = fixture(), p = f.page;
  await p.refresh();
  p.confirmDelete(p.items[1]);
  assert.equal(f.dialog.message, 'Camera');
  await click(f, 'app.string.dm_cancel');
  assert.deepEqual(f.calls, []);
  assert.equal(p.items.length, 2);
  p.confirmDelete(p.items[1]);
  await click(f, 'app.string.dm_delete');
  assert.deepEqual(f.calls, ['second']);
  assert.deepEqual(p.items.map(row => row.id), ['first']);
  await p.refresh();
  assert.deepEqual(p.items.map(row => row.id), ['first']);
});

test('failed removal keeps the entry, reports failure and does not leave the page busy', async () => {
  const f = fixture(), p = f.page;
  await p.refresh();
  f.fail = true;
  p.confirmDelete(p.items[0]);
  await click(f, 'app.string.dm_delete');
  assert.deepEqual(f.notices, ['app.string.dm_store_error']);
  assert.equal(p.busy, false);
  assert.deepEqual(p.items.map(row => row.id), ['first', 'second']);
});

test('opening a saved viewer passes only its opaque asset ID to the router',async()=>{
  const {page}=fixture();const routes=[];page.getUIContext=()=>({getRouter:()=>({pushUrl:async route=>routes.push(route)})});
  await page.play({id:'selected-id',name:'RTMP',endpoint:'rtmp://host'});
  assert.deepEqual(routes,[{url:'pages/Player',params:{deviceId:'selected-id'}}]);
});
