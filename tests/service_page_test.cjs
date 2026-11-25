// Node 22.18+. Exercise the service page's production logic, not ArkUI rendering or OS services.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { stripTypeScriptTypes } = require('node:module');
const { MotionRecordingGate } = require('../entry/src/main/ets/util/MotionRecordingGate.ts');
const { RtmpRetryController, canPublish } = require('./rtmp_host_modules.cjs');
const root = path.join(__dirname, '../entry/src/main/ets');
const page = fs.readFileSync(path.join(root, 'pages/Index.ets'), 'utf8');
const logic = page.slice(page.indexOf('const INK'), page.indexOf('  @Builder'))
  .replace(/@Entry\s+@Component\s+struct Index/, 'class Index').replaceAll('@State ', '');
const program = stripTypeScriptTypes(logic + '\n}\nnew Index()');
const model = fs.readFileSync(path.join(root, 'model/RtspServer.ets'), 'utf8');
const modelProgram = stripTypeScriptTypes(model.slice(model.indexOf('export enum RtspEvent')).replaceAll('export ', '') +
  '\n;({ RtspServer, RtspEvent })', { mode: 'transform' });
const capabilities = fs.readFileSync(path.join(root, 'model/CameraCapabilities.ets'), 'utf8');
const parsers = vm.runInNewContext(stripTypeScriptTypes(capabilities.replaceAll('export ', '') +
  '\n;({ parseCameraResolutions, selectCameraResolution, parseCaptureFps, parseCameraDevices, parseEncoderOptions, parseFocusInfo, parseCodecTuning, codecTuningKey, resolveCameraBitrate })'));
const ownershipModule = fs.readFileSync(path.join(root, 'util/RecordingOptions.ets'), 'utf8');
const RecordingOwnership = vm.runInNewContext(stripTypeScriptTypes(ownershipModule.replaceAll('export ', '') + ';RecordingOwnership'));
const osdModule = fs.readFileSync(path.join(root, 'util/OsdSettings.ets'), 'utf8').replace(/^import .*;$/gm, '').replaceAll('export ', '');
const osd = vm.runInNewContext(stripTypeScriptTypes(osdModule + ';({ defaultOsdFont, osdRestartRequired, osdChargeSource })'), {batteryInfo: { BatteryChargeState: {ENABLE:1}, BatteryPluggedType:{} }});
const strings = Object.fromEntries(require('../entry/src/main/resources/base/element/string.json').string.map(e => [e.name,e.value]));
const next = () => new Promise(resolve => setImmediate(resolve));
function gate() { let resolve; const promise = new Promise(r => { resolve = r; }); return { resolve, promise }; }

function rig({ settings = {}, cold = false, grant = true, section } = {}) {
  const calls = [], grants = new Set();
  const ctx = { filesDir: '/test', resourceManager: {getStringByNameSync(key,...args) { assert.ok(strings[key], key); return strings[key].replace(/%[ds]/g, () => args.shift()); }}, applicationInfo: { accessTokenId: 17 } };
  const win = { on: false, getWindowProperties() { return { isKeepScreenOn: this.on }; },
    async setWindowKeepScreenOn(on) { this.on = on; calls.push(`screen:${on}`); } };
  const bg = { BackgroundMode: { AUDIO_RECORDING: 1 },
    async startBackgroundRunning() { calls.push('bg:start'); },
    async stopBackgroundRunning() { calls.push('bg:stop'); } };
  const remote = { state: '{"generation":0,"running":false,"width":0,"height":0}', reads: 0, fpsReads: 0 };
  const timers = new Map(); let timerId = 0;
  const { RtspServer, RtspEvent } = vm.runInNewContext(modelProgram, {
    RtmpTlsTransport: class { stop() {} failure() { return remote.tlsFailure || ''; } },
    createRtspServer: () => remote, rtmpFailure: handle => handle.nativeFailure || '', cameraRemoteSizeState: handle => { handle.reads++; return handle.state; },
    serviceViewerCounts: h => { h.countReads = (h.countReads ?? 0) + 1; return h.viewers ?? { rtsp: 0, mjpeg: 0, flv: 0, opus: 0 }; },
    cameraToggleTorch: h => { calls.push('torch'); return h.torchOK === true; }
  });
  class Server extends RtspServer {
    setOsdConfig(cfg) { calls.push(`osd:${cfg.enabled}`); }
    takePendingArchives() { return []; }
    setArchiveCallback(cb) { this.archive = cb; return true; } clearArchiveCallback() { this.archive = undefined; }
    cameraDevices() { return '[]'; } encoderOptions() { return '[]'; } cameraFocusInfo() { return '{}'; }
    dualLayout() { return 0; } micFormat() { return {sampleRate:48000, channels:1}; }
    setOsdDeviceName() {} setOsdGps() {} setOsdBattery() {}
    setStatusCallback(callback) { this.status = callback; }
    boundV4Port(http) { return http ? remote.httpPort : remote.rtspPort; }
    start(cfg) { calls.push('rtsp'); remote.rtspPort = cfg.port; return true; }
    cameraResolutions() { calls.push('sizes'); return '[{"width":1280,"height":720}]'; }
    startCamera() { calls.push('camera'); return true; } stopCamera() { calls.push('stopCamera'); }
    cameraCaptureFps() { remote.fpsReads++; return remote.fps || '{}'; }
    startHttp(opts) { remote.httpPort = opts.port; return true; } stopHttp() { remote.httpPort = 0; } stopTestPattern() {} stop() {}
    setMotion() {} clearStatusCallback() {}
    startMic() { calls.push('mic'); return true; } stopMic() { calls.push('stopMic'); }
    async startRtmp() { calls.push('rtmp'); return true; } stopRtmp() { calls.push('stopRtmp'); }
  }
  const page = vm.runInNewContext(program, {
    $r: (id) => id, RecordingOwnership, MotionRecordingGate, RecordingDownloads: class {disable() {}}, ...osd,
    deliveryMotionSnapshotPath: async () => remote.snapshotPath ?? '',
    onDeliveryCompleted: async (_ctx,path) => {calls.push(`delivered:${path}`); return '';},
    leaseBindings: { set(...args) { (remote.bindings ??= new Map()).set(args[0], args.slice(1)); }, stop(k) { remote.bindings?.delete(k); }, stopAll() { remote.bindings?.clear(); } },
    leaseCredentials: (u, p) => u === 'operator' && p === 'Bound-Secret123',
    queryDualCamera: () => remote.plan, lanServiceAnnouncements: () => ({rtsp:{setBoundPort(){}},http:{setBoundPort(){}},stopAll(){}}),
    RtmpRetryController, canPublish, RtspServer: Server, router: { getParams: () => ({ coldAutoStart: cold, section }), back: () => { calls.push('back'); } },
    pasteboard: { MIMETYPE_TEXT_PLAIN:'text/plain',ShareOption:{LOCALDEVICE:1},
      createData:(type,text)=>({text, prop:{},getProperty(){return this.prop},setProperty(p){this.prop=p}}),
      getSystemPasteboard:()=>({setDataSync:d=>{remote.copied=d}}) },
    getContext: () => ctx, preferences: { getPreferencesSync: () => ({
      getSync: (k, v) => settings[k] ?? v, putSync: (k, v) => { settings[k] = v; }, flush: async () => {}
    }) },
    window: { getLastWindow: async () => win }, backgroundTaskManager: bg,
    notificationManager: { requestEnableNotification: async () => {} },
    wantAgent: { OperationType: {}, WantAgentFlags: {}, getWantAgent: async () => ({}) },
    abilityAccessCtrl: { createAtManager: () => ({
      checkAccessTokenSync: (_, p) => grants.has(p) ? 0 : -1,
      requestPermissionsFromUser: async (_, [p]) => {
        calls.push('permission:' + p); if (grant) grants.add(p); return { authResults: [grant ? 0 : -1] };
      }
    }) },
    deviceInfo: {}, wifiManager: { getIpInfo: () => ({ ipAddress: 0x7f000001 }) }, batteryInfo: {},
    hilog: { warn() {}, error() {} }, ...parsers, RtspEvent,
    setInterval: (fn, ms) => { timers.set(++timerId, { fn, ms }); return timerId; },
    clearInterval: id => timers.delete(id)
  });
  return { page, calls, win, bg, settings, remote, timers };
}

function openCameraService() {
  const r = rig({ settings: { cameraSize_0: '1280x720', password:'host-only_random9' } });
  r.page.aboutToAppear(); r.page.onPageShow(); r.page.onStartServer(); r.page.onToggleHttp();
  return r;
}
test('IGD glue captures authentication and exact native port at successful bind; stop invalidates only that generation', () => {
  const { page, remote } = rig(); page.aboutToAppear(); page.onPageShow();
  page.port = '31005'; page.httpPort = '31006'; page.user = 'operator'; page.password = 'Bound-Secret123';
  page.onStartServer(); page.onToggleHttp();
  const [rp, ra, rtspLive] = remote.bindings.get('RTSP'), [hp, ha, httpLive] = remote.bindings.get('HTTP');
  assert.deepEqual([rp,ra,hp,ha],[31005,true,31006,true]);
  page.password = 'admin'; page.port = '31009'; assert.ok(rtspLive() && httpLive());
  remote.httpPort = 0; assert.equal(httpLive(),false); page.onToggleHttp(); assert.ok(!remote.bindings.has('HTTP'));
  assert.ok(remote.bindings.has('RTSP')); page.onStopServer(); assert.equal(remote.bindings.size,0);
  page.onStartServer(); assert.equal(remote.bindings.get('RTSP')[1],false);
  page.password = 'Bound-Secret123'; page.allowLegacyVideoPassword = true; page.onToggleHttp(); assert.equal(remote.bindings.get('HTTP')[1],false);
  page.aboutToDisappear(); assert.equal(remote.bindings.size,0);
  page.server.start = () => false; page.onStartServer(); assert.ok(!remote.bindings.has('RTSP'));
});

test('preview is a single unconditional full-page surface behind every UI layer', () => {
  assert.equal(page.match(/XComponent\(\{/g)?.length, 1);
  const build = page.slice(page.lastIndexOf('\n  build() {')).replace(/\/\/[^\n]*/g, '');
  assert.match(build, /build\(\) \{\s*Stack\(\) \{\s*XComponent\(\{ id: 'cameraPreview', type: 'surface', libraryname: 'streaming' \}\)\s*\.width\('100%'\)\.height\('100%'\)/);
  assert.match(build, /\.renderFit\(RenderFit\.RESIZE_CONTAIN\)/);
  assert.doesNotMatch(build, /aspectRatio|Scroll\(/);
  assert.ok(build.indexOf('XComponent(') < build.indexOf('if (this.panel'));
});

test('menu and panel back order preserves the active camera, mic and listeners', async () => {
  const { page, calls, settings } = openCameraService();
  await page.requestCameraStart(); await page.onStartMic(); await next();
  const before = [...calls], prefs = { ...settings };
  page.overflowOpen = true; assert.ok(page.onBackPress()); assert.equal(page.overflowOpen, false);
  page.overflowOpen = true; page.openPreference('camera'); assert.equal(page.overflowOpen, false);
  assert.equal(page.panel, 'settings'); assert.equal(page.settingsGroup, 'camera');
  assert.ok(page.onBackPress()); assert.equal(page.settingsGroup, 'root'); assert.equal(page.panel, 'settings');
  assert.ok(page.onBackPress()); assert.equal(page.panel, 'preview');
  for (const panel of ['capture', 'addresses', 'share', 'publish', 'diagnostics']) {
    page.openPanel(panel); assert.ok(page.onBackPress()); assert.equal(page.panel, 'preview');
  }
  // Android BaseServerActivity.onBackPressed: leaving the preview asks first; cancel keeps everything.
  let dialog; page.getUIContext = () => ({ showAlertDialog: d => { dialog = d; } });
  assert.ok(page.onBackPress()); assert.ok(dialog);
  dialog.primaryButton.action();
  assert.ok(page.cameraRunning && page.micRunning && page.running && page.httpRunning);
  assert.deepEqual(calls, before); assert.deepEqual(settings, prefs);
  dialog.secondaryButton.action(); assert.deepEqual(calls.slice(before.length), ['back']);
  assert.equal(page.onBackPress(), false);  // the confirmed exit is not asked again
});

test('location is requested only by a user action; codec and port edits are validated', async () => {
  const gps = 'permission:ohos.permission.APPROXIMATELY_LOCATION';
  const { page, calls, settings, timers } = rig({ settings: { osdEnabled: true, display_gps_location: true } });
  page.aboutToAppear(); page.onPageShow(); page.applyOsd(false); await next();
  assert.ok(!calls.includes(gps));  // entering the page (or Settings) never prompts
  assert.ok(![...timers.values()].some(t => t.ms === 5000));
  page.applyOsd(false, true); await next(); await next();
  assert.ok(calls.includes(gps));  // the explicit toggle may prompt
  page.stopGpsTimer(); page.applyOsd(false); await next();
  assert.ok([...timers.values()].some(t => t.ms === 5000));  // an existing grant is used silently
  page.onPageHide(); assert.ok(![...timers.values()].some(t => t.ms === 5000));  // no polling while hidden

  page.recording = true; page.selectCodec(1);
  assert.equal(page.codecIdx, 0); assert.equal(settings.codec, undefined);
  assert.equal(page.serviceNotice, strings.srv_camera_busy);
  page.recording = false; page.selectCodec(1);
  assert.equal(page.codecIdx, 1); assert.equal(settings.codec, 1);

  for (const bad of ['0', '65536', '99999', '', '80a']) { page.port = bad; assert.equal(page.makeConfig().port, 8554); }
  page.port = '554'; assert.equal(page.makeConfig().port, 554);
  page.fieldNotice(false, 'srv_port_invalid'); assert.equal(page.serviceNotice, strings.srv_port_invalid);
  page.fieldNotice(true, 'srv_port_invalid'); assert.equal(page.serviceNotice, '');
  page.osdColor = '#12'; assert.equal(page.buildOsdConfig().color, '#FFFFFFFF');
  page.osdColor = '#00ff00'; assert.equal(page.buildOsdConfig().color, '#00FF00FF');
});

test('client button uses the four current native counts, including HTTP-only viewers, not RTSP events', () => {
  const { page: p, remote, timers } = openCameraService(); let dialog;
  p.getUIContext = () => ({ showAlertDialog: d => { dialog = d; } });
  remote.viewers = { mjpeg: 2, flv: 1, opus: 3, rtsp: 0 };
  timers.get(p.remoteSizeTimerId).fn(); assert.equal(p.viewerTotal, 6);
  p.clients = 99; remote.viewers = { mjpeg: 1, flv: 2, opus: 0, rtsp: 3 };
  const reads = remote.countReads; p.showViewerCounts(); assert.equal(remote.countReads, reads + 1);
  assert.equal(dialog.message, 'MJPEG: 1\nFLV: 2\nOPUS: 0\nRTSP: 3'); assert.equal(p.viewerTotal, 6);
  remote.viewers = { mjpeg: 0, flv: 0, opus: 0, rtsp: 3 }; p.onToggleHttp(); assert.equal(p.viewerTotal, 3);
  p.onStopServer(); p.showViewerCounts(); assert.equal(p.viewerTotal, 0);
});

test('torch is a manual single-camera operation; disabled states do not call native and failures are visible', () => {
  const { page: p, calls, remote } = rig(); const notices = [];
  p.getUIContext = () => ({ getPromptAction: () => ({ showToast: t => notices.push(t.message) }) });
  for (const [running, camera, starting, pattern, dual] of [[false,true,false,false,0],[true,false,false,false,0],
    [true,true,true,false,0],[true,true,false,true,0],[true,true,false,false,2]]) {
    Object.assign(p, { running, cameraRunning: camera, cameraStarting: starting, patternRunning: pattern, dualLayout: dual });
    assert.equal(p.torchAvailable(),false); p.togglePreviewTorch();
  }
  assert.deepEqual(calls, []); p.dualLayout = 0;
  p.togglePreviewTorch(); remote.torchOK = true; p.togglePreviewTorch();
  assert.deepEqual(calls, ['torch','torch']); assert.deepEqual(notices, ['app.string.svc_ui_torch_failed']);
});

test('toolbar uses bundled camera action icons; settings and capture remain under More', () => {
  const chrome = page.slice(page.indexOf('\n  previewChrome()'), page.indexOf('\n  overflowMenu()'));
  for (const icon of ['switch_webcam','motion_detection','flash_on','imagesize']) {
    assert.ok(chrome.includes(`app.media.svc_ui_${icon}`));
    const png = fs.readFileSync(path.join(root, `../resources/base/media/svc_ui_${icon}.png`));
    assert.equal(png.subarray(0,8).toString('hex'), '89504e470d0a1a0a');
  }
  assert.match(chrome, /this\.showViewerCounts\(\)/); assert.doesNotMatch(chrome, /openPanel\('settings'\)|openPanel\('capture'\)/);
  const more = page.slice(page.indexOf('\n  overflowMenu()'), page.indexOf('\n  panelLayer()'));
  for (const panel of ['capture','settings']) assert.ok(more.includes(`openPanel('${panel}')`));
});

test('settings launcher goes back to caller, without requesting capture or starting services', async () => {
  const { page, calls } = rig({ section: 'settings', settings: { auto_rtsp_after_turn_on: true } });
  page.aboutToAppear(); page.onPageShow(); await next();
  assert.equal(page.panel, 'settings'); page.openPreference('camera'); assert.ok(page.onBackPress());
  assert.equal(page.settingsGroup, 'root'); assert.equal(page.onBackPress(), false);
  assert.ok(!calls.some(c => c.startsWith('permission:') || c === 'rtsp' || c === 'camera'));
});

test('QR only presents the selected local endpoint; all new UI strings exist in both locales', () => {
  const share = page.slice(page.indexOf('\n  sharePanel()'), page.indexOf('\n  publishPanel()'));
  assert.match(share, /QRCode\(this.shareWeb && this.httpRunning \? this.httpUrl : this.rtspUrl\)/);
  assert.doesNotMatch(share, /password|this\.user\b|rtmpUrl|copyRtspAddress\(true\)|router\./);
  const keys = [...new Set([...page.matchAll(/app\.string\.(svc_ui_\w+)/g)].map(m => m[1]))].sort();
  for (const locale of ['base', 'zh_CN']) {
    const entries = require(`../entry/src/main/resources/${locale}/element/svc_ui.json`).string;
    assert.deepEqual(entries.map(e => e.name).sort(), keys); assert.ok(entries.every(e => e.value.trim()));
  }
});

function publishState(remote, generation, width, height) {
  remote.state = JSON.stringify({ generation, running: width !== 0, width, height });
}
const sizeBroadcast = page => page.server.status(4, 999, 'rtsp://user:secret@invalid/');

test('size broadcast reads actual capture pixels and FPS, not event detail or client count', async () => {
  const { page, calls, settings, remote } = openCameraService();
  page.clients = 3;
  sizeBroadcast(page); // Generation zero, camera closed; this is not a client connection.
  assert.equal(page.cameraRunning, false);
  await page.requestCameraStart();
  publishState(remote, 1, 1920, 1080);
  remote.fps = '{"ranges":[{"min":15,"max":30}],"active":{"min":15,"max":30}}';
  sizeBroadcast(page);
  assert.ok(page.cameraRunning);
  assert.deepEqual([page.cameraWidth, page.cameraHeight], [1920, 1080]);
  assert.ok(page.resolutions.some(s => s.width === 1920 && s.height === 1080));
  assert.equal(page.fpsInfo.active.max, 30);
  assert.equal(page.clients, 3);
  const fpsReads = remote.fpsReads;
  sizeBroadcast(page);
  assert.equal(remote.fpsReads, fpsReads, 'same generation must not re-query FPS');
  publishState(remote, 2, 1920, 1080); // Rollback rebuilt a session of identical size.
  remote.fps = '{"ranges":[],"active":{"min":20,"max":20}}';
  sizeBroadcast(page);
  assert.equal(page.fpsInfo.active.max, 20);
  publishState(remote, 3, 0, 0); sizeBroadcast(page);
  assert.equal(page.cameraRunning, false);
  assert.equal(page.fpsInfo.ranges.length, 0); assert.equal(page.fpsInfo.active, undefined);
  assert.deepEqual([page.cameraWidth, page.cameraHeight], [1920, 1080], '0x0 is not a selection');
  assert.equal(settings.cameraSize_0, '1280x720');
  assert.equal(calls.filter(c => c === 'camera').length, 1);
  assert.ok(!page.logs.join('\n').includes('secret'));
});

test('invalid or old snapshots never advance the watermark or expose raw data', () => {
  const { page, remote } = openCameraService();
  for (const state of ['secret', 'null', '{}', '{"generation":9,"running":"true","width":1,"height":1}',
    '{"generation":9,"running":true,"width":0,"height":720}',
    '{"generation":9,"running":false,"width":1,"height":0}']) {
    remote.state = state; sizeBroadcast(page);
    assert.equal(page.remoteSizeGeneration, 0); assert.equal(page.cameraRunning, false);
  }
  publishState(remote, 2, 640, 480); sizeBroadcast(page);
  publishState(remote, 1, 0, 0); sizeBroadcast(page);
  assert.ok(page.cameraRunning); assert.equal(page.cameraWidth, 640);
  assert.ok(!page.logs.join('\n').includes('secret'));
});

test('lost broadcasts recover on one-second poll or page show; all shutdown paths clear poll', async () => {
  const { page, timers, remote, calls } = openCameraService();
  const tick = () => {
    assert.equal(timers.size, 1);
    const timer = timers.get(page.remoteSizeTimerId); assert.equal(timer.ms, 1000); return timer.fn;
  };
  publishState(remote, 1, 640, 480); tick()();
  assert.equal(page.cameraWidth, 640);
  const oldTick = tick(); page.onPageHide(); assert.equal(timers.size, 0);
  const reads = remote.reads; oldTick(); assert.equal(remote.reads, reads);
  publishState(remote, 2, 960, 540); page.onPageShow(); await next();
  assert.equal(page.cameraWidth, 960); tick();
  publishState(remote, 3, 0, 0); page.onToggleHttp();
  assert.equal(page.cameraRunning, false); assert.equal(timers.size, 0);
  page.onPageHide(); page.onPageShow(); await next();
  assert.equal(timers.size, 0); assert.ok(!calls.includes('camera'));
  page.onToggleHttp(); tick(); page.onStopServer(); assert.equal(timers.size, 0);
  page.onStartServer(); page.onToggleHttp(); tick(); page.aboutToDisappear(); assert.equal(timers.size, 0);
});

test('local stop/start supersedes undelivered remote state but later changes still apply', async () => {
  const { page, remote, calls } = openCameraService();
  await page.requestCameraStart();
  publishState(remote, 1, 1920, 1080); // Intentionally withhold both broadcast and timer.
  page.onStopCamera(); sizeBroadcast(page);
  assert.equal(page.cameraRunning, false); assert.equal(page.cameraWidth, 1280);
  publishState(remote, 2, 0, 0);
  const pending = page.requestCameraStart(); sizeBroadcast(page); await pending;
  sizeBroadcast(page);
  assert.ok(page.cameraRunning); assert.equal(page.cameraWidth, 1280);
  publishState(remote, 3, 640, 480); sizeBroadcast(page);
  assert.equal(page.cameraWidth, 640); assert.equal(calls.filter(c => c === 'camera').length, 2);
});

test('lost remote failure during pending foreground repair cannot reopen the camera', async () => {
  const { page, remote, calls } = openCameraService();
  await page.requestCameraStart(); page.onPageHide();
  const permission = gate(); page.requestCapturePermission = () => permission.promise;
  page.onPageShow(); assert.equal(page.cameraStarting, true);
  publishState(remote, 1, 0, 0); permission.resolve(true); await next();
  assert.equal(page.cameraRunning, false); assert.equal(calls.filter(c => c === 'camera').length, 1);
});

const cameraPerm = 'permission:ohos.permission.CAMERA';
const source = { auto_rtmp_after_turn_on: true, osdEnabled: true, rtmpUrl: 'rtmp://test/live/secret' };
test('cold RTMP alone: restore overlay, then server, permission, camera and publisher; no mic permission', async () => {
  const { page, calls } = rig({ cold: true, settings: { ...source } });
  page.aboutToAppear(); page.onPageShow(); await next();
  assert.deepEqual(calls, ['osd:true', 'rtsp', cameraPerm, 'sizes', 'camera', 'rtmp']);
  page.onPageHide(); page.onPageShow(); await next();
  assert.equal(calls.filter(s => s === 'rtmp').length, 1);
  assert.equal(calls.filter(s => s === cameraPerm).length, 1);
  assert.equal(page.rtmpState, 'publishing');
  assert.ok(!page.logs.join('\n').includes('secret'));
});

test('manual open ignores auto flags; missing target and denied capture are visible and cannot publish', async () => {
  for (const [cold, url, grant, expectServer] of [[false, source.rtmpUrl, true, false],
    [true, '', true, false], [true, source.rtmpUrl, false, true]]) {
    const { page, calls } = rig({ cold, grant, settings: { ...source, rtmpUrl: url } });
    page.aboutToAppear(); page.onPageShow(); await next();
    assert.equal(calls.includes('rtsp'), expectServer);
    assert.equal(calls.includes('rtmp'), false);
    assert.equal(calls.includes('camera'), false);
    assert.equal(Boolean(page.serviceNotice), cold);
  }
});

test('fresh profile selection replaces target and cancels old publisher; ordinary foreground preserves draft', async () => {
  const { page, settings, calls } = rig({ cold: true, settings: { ...source } });
  page.aboutToAppear(); page.onPageShow(); await next();
  settings.rtmpUrl = 'rtmp://other/live/next';
  page.onPageHide(); page.onPageShow(); await next();
  assert.equal(page.rtmpUrl, settings.rtmpUrl);
  assert.equal(page.rtmpState, 'idle');
  assert.ok(calls.includes('stopRtmp'));
  page.rtmpUrl = 'unfinished'; page.onPageShow();
  assert.equal(page.rtmpUrl, 'unfinished');
});

test('stop mic during a pending continuous-task start eventually stops the OS task', async () => {
  const { page, bg, calls } = rig();
  const started = gate(), finish = gate();
  bg.startBackgroundRunning = async () => { started.resolve(); await finish.promise; calls.push('bg:start'); };
  page.aboutToAppear(); page.onPageShow(); page.onStartServer();
  await page.onStartMic(); await started.promise;
  page.onStopMic(); assert.equal(page.micRunning, false);
  finish.resolve(); await next();
  assert.deepEqual(calls.filter(c => c.startsWith('bg:')), ['bg:start', 'bg:stop']);
  assert.ok(calls.includes('permission:ohos.permission.MICROPHONE'));
  assert.ok(!calls.includes(cameraPerm));
});

test('keep-screen-on restores an owned flag after an in-flight enable, never clears a pre-existing flag', async () => {
  for (const originallyOn of [false, true]) {
    const { page, win } = rig({ settings: { keepScreenOn: true } });
    win.on = originallyOn;
    const gate1 = gate();
    win.setWindowKeepScreenOn = async on => { if (on) await gate1.promise; win.on = on; };
    page.aboutToAppear(); page.onPageShow(); await next();
    page.onPageHide(); gate1.resolve(); await next();
    assert.equal(win.on, originallyOn);
  }
});


test('restored lens, codec and mic preferences reach native starts; read-back never overwrites the request', async () => {
  const settings = { codec: 1, captureRotation:90,captureMirror:true,cameraDevice_0: 'wide', cameraSize_id_wide: '960x540',
    'encoder_video/hevc': 'chip', 'camera.tuning.hevc': '{"bitrate":0,"iFrameMs":3000}', micRate: 0, micChannels: 2 };
  const { page, remote } = rig({settings});
  page.server.cameraDevices = () => '[{"id":"wide","front":false}]';
  page.server.encoderOptions = () => '[{"name":"chip","hardware":true,"profiles":[],"minBitrate":1,"maxBitrate":9000000}]';
  page.server.cameraResolutions = (front,id) => { assert.equal(front,false); assert.equal(id,'wide'); return '[{"width":960,"height":540}]'; };
  let cameraArgs, micArgs, mode, center;
  page.server.startCamera = (...args) => { cameraArgs=args; return true; };
  page.server.startMic = (...args) => { micArgs=args; return true; };
  page.server.micFormat = () => ({sampleRate:44100,channels:1});
  page.server.cameraFocusInfo = () => '{"modes":[1,2],"active":2}';
  page.server.cameraSetFocusMode = v => {mode=v;return true};
  page.server.cameraFocusAt = (x,y) => {center=[x,y];return true};
  remote.plan = {primaryId:'different'};
  page.aboutToAppear(); page.onPageShow(); page.onStartServer(); await page.requestCameraStart();
  assert.deepEqual(cameraArgs,[960,540,960*540*2,'video/hevc',false,3000,'wide','chip',-1,90,true]);
  assert.equal(page.dualAvailable,false);
  page.setFocusMode(8); assert.equal(mode,undefined); page.setFocusMode(2); page.focusCenter();
  assert.equal(mode,2); assert.deepEqual(center,[.5,.5]);
  await page.onStartMic(); assert.deepEqual(micArgs,[0,2]); assert.equal(page.actualMicChannels,1);
  assert.equal(page.actualMicRate,44100); assert.equal(settings.micChannels,2); assert.equal(settings.micRate,0);
  page.onStopMic(); assert.equal(page.actualMicRate,0);
});

test('dual layout is offered only for the active lens and UI reads back rollback and fatal failure', async () => {
  const {page,remote} = openCameraService(); remote.plan = {primaryId:'wide',secondaryId:'front'};
  page.server.cameraDevices = () => '[{"id":"wide","front":false}]';
  let layout=0, result=0;
  page.server.dualLayout=()=>layout;
  page.server.setDualLayout=(n,plan)=>{assert.equal(plan.primaryId,'wide'); if(!result)layout=n; return result};
  await page.requestCameraStart(); assert.equal(page.dualAvailable,true);
  page.setDualLayout(2); assert.equal(page.dualLayout,2);
  page.selectFacing(1); assert.equal(page.facingIdx,0);
  result=2; page.setDualLayout(3); assert.equal(page.dualLayout,2); assert.equal(page.cameraRunning,true);
  result=3; page.setDualLayout(0); assert.equal(page.dualLayout,0); assert.equal(page.cameraRunning,false);
});

test('independent recording is validated before permission; owned mic closes, existing mic is preserved', async () => {
  const {page,calls} = rig({settings:{recordAudioMode:2,recordVideoMode:2,recordBitrateKbps:'4000',recordGopMs:'2000'}});
  let writer=false, received;
  page.server.startRecord=(path,options)=>{received={...options};writer=true;return true};
  page.server.isRecording=()=>writer; page.server.stopRecord=()=>{writer=false};
  page.aboutToAppear(); page.onPageShow(); page.onStartServer();
  page.recordGopMs='1'; await page.onToggleRecord(); assert.equal(writer,false); assert.ok(!calls.includes('mic'));
  page.recordGopMs='2000'; await page.onToggleRecord(); assert.equal(writer,true); assert.ok(page.micRunning);
  assert.deepEqual(received,{fourGbLimit:true,segmentMinutes:10,includeAudio:true,videoMime:'video/hevc',videoBitrate:4000000,gopMs:2000});
  await page.onToggleRecord(); assert.equal(page.micRunning,false);
  await page.onStartMic(); page.recordAudio=0; await page.onToggleRecord(); assert.equal(received.includeAudio,false);
  await page.onToggleRecord(); assert.equal(page.micRunning,true);
});

test('stop during mic permission invalidates the manual recording ticket', async () => {
  const {page,calls}=rig({settings:{recordAudioMode:2}}); page.aboutToAppear();page.onPageShow();page.onStartServer();
  const ask=gate();page.requestCapturePermission=()=>ask.promise;page.server.startRecord=()=>assert.fail('stale recorder');
  const start=page.onToggleRecord();assert.equal(page.recordStarting,true);page.onStopServer();ask.resolve(true);await start;
  assert.ok(!calls.includes('mic'));assert.equal(page.recording,false);assert.equal(page.recordStarting,false);
});

test('legacy password is off until explicitly confirmed; changing a preference does not reconfigure a listener', () => {
  const {page,settings}=rig({settings:{password:'host-only_random9'}});let dialog;const options=[];page.getUIContext=()=>({showAlertDialog:d=>{dialog=d}});
  page.server.startHttp=o=>{options.push(o.allowLegacyVideoPassword);return true};
  page.aboutToAppear();page.onPageShow();page.onStartServer();page.onToggleHttp();
  page.configureLegacyVideoPassword();dialog.primaryButton.action();assert.equal(page.allowLegacyVideoPassword,false);
  page.configureLegacyVideoPassword();dialog.secondaryButton.action();assert.equal(settings.allowLegacyVideoPassword,true);
  assert.deepEqual(options,[false]); page.onToggleHttp();page.onToggleHttp();assert.deepEqual(options,[false,true]);
  page.configureLegacyVideoPassword();assert.equal(settings.allowLegacyVideoPassword,false);
});

test('manual recording permission returned after hide/show cannot open a writer', async () => {
  const { page } = openCameraService();
  const permission = gate(); page.recordAudio = 2;
  page.requestCapturePermission = () => permission.promise;
  let starts = 0; page.server.startRecord = () => { starts++; return true; };
  const pending = page.onToggleRecord(); assert.equal(page.recordStarting, true);
  page.onPageHide(); page.onPageShow(); permission.resolve(true); await pending;
  assert.equal(starts, 0); assert.equal(page.recording, false); assert.equal(page.recordTicket, 0);
});

test('leaving motion mode cancels pending camera permission even after returning', async () => {
  const { page, calls } = openCameraService(); const permission = gate();
  page.requestCapturePermission = () => permission.promise;
  const pending = page.requestCameraStart(true);
  page.onPageHide(); page.onPageShow(); permission.resolve(true); await pending;
  assert.equal(calls.includes('camera'), false); assert.equal(page.motionEnabled, false);
});


test('watermark-only restarts for the first and last compositor user, not for text edits or both toggles', async () => {
  const {page}=openCameraService(); await page.requestCameraStart();
  const trace=[];page.server.setOsdConfig=c=>{trace.push(`config:${c.enabled}/${c.wmEnabled}`);return true};
  page.requestCameraStart=()=>trace.push('restart');
  page.onToggleWatermark(true);page.onToggleOsd(true);page.onToggleWatermark(false);page.onToggleOsd(false);
  assert.deepEqual(trace,['config:false/true','restart','config:true/true','config:true/false','config:false/false','restart']);
});

test('hidden/visible repair does not interrupt an active recorder or mark a busy camera stopped', async () => {
  const {page,calls}=openCameraService();await page.requestCameraStart();page.recording=true;page.server.isRecording=()=>true;
  page.onPageHide();page.onPageShow();await next();assert.equal(calls.filter(x=>x==='camera').length,1);
  assert.ok(page.recording);assert.ok(page.cameraRunning);page.selectFacing(1);assert.equal(page.facingIdx,0);
});

test('completion routes only issued motion snapshots and native closed archives; disabled Download never copies', async () => {
  const {page,remote,calls}=openCameraService();const archive='/test/ipc_2026.mp4';
  page.server.archive(archive);await next();assert.ok(calls.includes(`delivered:${archive}`));
  page.motionEnabled=true;page.server.motionActive=()=>true;
  remote.snapshotPath='/test/MD/IPS_2026.bmp';let snapshots=0;page.server.takeSnapshot=()=>{snapshots++;return true;};
  await page.requestMotionSnapshot(page.episodeGeneration);assert.equal(snapshots,1);
  page.server.status(2,0,'snapshot saved /test/snapshot.bmp');assert.equal(calls.filter(c=>c.startsWith('delivered:')).length,1);
  page.server.status(2,0,`snapshot saved ${remote.snapshotPath}`);page.server.status(2,0,`snapshot saved ${remote.snapshotPath}`);
  assert.deepEqual(calls.filter(c=>c.startsWith('delivered:')),[`delivered:${archive}`,`delivered:${remote.snapshotPath}`]);
});


test('factory and weak passwords never expose the HTTP LAN listener; RTSP behavior is unchanged', () => {
  const {page}=rig();let starts=0;page.server.startHttp=()=>{starts++;return true};page.aboutToAppear();page.onPageShow();page.onStartServer();
  for(const value of ['', 'admin','password1234567','aaaaaaaaaaaa']) {
    page.password=value;page.onToggleHttp();assert.equal(page.httpRunning,false);assert.equal(starts,0);assert.ok(page.serviceNotice);
  }
  assert.equal(page.running,true);page.password='Lan-only long$pass';page.onToggleHttp();assert.equal(starts,1);
});

test('normal stop drains completed files once; true retirement cancels queued files and Download authorization', async () => {
  const {page,calls}=openCameraService();let paths=[],drains=0; const copied=[];
  page.copyDownloads=true;page.downloads={disable(){},async copyCompleted(_ctx,path){copied.push(path);return true}};
  page.server.takePendingArchives=()=>{drains++;const done=paths;paths=[];return done};
  const queued=()=>{const cb=page.server.archive;paths.splice(0).forEach(p=>cb?.(p))};
  paths.push('/test/ipc_final.mp4');page.onStopServer();assert.ok(page.server.archive);queued();await next();
  assert.equal(drains,1);assert.deepEqual(copied,['/test/ipc_final.mp4']);
  assert.deepEqual(calls.filter(c=>c.startsWith('delivered:')),['delivered:/test/ipc_final.mp4']);
  page.onStartServer();paths.push('/test/ipc_retired.mp4');const retired=page.server.archive;page.aboutToDisappear();
  assert.equal(page.copyDownloads,false);assert.equal(page.server.archive,undefined);retired(paths[0]);queued();await next();
  assert.equal(drains,1);assert.equal(copied.length,1);assert.equal(calls.filter(c=>c.startsWith('delivered:')).length,1);
});


test('clipboard defaults to a credential-free local-only URL and adding credentials is explicit', () => {
  const {page,remote}=rig({settings:{user:'test user',password:'secret@value'}});page.aboutToAppear();
  page.getUIContext=()=>({getPromptAction:()=>({showToast(){}})});page.copyRtspAddress();
  assert.equal(remote.copied.text,page.rtspUrl);assert.equal(remote.copied.prop.shareOption,1);
  page.copyRtspAddress(true);assert.match(remote.copied.text,/test%20user:secret%40value@/);assert.equal(remote.copied.prop.shareOption,1);
  assert.ok(!page.logs.join(' ').includes('secret'));
});

test('TLS wrapper never downgrades, passes verified session token and cancels stale authentication',async()=>{
  const calls=[];let tls;
  class Transport {constructor(){tls=this} stop(){calls.push('tls:stop')} start(_url,begin){this.begin=begin;return this.wait?.promise??begin(73)} failure(){return this.cause??''} }
  const {RtspServer}=vm.runInNewContext(modelProgram,{RtmpTlsTransport:Transport,createRtspServer:()=>({}),
    rtmpStart:async(_h,url,token)=>{calls.push(`start:${token??'plain'}`);return true},rtmpStop:()=>calls.push('native:stop'),rtmpFailure:()=>''});
  const server=new RtspServer();assert.ok(await server.startRtmp('rtmp://example/app/id'));assert.ok(await server.startRtmp('rtmps://example/app/id'));
  assert.deepEqual(calls,['tls:stop','start:plain','tls:stop','start:73']);
  tls.wait=gate();const old=server.startRtmp('rtmps://example/app/id');server.stopRtmp();tls.cause='TLS verification failed';tls.wait.resolve(false);
  assert.equal(await old,false);assert.equal(server.rtmpFailure(),'');assert.deepEqual(calls.slice(-2),['tls:stop','native:stop']);
  tls.wait=gate();const failed=server.startRtmp('rtmps://example/app/id');tls.wait.resolve(false);await assert.rejects(failed,/TLS verification failed/);
  assert.equal(server.rtmpFailure(),'TLS verification failed');assert.equal(calls.filter(c=>c==='start:plain').length,1);
});


test('explicit RTMP rejection beats simultaneous TLS close on the event, the poll, or both', async () => {
  for (const source of ['event', 'poll', 'event-with-snapshot']) {
    const {page, remote, timers} = rig(); page.aboutToAppear(); page.onPageShow(); page.onStartServer();
    const connecting = gate();
    page.server.startRtmp = () => { page.server.tlsPublisher = true; return connecting.promise; };
    page.rtmpUrl = 'rtmps://private.example/app/secret';
    page.onStartRtmp();
    const rejection = 'connection lost: server rejected: NetStream.Publish.Denied';
    remote.tlsFailure = 'RTMPS connection closed';
    if (source === 'event') {
      // Native startup failures need not be installed as an active snapshot yet.
      page.server.status(3, 0, 'rtmp: startup failed: server rejected: NetConnection.Connect.Rejected');
      connecting.resolve(true); await next(); // late completion cannot revive the session
    } else {
      connecting.resolve(true); await next(); assert.equal(page.rtmpState, 'publishing');
      remote.nativeFailure = rejection;
      if (source === 'poll') timers.get(page.rtmpPollTimerId).fn();
      else page.server.status(3, 0, 'rtmp: connection lost: server closed connection');
    }
    try {
      assert.equal(page.rtmpState, 'idle', source);
      assert.doesNotMatch(page.logs.join(' '), /private|secret|server rejected|NetStream|NetConnection|RTMPS connection closed/);
      // Removing the native active slot immediately drops its rejection; nothing leaks into a replacement.
      remote.nativeFailure = ''; remote.tlsFailure = '';
      assert.equal(page.server.rtmpFailure(), '');
    } finally { page.onStopRtmp(); }
  }
});

test('TLS verification error is a local non-retry failure, not cancellation; domain and key are never logged',async()=>{
  const {page,calls}=rig();page.aboutToAppear();page.onPageShow();page.onStartServer();
  page.rtmpUrl='rtmps://camera-private.example/app/private-key';
  page.server.startRtmp=async()=>{calls.push('secure-start');throw new Error('RTMPS certificate SAN mismatch')};
  page.onStartRtmp();await next();assert.equal(page.rtmpState,'idle');assert.equal(page.rtmpStatus,strings.srv_tls_failed);
  assert.equal(calls.filter(c=>c==='secure-start').length,1);assert.doesNotMatch(page.logs.join(' '),/camera-private|private-key/);
});
