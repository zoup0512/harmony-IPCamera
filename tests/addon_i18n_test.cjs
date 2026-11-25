// Local catalogs and page logic only. No services, sockets, SDK or device required.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const main = join(__dirname, '../entry/src/main');
const pages = { cloud: 'CloudArchive', delivery: 'Delivery', lan: 'LanServiceSettings', onvif: 'OnvifManaged', remote: 'RemoteArchives' };
const source = file => readFileSync(join(main, 'ets', file + '.ets'), 'utf8');
function strings(locale, group) {
  const { string } = JSON.parse(readFileSync(join(main, 'resources', locale, 'element', `addon_${group}.json`), 'utf8'));
  const result = new Map(string.map(({name, value}) => [name, value]));
  assert.equal(result.size, string.length, 'duplicate keys');
  return result;
}
for (const [group, page] of Object.entries(pages)) {
  test(`${page}: resource references, keys and format arguments agree across locales`, () => {
    const code = source('pages/' + page) + (group === 'lan' ? source('model/LanServiceAnnouncements') : '');
    const refs = new Set([...code.matchAll(/\$r\('app\.string\.(addon_\w+)'/g)].map(m => m[1]));
    const en = strings('base', group), zh = strings('zh_CN', group);
    assert.ok(refs.size > 0);
    assert.deepEqual(refs, new Set(en.keys()));
    assert.deepEqual(refs, new Set(zh.keys()));
    for (const [key, value] of en) {
      assert.ok(key.startsWith(`addon_${group}_`) && value.trim() && zh.get(key).trim(), key);
      assert.deepEqual(value.match(/%[sd]/g), zh.get(key).match(/%[sd]/g), key);
    }
  });
}
function pageClass(name, mocks) {
  const code = source('pages/' + name).split(/\n  (?:build\(\)|@Builder)/)[0]
    .replace(/^import .*;$/gm, '').replace(/^@(Entry|Component)\s*$/gm, '')
    .replace('struct ' + name, 'class ' + name).replace(/@State /g, '') + '}';
  return new Function(...Object.keys(mocks), stripTypeScriptTypes(code) + '\nreturn ' + name)(...Object.values(mocks));
}
const $r = (id, ...args) => ({id, args});
for (const locale of ['base', 'zh_CN']) {
  test(`confirmations (${locale}) preserve exact filenames, origin, old/new values and do not send anything`, () => {
    const resources = new Map([...strings(locale, 'cloud'), ...strings(locale, 'onvif')]);
    const format = (id, ...args) => {
      const value = resources.get(id.replace('app.string.', ''));
      assert.ok(value, id); assert.equal((value.match(/%[sd]/g) || []).length, args.length, id);
      return value.replace(/%[sd]/g, () => String(args.shift()));
    };
    let dialog;
    const Cloud = pageClass('CloudArchive', { $r, CloudArchiveService: class {}, AlertDialog: {show: d => dialog = d} });
    const cloud = new Cloud(); cloud.connected = 'signed in'; cloud.askUpload({name: 'ipc_$%s.mp4', size: 120});
    const upload = format(dialog.message.id, ...dialog.message.args);
    assert.ok(upload.includes('ipc_$%s.mp4') && upload.includes('120') && upload.includes('OneDrive'));
    const Onvif = pageClass('OnvifManaged', {$r, getContext: () => ({resourceManager: {getStringSync: format}})});
    const camera = new Onvif(); camera.getUIContext = () => ({showAlertDialog: d => dialog = d});
    camera.session = {target: {origin: 'https://10.0.0.1'}, preview: () => 1};
    camera.profiles = [{name: 'Main'}]; camera.index = 0; camera.password = 'do-not-show';
    camera.streamUri = 'rtsp://10.0.0.1/private?token=do-not-show';
    camera.editor = {original: {size: '640x480', fps: '15', bitrate: '512', level: 'Main'}};
    Object.assign(camera, {resolutionValue: '1280x720', fps: '30', bitrate: '1024', level: 'High'});
    camera.review(); const warning = format(dialog.message.id, ...dialog.message.args);
    for (const s of ['https://10.0.0.1', 'Main', '640x480 → 1280x720', '15 → 30 fps', '512 → 1024 kbps', 'Main → High']) assert.ok(warning.includes(s), s);
    assert.doesNotMatch(warning, /do-not-show|private|token=/);
  });
}
test('single-file cleanup requires a second positive action; stale dialogs and expiration cannot authorize removal', async () => {
  let dialog, expired, calls=0;
  const Cloud = pageClass('CloudArchive', {$r, CloudArchiveService: class {}, AlertDialog:{show:d=>dialog=d},
    getContext:()=>({filesDir:'/mock'}), recordings:()=>[], setTimeout:cb=>{expired=cb;return 1},clearTimeout:()=>{}});
  const page = new Cloud(), row={name:'ipc_010203.mp4',size:120,identity:'inode'};
  page.service={upload:async()=>true,cleanupExpires:()=>Date.now()+3000,account:()=>'',cancel(){},canClean:()=>true,async cleanConfirmed(r){assert.equal(r,row);calls++}};
  page.connected='mock'; await page.upload(row); page.askClean(row);assert.equal(calls,0);
  assert.deepEqual(dialog.message.args,[row.name,row.size]);dialog.primaryButton.action();assert.equal(calls,0);
  expired();dialog.secondaryButton.action();assert.equal(page.cleanKey,'');assert.equal(calls,0);
  page.connected='mock';await page.upload(row);page.askClean(row);dialog.secondaryButton.action();dialog.secondaryButton.action();assert.equal(calls,1);
});
test('unrecognized diagnostic text never reaches remote archive or delivery UI', () => {
  const Remote = pageClass('RemoteArchives', {$r});
  const r = new Remote();
  assert.deepEqual(r.listFailure('https://user:secret@server/private'), $r('app.string.addon_remote_list_failed'));
  assert.deepEqual(r.listFailure('服务器响应 403'), $r('app.string.addon_remote_http', '403'));
  const Delivery = pageClass('Delivery', {$r, MailSettings: class {}, FtpSettings: class {},
    lastDeliveryResult: () => '告警邮件：AUTH secret'});
  const delivery = new Delivery(); delivery.onPageShow();
  assert.deepEqual(delivery.recent, $r('app.string.addon_delivery_failed'));
  assert.deepEqual(delivery.recentTitle, $r('app.string.addon_delivery_recent_mail'));
});
