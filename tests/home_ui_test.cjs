// Host-only tests: execute the page's handlers with isolated router/preference doubles.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const main = join(__dirname, '../entry/src/main');
const source = readFileSync(join(main, 'ets/pages/Devices.ets'), 'utf8');
const handlers = source.split('  @Builder')[0].replace(/^import [^;]+;/gm, '')
  .replace(/^@(Entry|Component)\s*$/gm, '').replace('struct Devices', 'class Devices').replace(/@State /g, '') + '}';
const factory = new Function('$r', 'preferences', 'getContext', 'listDevices',
  stripTypeScriptTypes(handlers) + '\nreturn () => new Devices();');
function fixture(auto = '') {
  let reads = 0;
  const routes = [];
  const newPage = factory(key => key, { getPreferencesSync: () => {
    reads++;
    return { getSync: key => key === auto };
  } }, () => ({}), async () => []);
  function page() {
    const p = newPage();
    p.getUIContext = () => ({ getRouter: () => ({ pushUrl: async r => { routes.push(r); } }) });
    return p;
  }
  return { page, routes, reads: () => reads };
}

test('Back dismisses add or row menu before editor, search, or navigation', () => {
  const p = fixture().page(); p.searching = true; p.query = 'camera';
  p.adding = true; assert.equal(p.onBackPress(), true); assert.equal(p.adding, false);
  assert.equal(p.searching, true); assert.equal(p.query, 'camera');
  p.rowMenuId = 'cam'; assert.equal(p.onBackPress(), true); assert.equal(p.rowMenuId, '');
  p.startEditor('cam', { name: 'Camera', url: 'private-url' });
  assert.equal(p.onBackPress(), true); assert.equal(p.editing, false); assert.equal(p.address, '');
  assert.equal(p.onBackPress(), true); assert.equal(p.searching, false); assert.equal(p.query, '');
  assert.equal(p.onBackPress(), false);
});

test('row actions target the selected result, and Delete only opens confirmation', () => {
  const p = fixture().page(); p.items = [
    { id: '1', name: 'West', endpoint: 'rtsp://cam-west' },
    { id: '2', name: 'East', endpoint: 'https://cam-east' }
  ];
  p.query = ' CAM-EAST '; const [item] = p.matches(); assert.equal(item.id, '2');
  const called = []; p.edit = i => called.push(['edit', i.id]); p.confirmDelete = i => called.push(['confirm', i.id]);
  p.remove = () => assert.fail('menu must never delete directly');
  const menu = p.rowActions(item); assert.deepEqual(menu.map(m => m.value), ['app.string.dm_edit', 'app.string.dm_delete']);
  menu.forEach(m => { p.rowMenuId = item.id; m.action(); assert.equal(p.rowMenuId, ''); });
  assert.deepEqual(called, [['edit','2'],['confirm','2']]);
});

test('settings and server navigation never request auto-start; launch preferences are checked only once per process', async () => {
  for (const pref of ['', 'auto_rtsp_after_turn_on', 'auto_rtmp_after_turn_on']) {
    const f = fixture(pref), p = f.page(); p.onPageShow(); p.onPageShow(); f.page().onPageShow();
    assert.equal(f.reads(), 1);
    await p.openSettings(); await p.open('pages/Index');
    assert.deepEqual(f.routes, [...(pref ? [{ url:'pages/Index', params:{coldAutoStart:true} }] : []),
      { url:'pages/Index', params:{section:'settings'} }, { url:'pages/Index' }]);
  }
});

test('scrolling down yields space; scrolling up restores add and never hides an open menu', () => {
  const p = fixture().page(); p.onListPosition(2); assert.equal(p.fabHidden, true);
  p.onListPosition(1); assert.equal(p.fabHidden, false);
  p.adding = true; p.onListPosition(3); assert.equal(p.fabHidden, false);
  p.rowMenuId = 'x'; p.onPageHide(); assert.equal(p.rowMenuId, ''); assert.equal(p.adding, false);
});

test('layout reproduces the reference FAB overlap without covering the service label, and localizes all new labels', () => {
  const build = source.slice(source.indexOf('  build()'));
  assert.match(build, /Stack\(\{ alignContent: Alignment.BottomEnd \}\)[\s\S]*?this\.listArea\(\)[\s\S]*?\.layoutWeight\(1\)[\s\S]*?this\.serviceBar\(\)[\s\S]*?this\.fab\(\)/);
  // 1080px / 480 dpi reference: FAB centre (951, 2109), service divider y=2103, nav area starts y=2268.
  assert.match(source, /FAB_SIZE = 56;/); assert.match(source, /FAB_RIGHT = 15;/);
  assert.match(source, /FAB_BOTTOM = 25;/); assert.match(source, /SERVICE_HEIGHT = 54;/);
  assert.match(source, /right: FAB_RIGHT \+ FAB_SIZE \+ 6/);
  assert.match(source,/\.contentEndOffset\(FAB_SIZE \+ FAB_BOTTOM/);
  assert.match(source,/onScrollStop\(\(\) => \{ this\.fabHidden = false/);
  const choices = source.slice(source.indexOf('  addChoices()'), source.indexOf('  fab()'));
  assert.match(choices, /home_ui_add_onvif[\s\S]*?pages\/OnvifScan[\s\S]*?home_ui_add_ip[\s\S]*?startEditor[\s\S]*?home_ui_scan_unavailable[\s\S]*?this\.canScan[\s\S]*?this\.scan\(\)/);
  for (const asset of ['home_ui_empty','home_ui_camera']) {
    assert.equal(readFileSync(join(main, `resources/base/media/${asset}.png`)).readUInt32BE(0), 0x89504e47);
  }
  const keys = [...new Set([...source.matchAll(/app\.string\.(home_ui_\w+)/g)].map(m => m[1]))].sort();
  const locales = ['base','zh_CN'].map(dir => JSON.parse(readFileSync(join(main, `resources/${dir}/element/home_ui.json`))).string);
  for (const rows of locales) assert.deepEqual(rows.map(r => r.name).sort(), keys);
  for (const {name, value} of locales[0]) {
    const zh = locales[1].find(r => r.name === name).value;
    assert.match(zh, /[\u3400-\u9fff]/); assert.equal((value.match(/%s/g) || []).length, (zh.match(/%s/g) || []).length);
  }
});
