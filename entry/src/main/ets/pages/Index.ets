import { wifiManager } from '@kit.ConnectivityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { abilityAccessCtrl, common, Permissions, wantAgent } from '@kit.AbilityKit';
import { BusinessError, batteryInfo, deviceInfo } from '@kit.BasicServicesKit';
import { geoLocationManager } from '@kit.LocationKit';
import { notificationManager } from '@kit.NotificationKit';
import { preferences } from '@kit.ArkData';
import { image } from '@kit.ImageKit';
import { fileIo, picker } from '@kit.CoreFileKit';
import { HttpOptions, OsdConfig, RtspEvent, RtspServer, RtspStartConfig } from '../model/RtspServer';

const TAG: string = 'IPCameraIndex';
const DOMAIN: number = 0xC010;
const BUNDLE_NAME: string = 'com.shenyaocn.harmony.ipcamera';

const RESOLUTIONS: string[] = ['640x480', '1280x720', '1920x1080'];
const BITRATES: string[] = ['1 Mbps', '2 Mbps', '4 Mbps'];
const CODECS: string[] = ['H.264', 'H.265'];
const FACING: string[] = ['后置', '前置'];
const OSD_POSITIONS: string[] = ['左上', '右上', '左下', '右下'];
const OSD_FONT_STYLES: string[] = ['常规', '加粗', '斜体', '粗斜体'];

interface Settings {
  resolution: number;
  bitrate: number;
  codec: number;
  facing: number;
  port: number;
  path: string;
  user: string;
  password: string;
  rtmpUrl: string;
  httpPort: number;
  motionTimeout: number;
  keyframeInterval: number;
}

function toSelectOptions(arr: string[]): SelectOption[] {
  const out: SelectOption[] = [];
  arr.forEach((v: string) => {
    const o: SelectOption = { value: v };
    out.push(o);
  });
  return out;
}

function loadSettings(ctx: common.UIAbilityContext): Settings {
  const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
  const s: Settings = {
    resolution: pref.getSync('resolution', 1) as number,
    bitrate: pref.getSync('bitrate', 1) as number,
    codec: pref.getSync('codec', 0) as number,
    facing: pref.getSync('facing', 0) as number,
    port: pref.getSync('port', 8554) as number,
    httpPort: pref.getSync('httpPort', 8081) as number,
    motionTimeout: pref.getSync('motionTimeout', 15) as number,
    keyframeInterval: pref.getSync('keyframeInterval', 2) as number,
    path: pref.getSync('path', 'live') as string,
    user: pref.getSync('user', 'admin') as string,
    password: pref.getSync('password', 'admin') as string,
    rtmpUrl: pref.getSync('rtmpUrl', 'rtmp://127.0.0.1:11935/live/stream') as string
  };
  if (s.resolution < 0 || s.resolution >= RESOLUTIONS.length) s.resolution = 1;
  if (s.bitrate < 0 || s.bitrate >= BITRATES.length) s.bitrate = 1;
  if (s.codec < 0 || s.codec >= CODECS.length) s.codec = 0;
  if (s.facing < 0 || s.facing >= FACING.length) s.facing = 0;
  if (!s.rtmpUrl.startsWith('rtmp://')) {
    s.rtmpUrl = 'rtmp://127.0.0.1:11935/live/stream';
  }
  return s;
}

@Entry
@Component
struct Index {
  @State running: boolean = false;
  @State patternRunning: boolean = false;
  @State cameraRunning: boolean = false;
  @State micRunning: boolean = false;
  @State rtmpRunning: boolean = false;
  @State recording: boolean = false;
  @State httpRunning: boolean = false;
  @State motionEnabled: boolean = false;
  @State httpPort: string = '8081';
  @State motionTimeout: string = '15';
  @State keyframeInterval: string = '2';
  @State clients: number = 0;
  @State logs: string[] = [];
  @State rtspUrl: string = '';
  @State resolutionIdx: number = 1;
  @State bitrateIdx: number = 1;
  @State codecIdx: number = 0;
  @State facingIdx: number = 0;
  @State port: string = '8554';
  @State streamPath: string = 'live';
  @State user: string = 'admin';
  @State password: string = 'admin';
  @State rtmpUrl: string = '';
  @State osdEnabled: boolean = false;
  @State osdPositionIdx: number = 0;
  @State osdFontStyleIdx: number = 0;
  @State osdPadding: string = '1';
  @State osdColor: string = '#FFFFFFFF';
  @State osdShowTimestamp: boolean = true;
  @State osdShowDevName: boolean = true;
  @State osdShowBattery: boolean = true;
  @State osdShowGps: boolean = false;
  @State osdSpeedMph: boolean = false;
  @State osdCustomText: string = '';
  @State wmEnabled: boolean = false;
  @State wmPositionIdx: number = 0;
  @State wmPadding: string = '0';
  @State wmMaxArea: string = '10';
  @State osdSpeedUnitIdx: number = 0;
  private keepAliveOn: boolean = false;
  private server: RtspServer = new RtspServer();
  private osdWatermarkPm: image.PixelMap | null = null;
  private gpsTimerId: number = -1;
  private batteryTimerId: number = -1;

  aboutToAppear(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    const s: Settings = loadSettings(ctx);
    this.resolutionIdx = s.resolution;
    this.bitrateIdx = s.bitrate;
    this.codecIdx = s.codec;
    this.facingIdx = s.facing;
    this.port = `${s.port}`;
    this.httpPort = `${s.httpPort}`;
    this.motionTimeout = `${s.motionTimeout}`;
    this.keyframeInterval = `${s.keyframeInterval}`;
    this.streamPath = s.path;
    this.user = s.user;
    this.password = s.password;
    this.rtmpUrl = s.rtmpUrl;
    this.loadOsdSettings();
    this.server.setOsdDeviceName(`${deviceInfo.marketName} ${deviceInfo.productModel}`);
    this.refreshUrl();
    this.server.setStatusCallback((event: number, clients: number, detail: string) => {
      let name: string = 'EVENT';
      if (event === RtspEvent.Connected) {
        name = 'CONNECT';
      } else if (event === RtspEvent.Disconnected) {
        name = 'DISCONNECT';
      } else if (event === RtspEvent.Playing) {
        name = 'PLAY';
      } else if (event === RtspEvent.Error) {
        name = 'ERROR';
      }
      this.clients = clients;
      if (detail.startsWith('snapshot') || detail === 'MOTION detected') {
        this.appendLog(detail);
      } else {
        this.appendLog(`${name} ${detail} (clients=${clients})`);
      }
    });
    this.appendLog('ready - 启动服务后 VLC/ffplay 打开 RTSP 地址;相机画面在本页预览');
  }

  aboutToDisappear(): void {
    this.stopGpsTimer();
    this.stopBatteryTimer();
    this.server.stopTestPattern();
    this.server.stopCamera();
    this.server.stopMic();
    this.server.stopRtmp();
    this.rtmpRunning = false;
    this.server.stopHttp();
    this.httpRunning = false;
    this.server.stop();
    if (this.keepAliveOn) {
      this.keepAliveOn = false;
      this.stopKeepAlive();
    }
  }

  saveSetting(key: string, value: preferences.ValueType): void {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
      pref.putSync(key, value);
      pref.flush();
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'saveSetting failed');
    }
  }

  deviceIp(): string {
    try {
      const info: wifiManager.IpInfo = wifiManager.getIpInfo();
      const ip: number = info.ipAddress;
      return `${ip & 0xFF}.${(ip >>> 8) & 0xFF}.${(ip >>> 16) & 0xFF}.${(ip >>> 24) & 0xFF}`;
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'getIpInfo failed');
      return '0.0.0.0';
    }
  }

  refreshUrl(): void {
    this.rtspUrl = `rtsp://${this.user}:${this.password}@${this.deviceIp()}:${this.port}/${this.streamPath}`;
  }

  timestamp(): string {
    const d: Date = new Date();
    const p = (n: number): string => (n < 10 ? '0' : '') + n;
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  appendLog(msg: string): void {
    this.logs = [`[${this.timestamp()}] ${msg}`, ...this.logs].slice(0, 200);
  }

  makeConfig(): RtspStartConfig {
    const cfg: RtspStartConfig = {
      port: parseInt(this.port) || 8554,
      path: this.streamPath.trim() === '' ? 'live' : this.streamPath.trim(),
      user: this.user,
      password: this.password
    };
    return cfg;
  }

  async startKeepAlive(mode: backgroundTaskManager.BackgroundMode): Promise<void> {
    const uiCtx = getContext(this) as common.UIAbilityContext;
    try {
      await notificationManager.requestEnableNotification(uiCtx);
    } catch (err) {
      const e = err as BusinessError;
      hilog.warn(DOMAIN, TAG, 'notification auth: %{public}d %{public}s', e.code, e.message);
    }
    try {
      const info: wantAgent.WantAgentInfo = {
        wants: [{ bundleName: BUNDLE_NAME, abilityName: 'EntryAbility' }],
        operationType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      };
      const agent = await wantAgent.getWantAgent(info);
      await backgroundTaskManager.startBackgroundRunning(uiCtx, mode, agent);
      this.appendLog('background continuous task started');
    } catch (err) {
      const e = err as BusinessError;
      hilog.error(DOMAIN, TAG, 'startBackgroundRunning: %{public}d %{public}s', e.code, e.message);
      this.appendLog(`keep-alive failed (code ${e.code})`);
    }
  }

  async stopKeepAlive(): Promise<void> {
    try {
      await backgroundTaskManager.stopBackgroundRunning(getContext(this));
      this.appendLog('background continuous task stopped');
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'stopBackgroundRunning failed');
    }
  }

  onStartServer(): void {
    const ok: boolean = this.server.start(this.makeConfig());
    this.running = ok;
    this.refreshUrl();
    this.appendLog(ok ? `RTSP server listening on :${this.port}/${this.streamPath}`
                      : 'start failed (port busy?)');
  }

  onStopServer(): void {
    this.server.stopTestPattern();
    this.server.stopCamera();
    this.server.stopMic();
    this.server.stopRtmp();
    this.patternRunning = false;
    this.cameraRunning = false;
    this.micRunning = false;
    this.rtmpRunning = false;
    this.server.stop();
    this.running = false;
    this.clients = 0;
    this.appendLog('server stopped');
    if (this.keepAliveOn) {
      this.keepAliveOn = false;
      this.stopKeepAlive();
    }
  }

  onStartCamera(): void {
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    if (this.patternRunning) {
      this.server.stopTestPattern();
      this.patternRunning = false;
      this.appendLog('test pattern replaced by camera');
    }
    const ok: boolean = this.startCameraNow();
    this.cameraRunning = ok;
    this.appendLog(ok
      ? `${CODECS[this.codecIdx]} ${RESOLUTIONS[this.resolutionIdx]} @${BITRATES[this.bitrateIdx]} ${FACING[this.facingIdx]}摄像头`
      : `${CODECS[this.codecIdx]} camera failed to start`);
  }

  startCameraNow(): boolean {
    const parts = RESOLUTIONS[this.resolutionIdx].split('x');
    const bitrate = [1000000, 2000000, 4000000][this.bitrateIdx];
    const mime = this.codecIdx === 1 ? 'video/hevc' : 'video/avc';
    const kfMs = (parseInt(this.keyframeInterval) || 2) * 1000;
    return this.server.startCamera(parseInt(parts[0]), parseInt(parts[1]),
      bitrate, mime, this.facingIdx === 1, kfMs);
  }

  onStopCamera(): void {
    this.server.stopCamera();
    this.cameraRunning = false;
    this.appendLog('camera stopped');
  }

  async onStartMic(): Promise<void> {
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    const ok: boolean = this.server.startMic();
    this.micRunning = ok;
    this.appendLog(ok ? 'microphone streaming (48kHz mono AAC)' : 'microphone failed to start');
    if (ok && !this.keepAliveOn) {
      this.keepAliveOn = true;
      await this.startKeepAlive(backgroundTaskManager.BackgroundMode.AUDIO_RECORDING);
    }
  }

  onStopMic(): void {
    this.server.stopMic();
    this.micRunning = false;
    this.appendLog('microphone stopped');
  }

  onStartRtmp(): void {
    hilog.info(DOMAIN, TAG, 'onStartRtmp url=%{public}s', this.rtmpUrl);
    const url = this.rtmpUrl.trim();
    if (url === '') {
      this.appendLog('RTMP url is empty');
      return;
    }
    this.saveSetting('rtmpUrl', url);
    if (this.rtmpRunning) {
      this.server.stopRtmp();
    }
    const ok: boolean = this.server.startRtmp(url);
    this.rtmpRunning = ok;
    this.appendLog(ok ? `RTMP publishing to ${url}` : 'RTMP publish failed');
  }

  onStopRtmp(): void {
    this.server.stopRtmp();
    this.rtmpRunning = false;
    this.appendLog('RTMP publish stopped');
  }

  onToggleRecord(): void {
    if (this.recording) {
      this.server.stopRecord();
      this.recording = false;
      this.appendLog('recording stopped (files dir: record.mp4)');
      return;
    }
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    const ctx = getContext(this) as common.UIAbilityContext;
    const ts = this.timestamp().replace(/:/g, '');
    const ok: boolean = this.server.startRecord(`${ctx.filesDir}/ipc_${ts}.mp4`);
    this.recording = ok;
    this.appendLog(ok ? `recording started (MP4 ipc_${ts}.mp4)` : 'recording failed to start');
  }

  onSnapshot(): void {
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    const ctx = getContext(this) as common.UIAbilityContext;
    const ok: boolean = this.server.takeSnapshot(`${ctx.filesDir}/snapshot.bmp`);
    this.appendLog(ok ? 'snapshot decoding (BMP, files dir/snapshot.bmp)'
                      : 'snapshot failed to start');
  }

  onToggleHttp(): void {
    if (this.httpRunning) {
      this.server.stopHttp();
      this.httpRunning = false;
      this.appendLog('web console stopped');
      return;
    }
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    const ctx = getContext(this) as common.UIAbilityContext;
    const opts: HttpOptions = {
      port: parseInt(this.httpPort) || 8081,
      user: this.user,
      password: this.password,
      filesDir: ctx.filesDir
    };
    const ok: boolean = this.server.startHttp(opts);
    this.httpRunning = ok;
    this.appendLog(ok ? `web console on http://${this.deviceIp()}:${this.httpPort}`
                      : 'web console failed');
  }

  onToggleMotion(): void {
    this.motionEnabled = !this.motionEnabled;
    const timeout = parseInt(this.motionTimeout) || 15;
    this.server.setMotion(this.motionEnabled, timeout);
    this.appendLog(this.motionEnabled ? `motion detection ON (timeout=${timeout}s)`
                                      : 'motion detection OFF');
  }

  onStartPattern(): void {
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    if (this.cameraRunning) {
      this.server.stopCamera();
      this.cameraRunning = false;
      this.appendLog('camera source replaced by test pattern');
    }
    const ok: boolean = this.server.startTestPattern(640, 480, 15, 1500000);
    this.patternRunning = ok;
    this.appendLog(ok ? 'test pattern (640x480@15 H.264) streaming'
                      : 'test pattern failed to start');
  }

  onStopPattern(): void {
    this.server.stopTestPattern();
    this.patternRunning = false;
    this.appendLog('test pattern stopped');
  }

  // ---------- OSD (text overlay) ----------

  loadOsdSettings(): void {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
      this.osdEnabled = pref.getSync('osdEnabled', false) as boolean;
      this.osdPositionIdx = pref.getSync('text_position', 0) as number;
      this.osdFontStyleIdx = pref.getSync('font_style', 0) as number;
      this.osdPadding = `${pref.getSync('osd_padding_int', 1) as number}`;
      this.osdColor = pref.getSync('text_color', '#FFFFFFFF') as string;
      this.osdShowTimestamp = pref.getSync('display_timestamp', true) as boolean;
      this.osdShowDevName = pref.getSync('display_dev_name', true) as boolean;
      this.osdShowBattery = pref.getSync('display_battery_info', true) as boolean;
      this.osdShowGps = pref.getSync('display_gps_location', false) as boolean;
      this.osdSpeedUnitIdx = pref.getSync('speed_unit', 0) as number;
      this.osdSpeedMph = this.osdSpeedUnitIdx === 1;
      this.osdCustomText = pref.getSync('text_custom', '') as string;
      this.wmEnabled = pref.getSync('watermark_overlay', false) as boolean;
      this.wmPositionIdx = pref.getSync('watermark_position', 0) as number;
      this.wmPadding = `${pref.getSync('watermark_padding_int', 0) as number}`;
      this.wmMaxArea = `${pref.getSync('watermark_max_area_occupied_int', 10) as number}`;
      const wmSrc = pref.getSync('watermark_src', '') as string;
      if (wmSrc !== '') {
        this.decodeWatermark(wmSrc);
      }
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'loadOsdSettings failed');
    }
  }

  buildOsdConfig(): OsdConfig {
    const cfg: OsdConfig = {
      enabled: this.osdEnabled,
      padding: parseInt(this.osdPadding) || 1,
      position: this.osdPositionIdx,
      fontStyle: this.osdFontStyleIdx,
      color: this.osdColor,
      showTimestamp: this.osdShowTimestamp,
      showDevName: this.osdShowDevName,
      showBattery: this.osdShowBattery,
      showGps: this.osdShowGps,
      speedMph: this.osdSpeedMph,
      customText: this.osdCustomText,
      wmEnabled: this.wmEnabled,
      wmPadding: parseInt(this.wmPadding) || 0,
      wmMaxAreaPercent: parseInt(this.wmMaxArea) || 10,
      wmPosition: this.wmPositionIdx
    };
    return cfg;
  }

  applyOsd(needCameraRestart: boolean): void {
    this.server.setOsdConfig(this.buildOsdConfig());
    if (this.osdEnabled && this.osdShowBattery) {
      this.startBatteryTimer();
    } else {
      this.stopBatteryTimer();
    }
    if (this.osdWatermarkPm != null) {
      this.server.setOsdWatermark(this.osdWatermarkPm);
    }
    if (this.osdShowGps && this.osdEnabled) {
      this.startGpsTimer();
    } else {
      this.stopGpsTimer();
    }
    if (needCameraRestart && this.cameraRunning) {
      // enabling/disabling switches the camera between the direct encoder
      // surface and the GL interception pipeline
      this.server.stopCamera();
      this.cameraRunning = this.startCameraNow();
      this.appendLog(this.cameraRunning ? 'camera restarted with OSD pipeline'
                                        : 'camera restart failed');
    }
  }

  onToggleOsd(on: boolean): void {
    this.osdEnabled = on;
    this.saveSetting('osdEnabled', on);
    this.applyOsd(true);
    this.appendLog(on ? 'OSD overlay ON (text baked into the stream)'
                      : 'OSD overlay OFF');
  }

  pushBattery(): void {
    try {
      const level = parseInt(`${batteryInfo.batterySOC}`);
      this.server.setOsdBattery(isNaN(level) ? -1 : level, batteryInfo.chargingStatus);
    } catch (err) {
      this.server.setOsdBattery(-1, 0);
    }
  }

  startBatteryTimer(): void {
    if (this.batteryTimerId >= 0) return;
    this.pushBattery();
    this.batteryTimerId = setInterval(() => this.pushBattery(), 60000);
  }

  stopBatteryTimer(): void {
    if (this.batteryTimerId >= 0) {
      clearInterval(this.batteryTimerId);
      this.batteryTimerId = -1;
    }
  }

  async startGpsTimer(): Promise<void> {
    if (this.gpsTimerId >= 0) return;
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const atManager = abilityAccessCtrl.createAtManager();
      const perms: Array<Permissions> = ['ohos.permission.APPROXIMATELY_LOCATION',
        'ohos.permission.LOCATION'];
      const result = await atManager.requestPermissionsFromUser(ctx, perms);
      let granted = true;
      result.authResults.forEach((r: number) => {
        if (r !== 0) granted = false;
      });
      if (!granted) {
        this.appendLog('GPS permission denied (OSD shows "GPS Waiting")');
        return;
      }
    } catch (err) {
      this.appendLog('GPS permission request failed');
      return;
    }
    this.pollGps();
    this.gpsTimerId = setInterval(() => this.pollGps(), 5000);
  }

  stopGpsTimer(): void {
    if (this.gpsTimerId >= 0) {
      clearInterval(this.gpsTimerId);
      this.gpsTimerId = -1;
    }
    this.server.setOsdGps(0, 0, 0, false, this.osdSpeedMph);
  }

  async pollGps(): Promise<void> {
    try {
      const loc: geoLocationManager.Location =
        await geoLocationManager.getCurrentLocation();
      const speedKmh: number = (loc.speed ?? 0) * 3.6;
      this.server.setOsdGps(loc.latitude, loc.longitude, speedKmh, true, this.osdSpeedMph);
    } catch (err) {
      this.server.setOsdGps(0, 0, 0, false, this.osdSpeedMph);
    }
  }

  async onPickWatermark(): Promise<void> {
    try {
      const picker0 = new picker.PhotoViewPicker();
      const result = await picker0.select({
        MIMEType: picker.PhotoViewMIMETypes.IMAGE_TYPE,
        maxSelectNumber: 1
      });
      if (result.photoUris.length < 1) return;
      const ctx = getContext(this) as common.UIAbilityContext;
      const dest = `${ctx.filesDir}/osd_watermark_src`;
      const srcFile = fileIo.openSync(result.photoUris[0], fileIo.OpenMode.READ_ONLY);
      fileIo.copyFileSync(srcFile.fd, dest);
      fileIo.closeSync(srcFile);
      this.saveSetting('watermark_src', dest);
      await this.decodeWatermark(dest);
      this.appendLog('watermark loaded');
    } catch (err) {
      this.appendLog('watermark pick failed');
    }
  }

  async decodeWatermark(path: string): Promise<void> {
    try {
      const src = image.createImageSource(path);
      const info = await src.getImageInfo();
      const scale = Math.min(1, 1280 / Math.max(info.size.width, info.size.height));
      const pm = await src.createPixelMap({
        desiredPixelFormat: image.PixelMapFormat.RGBA_8888,
        desiredSize: {
          width: Math.round(info.size.width * scale),
          height: Math.round(info.size.height * scale)
        }
      });
      await src.release();
      this.osdWatermarkPm = pm;
      this.server.setOsdWatermark(pm);
    } catch (err) {
      this.appendLog('watermark decode failed');
    }
  }

  // ---------- end OSD ----------

  build() {
    Scroll() {
      Column({ space: 8 }) {
        Text('IPCamera Harmony')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .margin({ top: 12 })

        Row({ space: 10 }) {
          Text(this.running ? '● RUNNING' : '○ STOPPED')
            .fontSize(13)
            .fontColor(this.running ? '#0A7D32' : '#9E9E9E')
          Text(`clients: ${this.clients}`).fontSize(13).fontColor('#333333')
          if (this.cameraRunning) {
            Text('cam ●').fontSize(13).fontColor('#B3261E')
          }
          if (this.micRunning) {
            Text('mic ●').fontSize(13).fontColor('#B3261E')
          }
          if (this.rtmpRunning) {
            Text('rtmp ●').fontSize(13).fontColor('#B3261E')
          }
        }

        XComponent({ id: 'cameraPreview', type: 'surface', libraryname: 'streaming' })
          .width('92%')
          .height(200)
          .borderRadius(8)
          .backgroundColor('#000000')

        Text(this.httpRunning
          ? `http://${this.deviceIp()}:${this.httpPort} (admin/${this.password})`
          : '网页控制台未启动')
          .fontSize(12)
          .fontColor(this.httpRunning ? '#0A7D32' : '#9E9E9E')
          .copyOption(CopyOptions.LocalDevice)
          .padding(8)
          .borderRadius(8)
          .backgroundColor('#EFF7EF')
          .width('92%')

        Text(this.rtspUrl)
          .fontSize(12)
          .fontColor('#1A56B0')
          .copyOption(CopyOptions.LocalDevice)
          .padding(8)
          .borderRadius(8)
          .backgroundColor('#F0F3F8')
          .width('92%')

        Row({ space: 8 }) {
          Button('启动服务').onClick(() => this.onStartServer()).enabled(!this.running)
          Button('停止服务').onClick(() => this.onStopServer()).enabled(this.running)
        }

        Row({ space: 8 }) {
          Button('开启相机').onClick(() => this.onStartCamera())
            .enabled(this.running && !this.cameraRunning)
          Button('停止相机').onClick(() => this.onStopCamera()).enabled(this.cameraRunning)
        }

        Row({ space: 8 }) {
          Button('开启麦克风').onClick(async () => this.onStartMic())
            .enabled(this.running && !this.micRunning)
          Button('停止麦克风').onClick(() => this.onStopMic()).enabled(this.micRunning)
        }

        Row({ space: 8 }) {
          TextInput({ text: this.rtmpUrl, placeholder: 'rtmp://host:1935/app/streamkey' })
            .fontSize(12)
            .width('58%')
            .onChange((value: string) => {
              this.rtmpUrl = value;
            })
          if (!this.rtmpRunning) {
            Button('开启推流').onClick(() => this.onStartRtmp())
          } else {
            Button('停止推流').onClick(() => this.onStopRtmp())
          }
        }

        Row({ space: 8 }) {
          Button(this.recording ? '停止录像' : '开启录像')
            .onClick(() => this.onToggleRecord())
            .enabled(this.running || this.recording)
          Button('抓拍').onClick(() => this.onSnapshot()).enabled(this.running)
        }

        Row({ space: 8 }) {
          Button(this.httpRunning ? '停止网页' : '启动网页')
            .onClick(() => this.onToggleHttp())
            .enabled(this.running || this.httpRunning)
          Button(this.motionEnabled ? '移动侦测:开' : '移动侦测:关')
            .onClick(() => this.onToggleMotion())
            .enabled(this.running || this.motionEnabled)
        }

        Row({ space: 8 }) {
          Button('开启测试流').onClick(() => this.onStartPattern())
            .enabled(this.running && !this.patternRunning)
          Button('停止测试流').onClick(() => this.onStopPattern()).enabled(this.patternRunning)
        }

        Text('设置(重启相机/服务后生效)').fontSize(13).fontWeight(FontWeight.Medium)
          .margin({ top: 4 })

        Row({ space: 8 }) {
          Text('分辨率').fontSize(12).width(52)
          Select(toSelectOptions(RESOLUTIONS))
            .selected(this.resolutionIdx)
            .value(RESOLUTIONS[this.resolutionIdx])
            .onSelect((idx: number) => {
              this.resolutionIdx = idx;
              this.saveSetting('resolution', idx);
            })
          Text('码率').fontSize(12).width(40)
          Select(toSelectOptions(BITRATES))
            .selected(this.bitrateIdx)
            .value(BITRATES[this.bitrateIdx])
            .onSelect((idx: number) => {
              this.bitrateIdx = idx;
              this.saveSetting('bitrate', idx);
            })
        }

        Row({ space: 8 }) {
          Text('编码').fontSize(12).width(52)
          Select(toSelectOptions(CODECS))
            .selected(this.codecIdx)
            .value(CODECS[this.codecIdx])
            .onSelect((idx: number) => {
              this.codecIdx = idx;
              this.saveSetting('codec', idx);
            })
          Text('摄像头').fontSize(12).width(48)
          Select(toSelectOptions(FACING))
            .selected(this.facingIdx)
            .value(FACING[this.facingIdx])
            .onSelect((idx: number) => {
              this.facingIdx = idx;
              this.saveSetting('facing', idx);
            })
        }

        Row({ space: 8 }) {
          Text('端口').fontSize(12).width(52)
          TextInput({ text: this.port }).fontSize(12).width(70).type(InputType.Number)
            .onChange((v: string) => {
              this.port = v;
              this.saveSetting('port', parseInt(v) || 8554);
            })
          Text('网页').fontSize(12).width(40)
          TextInput({ text: this.httpPort }).fontSize(12).width(70).type(InputType.Number)
            .onChange((v: string) => {
              this.httpPort = v;
              this.saveSetting('httpPort', parseInt(v) || 8081);
            })
          Text('路径').fontSize(12).width(40)
          TextInput({ text: this.streamPath }).fontSize(12).width(90)
            .onChange((v: string) => {
              this.streamPath = v;
              this.saveSetting('path', v);
            })
        }

        Row({ space: 8 }) {
          Text('账号').fontSize(12).width(52)
          TextInput({ text: this.user }).fontSize(12).width(70)
            .onChange((v: string) => {
              this.user = v;
              this.saveSetting('user', v);
            })
          Text('密码').fontSize(12).width(40)
          TextInput({ text: this.password }).fontSize(12).width(90).type(InputType.Password)
            .onChange((v: string) => {
              this.password = v;
              this.saveSetting('password', v);
            })
        }

        Row({ space: 8 }) {
          Text('移动超时(s)').fontSize(12).width(76)
          TextInput({ text: this.motionTimeout }).fontSize(12).width(60).type(InputType.Number)
            .onChange((v: string) => {
              this.motionTimeout = v;
              this.saveSetting('motionTimeout', parseInt(v) || 15);
            })
          Text('I 帧间隔(s)').fontSize(12).width(72)
          TextInput({ text: this.keyframeInterval }).fontSize(12).width(50).type(InputType.Number)
            .onChange((v: string) => {
              this.keyframeInterval = v;
              this.saveSetting('keyframeInterval', parseInt(v) || 2);
            })
        }

        Text('文字叠加 OSD(开关切换会重启相机)')
          .fontSize(13).fontWeight(FontWeight.Medium).margin({ top: 8 })

        Row({ space: 8 }) {
          Text('叠加').fontSize(12).width(52)
          Toggle({ type: ToggleType.Switch, isOn: this.osdEnabled })
            .onChange((on: boolean) => this.onToggleOsd(on))
          Text('位置').fontSize(12).width(40)
          Select(toSelectOptions(OSD_POSITIONS))
            .selected(this.osdPositionIdx)
            .value(OSD_POSITIONS[this.osdPositionIdx])
            .onSelect((idx: number) => {
              this.osdPositionIdx = idx;
              this.saveSetting('text_position', idx);
              this.applyOsd(false);
            })
          Text('边距').fontSize(12).width(40)
          TextInput({ text: this.osdPadding }).fontSize(12).width(50).type(InputType.Number)
            .onChange((v: string) => {
              this.osdPadding = v;
              this.saveSetting('osd_padding_int', parseInt(v) || 0);
              this.applyOsd(false);
            })
        }

        Row({ space: 8 }) {
          Text('字体').fontSize(12).width(52)
          Select(toSelectOptions(OSD_FONT_STYLES))
            .selected(this.osdFontStyleIdx)
            .value(OSD_FONT_STYLES[this.osdFontStyleIdx])
            .onSelect((idx: number) => {
              this.osdFontStyleIdx = idx;
              this.saveSetting('font_style', idx);
              this.applyOsd(false);
            })
          Text('颜色').fontSize(12).width(40)
          TextInput({ text: this.osdColor, placeholder: '#RRGGBBAA' })
            .fontSize(12).width(96)
            .onChange((v: string) => {
              this.osdColor = v;
              this.saveSetting('text_color', v);
              this.applyOsd(false);
            })
        }

        Row({ space: 6 }) {
          Text('时间戳').fontSize(11)
          Toggle({ type: ToggleType.Checkbox, isOn: this.osdShowTimestamp })
            .onChange((on: boolean) => {
              this.osdShowTimestamp = on;
              this.saveSetting('display_timestamp', on);
              this.applyOsd(false);
            })
          Text('设备名').fontSize(11)
          Toggle({ type: ToggleType.Checkbox, isOn: this.osdShowDevName })
            .onChange((on: boolean) => {
              this.osdShowDevName = on;
              this.saveSetting('display_dev_name', on);
              this.applyOsd(false);
            })
          Text('电量').fontSize(11)
          Toggle({ type: ToggleType.Checkbox, isOn: this.osdShowBattery })
            .onChange((on: boolean) => {
              this.osdShowBattery = on;
              this.saveSetting('display_battery_info', on);
              this.applyOsd(false);
            })
          Text('GPS').fontSize(11)
          Toggle({ type: ToggleType.Checkbox, isOn: this.osdShowGps })
            .onChange((on: boolean) => {
              this.osdShowGps = on;
              this.saveSetting('display_gps_location', on);
              this.applyOsd(false);
            })
        }

        Row({ space: 8 }) {
          Text('自定义文字').fontSize(12).width(76)
          TextInput({ text: this.osdCustomText, placeholder: '叠加在画面上的文字' })
            .fontSize(12).width(180)
            .onChange((v: string) => {
              this.osdCustomText = v;
              this.saveSetting('text_custom', v);
              this.applyOsd(false);
            })
        }

        Row({ space: 8 }) {
          Text('水印').fontSize(12).width(52)
          Toggle({ type: ToggleType.Switch, isOn: this.wmEnabled })
            .onChange((on: boolean) => {
              this.wmEnabled = on;
              this.saveSetting('watermark_overlay', on);
              this.applyOsd(false);
            })
          Text('位置').fontSize(12).width(40)
          Select(toSelectOptions(OSD_POSITIONS))
            .selected(this.wmPositionIdx)
            .value(OSD_POSITIONS[this.wmPositionIdx])
            .onSelect((idx: number) => {
              this.wmPositionIdx = idx;
              this.saveSetting('watermark_position', idx);
              this.applyOsd(false);
            })
          Text('占比%').fontSize(12).width(44)
          TextInput({ text: this.wmMaxArea }).fontSize(12).width(46).type(InputType.Number)
            .onChange((v: string) => {
              this.wmMaxArea = v;
              this.saveSetting('watermark_max_area_occupied_int', parseInt(v) || 10);
              this.applyOsd(false);
            })
          Button('选图').onClick(() => this.onPickWatermark())
        }

        Text('Event log').fontSize(13).fontWeight(FontWeight.Medium)

        List({ space: 3 }) {
          ForEach(this.logs, (item: string) => {
            ListItem() {
              Text(item)
                .fontSize(11)
                .fontFamily('monospace')
                .fontColor('#333333')
                .padding({ left: 12, right: 12, top: 1, bottom: 1 })
            }
          }, (item: string) => item)
        }
        .height(180)
        .width('92%')
        .borderRadius(8)
        .backgroundColor('#FAFAFA')
        .scrollBar(BarState.Auto)

        Text('RTSP: ffplay rtsp://user:pass@ip:port/path | RTMP 推流需 H.264 模式')
          .fontSize(10)
          .fontColor('#757575')
          .margin({ bottom: 10 })
      }
      .width('100%')
      .padding({ bottom: 20 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
