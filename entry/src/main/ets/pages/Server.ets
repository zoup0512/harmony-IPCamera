import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import { connection } from '@kit.NetworkKit';
import { deviceInfo, batteryInfo } from '@kit.BasicServicesKit';
import { geoLocationManager } from '@kit.LocationKit';
import { AppServer } from '../model/AppServer';
import { Prefs, RtmpAddr } from '../model/Prefs';
import { generateBarcode, scanCore } from '@kit.ScanKit';
import { image } from '@kit.ImageKit';
import { fileIo } from '@kit.CoreFileKit';
import { RecordOptions, RtspServer } from '../model/RtspServer';

interface StatsJson {
  rtsp: number; mjpeg: number; opus: number;
  camera: boolean; mic: boolean; motion: boolean; torch: boolean;
}

@Entry
@Component
struct Server {
  private ctx = getContext(this) as common.UIAbilityContext;
  private timer: number = -1;
  private server: RtspServer = AppServer.server();
  private pageActive: boolean = false;
  private running: boolean = false;
  private cameraRunning: boolean = false;
  private recording: boolean = false;
  private motionEnabled: boolean = false;
  private motionRecording: boolean = false;
  private motionTimerId: number = -1;
  private motionGeneration: number = 0;
  private motionRecordPath: string = '';
  private motionTimeout: string = '15';
  private segMinutes: string = '10';
  private segFourGb: boolean = true;
  private logs: string[] = [];

  @State ip: string = '0.0.0.0';
  @State status: string = '';
  @State previewOn: boolean = true;
  @State showSettings: boolean = false;
  @State qrPixel: image.PixelMap | null = null;
  @State rtmpList: RtmpAddr[] = [];

  @State rtspPort: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'video_port', 8554);
  @State httpPort: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'http_port', 8081);
  @State userName: string = Prefs.getStr(getContext(this) as common.UIAbilityContext, 'login_name', 'admin');
  @State userPass: string = Prefs.getStr(getContext(this) as common.UIAbilityContext, 'login_pass', 'admin');
  @State streamPath: string = Prefs.getStr(getContext(this) as common.UIAbilityContext, 'ip_camera_path', 'live');
  @State vw: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'video_width', 1280);
  @State vh: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'video_height', 720);
  @State mime: string = Prefs.getStr(getContext(this) as common.UIAbilityContext, 'video_codec', 'video/avc');
  @State bitrate: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'video_bitrate', 2000); // kbps
  @State hevcBitrate: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'hevc_bitrate', 2000);
  @State iFrame: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'iframe_interval', 2);
  @State fps: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'video_frame_rate', 30);
  @State profileIdx: number = Prefs.getInt(getContext(this) as common.UIAbilityContext, 'hw_encoder_profile', 0);
  @State stereo: boolean = Prefs.getBool(getContext(this) as common.UIAbilityContext, 'audio_source_stereo', false);
  @State front: boolean = Prefs.getBool(getContext(this) as common.UIAbilityContext, 'use_front_camera', false);
  @State osdOn: boolean = Prefs.getBool(getContext(this) as common.UIAbilityContext, 'video_text_overlay', false);
  @State pageTitle: string = Prefs.getStr(getContext(this) as common.UIAbilityContext, 'web_page_title', 'IPCamera');
  @State autoRtsp: boolean = Prefs.getBool(getContext(this) as common.UIAbilityContext, 'auto_rtsp_after_turn_on', false);
  @State autoRtmp: boolean = Prefs.getBool(getContext(this) as common.UIAbilityContext, 'auto_rtmp_after_turn_on', false);

  aboutToAppear(): void {
    this.pageActive = true;
    this.running = AppServer.cameraRunning;
    this.cameraRunning = AppServer.cameraRunning;
    this.recording = AppServer.recording;
    this.motionEnabled = AppServer.motionEnabled;
    this.motionTimeout = `${Prefs.getInt(this.ctx, 'motion_timeout', 15)}`;
    const sv = AppServer.server();
    sv.setStatusCallback((event: number, clients: number, detail: string) => {
      if (detail === 'MOTION detected' || detail === 'MOTION cleared') {
        // Status delivery is lossy; only the native-state timer controls recording.
        this.appendLog(detail);
      } else {
        this.appendLog(`${event}:${clients} ${detail}`);
      }
    });
    this.rtmpList = Prefs.rtmpList(this.ctx);
    this.refresh();
    this.timer = setInterval(() => this.refresh(), 2000);
    if (!AppServer.cameraRunning) this.startAll();
    if (this.motionEnabled) this.startMotionTimer();
  }

  onPageShow(): void {
    this.pageActive = true;
  }

  aboutToDisappear(): void {
    this.pageActive = false;
    this.stopMotionMode('motion detection OFF (page closed)');
    if (this.timer >= 0) clearInterval(this.timer);
  }

  appendLog(message: string): void {
    this.logs.push(message);
    if (this.logs.length > 50) this.logs.shift();
    this.status = message;
  }

  saveSetting(key: string, value: number): void {
    this.put(key === 'motionTimeout' ? 'motion_timeout' : key, value);
  }

  refresh(): void {
    try {
      const s = JSON.parse(AppServer.server().statsJson()) as StatsJson;
      this.status = `rtsp:${s.rtsp} mjpeg:${s.mjpeg} opus:${s.opus}` +
        (s.motion ? '  ' + 'MOTION' : '');
      if (this.osdOn && AppServer.cameraRunning) {
        AppServer.server().setOsdBattery(batteryInfo.batterySOC,
          batteryInfo.chargingStatus as number);
      }
    } catch { /* keep last */ }
  }

  // Android: warns once when the default route is cellular before streaming.
  checkMobileData(): void {
    try {
      const h = connection.getDefaultNetSync();
      const caps = connection.getNetCapabilitiesSync(h);
      if (caps.bearerTypes.indexOf(connection.NetBearType.BEARER_CELLULAR) >= 0) {
        promptAction.showToast({ message: $r('app.string.mobile_data_prompt'), duration: 4000 });
      }
    } catch { /* ignore */ }
  }

  stopAll(): void {
    const sv = AppServer.server();
    if (AppServer.recording) sv.stopRecord();
    if (AppServer.rtmpOn) sv.stopRtmp();
    if (AppServer.rtspOn) sv.stop();
    sv.stopMic(); sv.stopCamera(); sv.stopHttp();
    AppServer.recording = false; AppServer.rtmpOn = false; AppServer.rtspOn = false;
    AppServer.micRunning = false; AppServer.cameraRunning = false;
    this.recording = false; this.cameraRunning = false; this.running = false;
  }

  onStopServer(): void {
    this.stopMotionMode('motion detection OFF (server stopped)');
    this.stopAll();
  }

  onStopCamera(): void {
    this.stopMotionMode('motion detection OFF (camera stopped)');
    this.server.stopCamera();
    this.cameraRunning = false;
    AppServer.cameraRunning = false;
    this.appendLog('camera stopped');
  }

  startCameraNow(): boolean {
    if (!this.cameraRunning) this.startAll();
    return AppServer.cameraRunning;
  }

  onStartServer(): void {
    if (!AppServer.cameraRunning) this.startAll();
    this.running = AppServer.cameraRunning;
    this.cameraRunning = AppServer.cameraRunning;
  }

  onToggleMotion(): void {
    if (this.motionEnabled) {
      this.stopMotionMode('motion detection OFF');
      return;
    }
    // Motion detection and auto recording read the decoded stream; the
    // camera/encoder must be up first (Android brings the server up first).
    if (!this.running) this.onStartServer();
    if (!this.running) {
      this.appendLog('motion detection: stream failed to start');
      return;
    }
    if (!this.cameraRunning) {
      this.cameraRunning = this.startCameraNow();
      this.appendLog(this.cameraRunning ? 'camera started for motion detection'
                                        : 'motion detection: camera failed to start');
    }
    if (!this.cameraRunning) return;
    if (this.recording) {
      this.server.stopRecord();
      this.recording = false;
      AppServer.recording = false;
      this.appendLog('manual recording stopped (motion detection takes over)');
    }
    // Android clamps motion_timeout to 10-120 s.
    const timeout = Math.min(120, Math.max(10, parseInt(this.motionTimeout) || 15));
    this.motionTimeout = `${timeout}`;
    this.saveSetting('motionTimeout', timeout);
    this.motionEnabled = true;
    AppServer.motionEnabled = true;
    this.server.setMotion(true, timeout);
    this.startMotionTimer();
    this.appendLog(`motion detection ON (timeout=${timeout}s, auto record MD/)`);
  }

  // Android timestamp shape for motion records: yyyy-MM-dd.HH.mm.ss.SSSS
  motionTimestamp(): string {
    const d: Date = new Date();
    const p = (n: number, w: number): string => {
      let s = `${n}`;
      while (s.length < w) s = `0${s}`;
      return s;
    };
    return `${p(d.getFullYear(), 4)}-${p(d.getMonth() + 1, 2)}-${p(d.getDate(), 2)}.` +
           `${p(d.getHours(), 2)}.${p(d.getMinutes(), 2)}.${p(d.getSeconds(), 2)}.` +
           `${p(d.getMilliseconds(), 4)}`;
  }

  startMotionTimer(): void {
    this.stopMotionTimer();
    const generation = this.motionGeneration;
    this.motionTimerId = setInterval(() => {
      if (generation !== this.motionGeneration || !this.motionEnabled || !this.running) return;
      if (this.server.motionActive()) {
        this.onMotionStart();
      } else {
        this.onMotionStop();
      }
    }, 250);
  }

  stopMotionTimer(): void {
    this.motionGeneration++;
    if (this.motionTimerId >= 0) {
      clearInterval(this.motionTimerId);
      this.motionTimerId = -1;
    }
  }

  onMotionStart(): void {
    if (!this.motionEnabled || !this.running || !this.pageActive ||
        !this.server.motionActive() || this.motionRecording) return;
    if (this.recording) {
      this.appendLog('motion: manual recording is running, auto record skipped');
      return;
    }
    const ctx = getContext(this) as common.UIAbilityContext;
    const dir = `${ctx.filesDir}/MD`;
    try {
      if (!fileIo.accessSync(dir)) fileIo.mkdirSync(dir, true);
    } catch (err) {
      this.appendLog('motion: cannot create the MD folder');
      return;
    }
    const stamp = this.motionTimestamp();
    const path = `${dir}/IPS_${stamp}.mp4`;
    const recordOptions: RecordOptions = {
      fourGbLimit: this.segFourGb,
      segmentMinutes: parseInt(this.segMinutes) || 0
    };
    const ok: boolean = this.server.startRecord(path, recordOptions);
    this.motionRecording = ok;
    this.motionRecordPath = ok ? path : '';
    this.appendLog(ok ? `motion record started (MD/IPS_${stamp}.mp4)`
                      : 'motion record failed to start');
  }

  onMotionStop(): void {
    if (!this.motionRecording) return;
    const name = this.motionRecordPath.substring(this.motionRecordPath.lastIndexOf('/') + 1);
    this.server.stopRecord();
    this.motionRecording = false;
    this.motionRecordPath = '';
    this.appendLog(`motion record saved (MD/${name})`);
  }

  // Leaving motion mode (server stop, page teardown, camera stop) closes the
  // auto recording instead of leaving the file open.
  stopMotionMode(reason: string): void {
    const wasEnabled = this.motionEnabled;
    this.motionEnabled = false;
    AppServer.motionEnabled = false;
    this.stopMotionTimer();
    try {
      this.onMotionStop();
    } finally {
      this.server.setMotion(false);
    }
    if (wasEnabled) this.appendLog(reason);
  }

  // Android asks before shutting the server down on back.
  onBackPress(): boolean {
    promptAction.showDialog({
      title: $r('app.string.confirm_exit'),
      buttons: [
        { text: $r('app.string.ok'), color: '#E53935' },
        { text: $r('app.string.cancel'), color: '#007DFF' }
      ]
    }).then((r) => {
      if (r.index === 0) {
        this.stopAll();
        router.back();
      }
    });
    return true;
  }

  put(key: string, v: string | number | boolean): void { Prefs.put(this.ctx, key, v); }

  startAll(): void {
    const sv = AppServer.server();
    AppServer.frontCamera = this.front;
    AppServer.streamWidth = this.vw;
    AppServer.streamHeight = this.vh;
    AppServer.mime = this.mime;
    const kbps = this.mime === 'video/hevc' ? this.hevcBitrate : this.bitrate;
    const ok = sv.startCamera(this.vw, this.vh, kbps * 1000, this.mime,
      this.front, this.iFrame * 1000, this.fps, this.profileIdx);
    if (!ok) {
      promptAction.showToast({ message: $r('app.string.server_failed') });
      return;
    }
    AppServer.cameraRunning = true;
    this.running = true;
    this.cameraRunning = true;
    sv.startMic(this.stereo);
    AppServer.micRunning = true;
    sv.startHttp({ port: this.httpPort, user: this.userName, password: this.userPass,
      filesDir: this.ctx.filesDir, pageTitle: this.pageTitle });
    this.checkMobileData();
    try {
      sv.setOsdDeviceName(deviceInfo.marketName ?? 'HarmonyOS');
      sv.setOsdBattery(batteryInfo.batterySOC, batteryInfo.chargingStatus as number);
      if (Prefs.getBool(this.ctx, 'display_gps_location', false)) {
        geoLocationManager.getCurrentLocation().then((loc) => {
          sv.setOsdGps(loc.latitude, loc.longitude, loc.speed * 3.6, true,
            Prefs.getBool(this.ctx, 'speed_unit', false));
        }).catch(() => {});
      }
    } catch { /* osd feeds are best effort */ }
    if (this.autoRtsp) this.toggleRtsp(true);
    if (this.autoRtmp) this.startRtmp();
  }

  toggleRtsp(on?: boolean): void {
    const sv = AppServer.server();
    const want = on ?? !AppServer.rtspOn;
    if (want === AppServer.rtspOn) return;
    if (want) {
      const ok = sv.start({ port: this.rtspPort, path: this.streamPath,
        user: this.userName, password: this.userPass });
      AppServer.rtspOn = ok;
      if (!ok) promptAction.showToast({ message: 'rtsp start failed' });
    } else {
      sv.stop();
      AppServer.rtspOn = false;
    }
  }

  startRtmp(): void {
    const idx = Prefs.getInt(this.ctx, 'rtmp_current', -1);
    if (idx < 0 || idx >= this.rtmpList.length) {
      promptAction.showToast({ message: 'RTMP: pick an address first' });
      return;
    }
    const e = this.rtmpList[idx];
    const url = Prefs.joinUrl(e.rtmp_server, e.rtmp_stream_key);
    AppServer.rtmpOn = true;
    AppServer.server().startRtmp(url).then((ok) => {
      AppServer.rtmpOn = ok;
      if (ok) Prefs.rememberRtmp(this.ctx, e.rtmp_server, e.rtmp_stream_key, e.title);
    }).catch(() => { AppServer.rtmpOn = false; });
  }

  async shareQr(url: string): Promise<void> {
    try {
      this.qrPixel = await generateBarcode.createBarcode(url,
        { scanType: scanCore.ScanType.QR_CODE, width: 480, height: 480 });
    } catch {
      promptAction.showToast({ message: $r('app.string.error') });
    }
  }

  @Builder Toggle(label: ResourceStr, on: boolean, cb: (v: boolean) => void) {
    Row() {
      Text(label).fontSize(13).layoutWeight(1)
      Toggle({ type: ToggleType.Switch, isOn: on }).onChange((v: boolean) => cb(v))
    }.width('100%').padding({ top: 4, bottom: 4 })
  }

  @Builder NumRow(label: ResourceStr, val: number, cb: (v: number) => void) {
    Row({ space: 8 }) {
      Text(label).fontSize(13).width(96)
      TextInput({ text: val.toString() }).type(InputType.Number).fontSize(13)
        .layoutWeight(1).onChange((v: string) => cb(parseInt(v) || 0))
    }.width('100%').padding({ top: 2, bottom: 2 })
  }

  @Builder StrRow(label: ResourceStr, val: string, cb: (v: string) => void) {
    Row({ space: 8 }) {
      Text(label).fontSize(13).width(96)
      TextInput({ text: val }).fontSize(13).layoutWeight(1)
        .onChange((v: string) => cb(v))
    }.width('100%').padding({ top: 2, bottom: 2 })
  }

  build() {
    Column() {
      // preview area (Android: video fills the top; tap toggles settings)
      Stack() {
        XComponent({ id: 'camview', type: XComponentType.SURFACE, libraryname: 'streaming' })
          .backgroundColor(Color.Black)
        Column({ space: 2 }) {
          Text(`rtsp://${this.ip}:${this.rtspPort}/${this.streamPath}`).fontSize(11).fontColor('#4FC3F7')
          Text(`http://${this.ip}:${this.httpPort}/video`).fontSize(11).fontColor('#4FC3F7')
          Text(`http://${this.ip}:${this.httpPort}/audio.opus`).fontSize(11).fontColor('#4FC3F7')
          Text(this.status).fontSize(11).fontColor('#FFFFFF')
        }.alignItems(HorizontalAlign.Start).padding(8)
      }
      .width('100%').height(this.previewOn ? '42%' : 36)
      .onClick(() => { this.showSettings = !this.showSettings; })

      // toolbar
      Row({ space: 4 }) {
        Button($r('app.string.settings')).fontSize(11).height(30)
          .onClick(() => { this.showSettings = !this.showSettings; })
        Button($r('app.string.rtsp_server')).fontSize(11).height(30)
          .backgroundColor(AppServer.rtspOn ? '#2E7D32' : '#616161')
          .onClick(() => this.toggleRtsp())
        Button($r('app.string.rtmp_publish')).fontSize(11).height(30)
          .backgroundColor(AppServer.rtmpOn ? '#2E7D32' : '#616161')
          .onClick(() => {
            if (AppServer.rtmpOn) { AppServer.server().stopRtmp(); AppServer.rtmpOn = false; }
            else this.startRtmp();
          })
        Button($r('app.string.record')).fontSize(11).height(30)
          .backgroundColor(AppServer.recording ? '#B3261E' : '#616161')
          .onClick(() => {
            const sv = AppServer.server();
            if (this.recording) {
              sv.stopRecord(); AppServer.recording = false; this.recording = false;
            } else {
              const f = `${this.ctx.filesDir}/ipc_${Date.now()}.mp4`;
              const ok = sv.startRecord(f);
              AppServer.recording = ok; this.recording = ok;
            }
          })
        Button($r('app.string.flashlight')).fontSize(11).height(30)
          .backgroundColor(AppServer.torchOn ? '#F9A825' : '#616161')
          .onClick(() => {
            AppServer.server().cameraCommand('light');
            AppServer.torchOn = !AppServer.torchOn;
          })
        Button($r('app.string.camera_switch')).fontSize(11).height(30)
          .onClick(() => {
            AppServer.server().cameraCommand('camswitch');
            AppServer.frontCamera = !AppServer.frontCamera;
          })
      }.width('100%').padding(4).justifyContent(FlexAlign.SpaceBetween)

      if (this.qrPixel !== null) {
        Image(this.qrPixel).width(160).height(160).margin(6)
          .onClick(() => { this.qrPixel = null; })
      }

      if (this.showSettings) {
        Scroll() {
          Column({ space: 4 }) {
            this.Toggle($r('app.string.auto_rtsp'), this.autoRtsp, (v) => { this.autoRtsp = v; this.put('auto_rtsp_after_turn_on', v); })
            this.Toggle($r('app.string.auto_rtmp'), this.autoRtmp, (v) => { this.autoRtmp = v; this.put('auto_rtmp_after_turn_on', v); })
            this.Toggle($r('app.string.motion_detect'), this.motionEnabled, (v) => {
              this.onToggleMotion();
            })
            this.Toggle($r('app.string.text_overlay'), this.osdOn, (v) => { this.osdOn = v; this.put('video_text_overlay', v); })
            Row({ space: 8 }) {
              Button($r('app.string.overlay_settings')).fontSize(12).height(30)
                .onClick(() => router.pushUrl({ url: 'pages/TextOverlaySettings' }))
              Button($r('app.string.encoder_settings')).fontSize(12).height(30)
                .onClick(() => router.pushUrl({ url: 'pages/EncoderSettings' }))
              Button($r('app.string.manage_rtmp')).fontSize(12).height(30)
                .onClick(() => router.pushUrl({ url: 'pages/RtmpAddrs' }))
            }
            this.Toggle($r('app.string.front_camera'), this.front, (v) => { this.front = v; this.put('use_front_camera', v); })
            this.Toggle($r('app.string.audio_stereo'), this.stereo, (v) => { this.stereo = v; this.put('audio_source_stereo', v); })
            this.NumRow($r('app.string.video_port'), this.rtspPort, (v) => { this.rtspPort = v; this.put('video_port', v); })
            this.NumRow($r('app.string.http_port'), this.httpPort, (v) => { this.httpPort = v; this.put('http_port', v); })
            this.NumRow($r('app.string.video_bitrate'), this.bitrate, (v) => { this.bitrate = v; this.put('video_bitrate', v); })
            this.NumRow($r('app.string.hevc_bitrate'), this.hevcBitrate, (v) => { this.hevcBitrate = v; this.put('hevc_bitrate', v); })
            this.NumRow($r('app.string.iframe_interval'), this.iFrame, (v) => { this.iFrame = v; this.put('iframe_interval', v); })
            this.NumRow($r('app.string.frame_rate'), this.fps, (v) => { this.fps = v; this.put('video_frame_rate', v); })
            this.StrRow($r('app.string.username'), this.userName, (v) => { this.userName = v; this.put('login_name', v); })
            this.StrRow($r('app.string.password'), this.userPass, (v) => { this.userPass = v; this.put('login_pass', v); })
            this.StrRow($r('app.string.ip_camera_path'), this.streamPath, (v) => { this.streamPath = v; this.put('ip_camera_path', v); })
            this.StrRow($r('app.string.page_title'), this.pageTitle, (v) => { this.pageTitle = v; this.put('web_page_title', v); })
            Row({ space: 8 }) {
              Text($r('app.string.video_codec')).fontSize(13).width(96)
              Button(this.mime === 'video/hevc' ? 'HEVC' : 'H.264').fontSize(12).height(30)
                .onClick(() => {
                  this.mime = this.mime === 'video/hevc' ? 'video/avc' : 'video/hevc';
                  this.put('video_codec', this.mime);
                })
              Button($r('app.string.share_qrcode')).fontSize(12).height(30)
                .onClick(() => this.shareQr(`http://${this.ip}:${this.httpPort}/`))
            }.padding({ top: 4 })
            Text('Restart required for port/resolution changes').fontSize(10).fontColor('#9E9E9E')
          }
          .width('92%').alignItems(HorizontalAlign.Start)
        }
        .layoutWeight(1).width('100%')
      }
    }
    .width('100%').height('100%').backgroundColor('#000000')
  }
}
