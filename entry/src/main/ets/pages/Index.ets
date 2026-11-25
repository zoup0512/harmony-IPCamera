import { wifiManager } from '@kit.ConnectivityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { abilityAccessCtrl, autoStartupManager, common, Permissions, wantAgent } from '@kit.AbilityKit';
import { BusinessError, batteryInfo, pasteboard } from '@kit.BasicServicesKit';
import { geoLocationManager } from '@kit.LocationKit';
import { notificationManager } from '@kit.NotificationKit';
import { preferences } from '@kit.ArkData';
import { image } from '@kit.ImageKit';
import { fileIo, picker } from '@kit.CoreFileKit';
import { router, window } from '@kit.ArkUI';
import { ViewerCounts } from 'libstreaming.so';
import { HttpOptions, OsdConfig, RtspEvent, RtspServer, RtspStartConfig } from '../model/RtspServer';

import { CameraResolution, CaptureFpsRange, CaptureFpsInfo, parseCameraResolutions,
  selectCameraResolution, parseCaptureFps, CameraDeviceOption, EncoderOption, FocusInfo, CodecTuning,
  parseCameraDevices, parseEncoderOptions, parseFocusInfo, codecTuningKey, parseCodecTuning, resolveCameraBitrate } from '../model/CameraCapabilities';
import { DualCameraPlan, queryDualCamera } from '../util/DualCamera';
import { leaseBindings, leaseCredentials } from '../model/IgdLease';
import { lanServiceAnnouncements } from '../model/LanServiceAnnouncements';
import { defaultOsdFont, OsdFontOptions, osdChargeSource, osdRestartRequired } from '../util/OsdSettings';
import { RecordingOwnership, RecordingControls, RecordingSettings } from '../util/RecordingOptions';
import { RecordingDownloads } from '../util/RecordingDownloads';
import { deliveryMotionSnapshotPath, onDeliveryCompleted } from '../util/DeliveryService';
import { ANDROID_BAR, ANDROID_DIVIDER, ANDROID_TOOLBAR_HEIGHT } from '../components/AndroidTheme';
import { OsdFontSettings } from '../components/OsdFontSettings';
import { MicProfilePicker } from '../components/MicProfilePicker';
import { CameraProfilePicker } from '../components/CameraProfilePicker';

import { MotionRecordingGate } from '../util/MotionRecordingGate';
import { canPublish } from '../util/RtmpProfiles';
import { RtmpPhase, RtmpRetryController } from '../util/RtmpRetryController';

const INK = '#FFFFFF';
const MUTED = '#B3FFFFFF';
const ACCENT = '#EF9513';
const PAPER = '#303030';
const TAG: string = 'IPCameraIndex';
const DOMAIN: number = 0xC010;
const BUNDLE_NAME: string = 'com.shenyaocn.harmony.ipcamera';

const LEGACY_RESOLUTIONS: string[] = ['640x480', '1280x720', '1920x1080'];
const BITRATES: string[] = ['1 Mbps', '2 Mbps', '4 Mbps'];
const CODECS: string[] = ['H.264', 'H.265'];
const FACING: ResourceStr[] = [$r('app.string.srv_rear'), $r('app.string.srv_front')];
const OSD_POSITIONS: ResourceStr[] = [$r('app.string.srv_tl'), $r('app.string.srv_tr'), $r('app.string.srv_bl'), $r('app.string.srv_br')];
const OSD_FONT_STYLES: ResourceStr[] = [$r('app.string.srv_normal'), $r('app.string.srv_bold'), $r('app.string.srv_italic'), $r('app.string.srv_bold_italic')];

// Continuous tasks belong to the ability, not a particular instance of this page.
// A new page must not start its task until the previous page has finished stopping it.
let audioTaskQueue: Promise<void> = Promise.resolve();
let screenTaskQueue: Promise<void> = Promise.resolve();

// A concrete adapter avoids method-bearing object literals in ArkTS component initializers.
class PageRecordingControls implements RecordingControls {
  micRunning: () => boolean;
  micGeneration: () => number;
  startMic: () => boolean;
  stopMic: () => void;
  startRecord: (path: string, opts: RecordingSettings) => boolean;
  stopRecord: () => void;
  constructor(mic: () => boolean, generation: () => number, startMic: () => boolean, stopMic: () => void,
    start: (path: string, opts: RecordingSettings) => boolean, stop: () => void) {
    this.micRunning = mic; this.micGeneration = generation; this.startMic = startMic; this.stopMic = stopMic;
    this.startRecord = start; this.stopRecord = stop;
  }
}

type ServicePanel = 'preview' | 'settings' | 'capture' | 'addresses' | 'share' | 'publish' | 'diagnostics';
type PreferenceGroup = 'root' | 'camera' | 'encoder' | 'audio' | 'overlay' | 'record' | 'storage' | 'network' | 'startup';

const NO_VIEWERS: ViewerCounts = { rtsp: 0, mjpeg: 0, flv: 0, opus: 0 };

interface ServicePageParams {
  section?: string;
  coldAutoStart?: boolean;
}

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

function toSelectOptions(arr: ResourceStr[]): SelectOption[] {
  const out: SelectOption[] = [];
  arr.forEach((v: ResourceStr) => {
    const o: SelectOption = { value: v };
    out.push(o);
  });
  return out;
}

// Android uses a 5-digit numeric field with a port validator. 0 means invalid here.
function settingsPort(text: string): number {
  const v = text.trim();
  if (!/^[0-9]{1,5}$/.test(v)) return 0;
  const port = Number(v);
  return port >= 1 && port <= 65535 ? port : 0;
}

// Native OSD expects #RRGGBBAA. #RRGGBB is accepted as opaque. '' means invalid.
function osdColorValue(text: string): string {
  const v = text.trim();
  if (/^#[0-9A-Fa-f]{8}$/.test(v)) return v.toUpperCase();
  if (/^#[0-9A-Fa-f]{6}$/.test(v)) return `${v.toUpperCase()}FF`;
  return '';
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
    motionTimeout: Math.min(120, Math.max(10, pref.getSync('motionTimeout', 15) as number)),
    keyframeInterval: pref.getSync('keyframeInterval', 2) as number,
    path: pref.getSync('path', 'live') as string,
    user: pref.getSync('user', 'admin') as string,
    password: pref.getSync('password', 'admin') as string,
    rtmpUrl: pref.getSync('rtmpUrl', '') as string
  };
  if (s.resolution < 0 || s.resolution >= LEGACY_RESOLUTIONS.length) s.resolution = 1;
  if (s.bitrate < 0 || s.bitrate >= BITRATES.length) s.bitrate = 1;
  if (s.codec < 0 || s.codec >= CODECS.length) s.codec = 0;
  if (s.facing < 0 || s.facing >= FACING.length) s.facing = 0;
  return s;
}

@Entry
@Component
struct Index {
  @State running: boolean = false;
  @State patternRunning: boolean = false;
  @State captureRotation: number = -1;
  @State captureMirror: boolean = false;
  @State cameraRunning: boolean = false;
  @State micRunning: boolean = false;
  @State rtmpState: RtmpPhase = 'idle';
  @State rtmpStatus: string = '';
  @State serviceNotice: string = '';
  private coldAutoPending: boolean = false;
  private rtmpPollTimerId: number = -1;
  @State recording: boolean = false;
  @State httpRunning: boolean = false;
  @State motionEnabled: boolean = false;
  @State motionRecording: boolean = false;
  @State httpPort: string = '8081';
  @State motionTimeout: string = '15';
  @State keyframeInterval: string = '2';
  @State clients: number = 0;
  @State viewerTotal: number = 0;
  @State logs: string[] = [];
  @State rtspUrl: string = '';
  @State resolutions: CameraResolution[] = [];
  @State cameraWidth: number = 1280;
  @State cameraHeight: number = 720;
  @State fpsInfo: CaptureFpsInfo = { ranges: [] };
  private legacySize: string = '1280x720';
  private activeRtspConfig?: RtspStartConfig;
  @State devices: CameraDeviceOption[] = [];
  @State deviceId: string = '';
  @State encoders: EncoderOption[] = [];
  @State encoderName: string = '';
  @State avcProfile: number = -1;
  @State focusInfo: FocusInfo = { modes: [] };
  @State dualAvailable: boolean = false;
  @State dualLayout: number = 0;
  private activeDeviceId: string = '';
  private dualPlan?: DualCameraPlan;
  @State videoBitrate: string = '2000'; // kbit/s, zero uses automatic tuning.
  @State micRate: number = 48000;
  @State micChannels: number = 1;
  @State actualMicRate: number = 0;
  @State actualMicChannels: number = 0;
  private legacyIFrameMs: number = 2000;
  @State bitrateIdx: number = 1;
  @State codecIdx: number = 0;
  @State facingIdx: number = 0;
  @State port: string = '8554';
  @State streamPath: string = 'live';
  @State user: string = 'admin';
  @State password: string = 'admin';
  @State rtmpUrl: string = '';
  @State osdFont: OsdFontOptions = defaultOsdFont();
  @State osdEnabled: boolean = false;
  @State osdPositionIdx: number = 0;
  @State osdFontStyleIdx: number = 0;
  @State osdPadding: string = '1';
  @State osdColor: string = '#FFFFFFFF';
  private osdColorSaved: string = '#FFFFFFFF';  // last complete color sent to native
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
  @State segFourGb: boolean = true;
  @State segMinutes: string = '10';
  @State autoRtsp: boolean = false;
  @State autoRtmp: boolean = false;
  @State bootStatus: string = '';
  @State keepScreenOn: boolean = false;
  private pageVisible: boolean = false;
  private ownedScreenWindow?: window.Window;
  @State camOnConnect: boolean = true;
  @State panel: ServicePanel = 'preview';
  @State settingsGroup: PreferenceGroup = 'root';
  @State overflowOpen: boolean = false;
  @State shareWeb: boolean = false;
  private enteredFromSettings: boolean = false;
  private exitConfirmed: boolean = false;
  @State cameraStarting: boolean = false;
  @State micStarting: boolean = false;
  @State httpUrl: string = '';
  private cameraGeneration: number = 0;
  private remoteSizeGeneration: number = 0;
  private remoteSizeTimerId: number = -1;
  private micGeneration: number = 0;
  private savedRtmpUrl: string = '';
  @State recordAudio: number = 1; // Silent / use an open mic / request mic for manual recording.
  @State recordVideo: number = 0;
  @State recordBitrateKbps: string = '4000';
  @State recordGopMs: string = '2000';
  @State allowLegacyVideoPassword: boolean = false;
  @State recordStarting: boolean = false;
  private recordTicket: number = 0;
  private recorder: RecordingOwnership = new RecordingOwnership(new PageRecordingControls(
    () => this.micRunning, () => this.micGeneration, () => this.startGrantedMic(++this.micGeneration),
    () => this.endMicSession(), (path: string, opts: RecordingSettings) => this.server.startRecord(path, opts),
    () => this.server.stopRecord()));
  private downloads: RecordingDownloads = new RecordingDownloads();
  @State copyDownloads: boolean = false;
  @State authorizingDownload: boolean = false;
  private copyGeneration: number = 0;
  private alertSnapshots: Set<string> = new Set();
  private copyPending: number = 0;
  private copyQueue: Promise<void> = Promise.resolve();
  private episodeStarted: boolean = false;
  private episodeGeneration: number = 0;
  private manualRecordTimerId: number = -1;
  private motionRecordGate: MotionRecordingGate = new MotionRecordingGate();
  private gpsGeneration: number = 0;
  private gpsPermissionPending: boolean = false;
  private keepAliveOn: boolean = false;
  private server: RtspServer = new RtspServer();
  private watermarkGeneration: number = 0;
  private gpsTimerId: number = -1;
  private batteryTimerId: number = -1;
  private publishIntent: number = 0;
  private stoppingPublisher: boolean = false;
  private publisher: RtmpRetryController = new RtmpRetryController({
    start: (url: string) => this.pageActive && this.pageVisible && this.running ?
      this.server.startRtmp(url) : Promise.resolve(false),
    stop: () => this.server.stopRtmp(),
    changed: (phase: RtmpPhase, retry: number, delayMs: number) => this.onPublisherChanged(phase, retry, delayMs)
  });
  private pageActive: boolean = false;
  private motionRecordPath: string = '';
  private motionTimerId: number = -1;
  private motionGeneration: number = 0;

  aboutToAppear(): void {
    this.pageActive = true;
    this.rtmpStatus = this.msg('srv_rtmp_idle');
    this.bootStatus = this.msg('srv_not_checked');
    const ctx = getContext(this) as common.UIAbilityContext;
    const s: Settings = loadSettings(ctx);
    this.legacySize = LEGACY_RESOLUTIONS[s.resolution];
    this.bitrateIdx = s.bitrate;
    this.codecIdx = s.codec;
    this.facingIdx = s.facing;
    const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
    this.deviceId = pref.getSync(`cameraDevice_${this.facingIdx}`, '') as string;
    const rotation = pref.getSync('captureRotation', -1) as number;
    this.captureRotation = [-1,0,90,180,270].includes(rotation) ? rotation : -1;
    this.captureMirror = pref.getSync('captureMirror', false) as boolean;
    const rate = pref.getSync('micRate', 48000) as number;
    this.micRate = [0, 8000, 12000, 16000, 22050, 24000, 32000, 44100, 48000].includes(rate) ? rate : 48000;
    this.micChannels = pref.getSync('micChannels', 1) === 2 ? 2 : 1;
    this.legacyIFrameMs = s.keyframeInterval * 1000;
    this.loadCodecSettings();
    this.loadPreferredSize();
    this.port = `${s.port}`;
    this.httpPort = `${s.httpPort}`;
    this.motionTimeout = `${s.motionTimeout}`;
    this.streamPath = s.path;
    this.user = s.user;
    this.password = s.password;
    this.rtmpUrl = s.rtmpUrl;
    this.savedRtmpUrl = s.rtmpUrl;
    const params = router.getParams() as ServicePageParams;
    this.enteredFromSettings = params?.section === 'settings';
    this.panel = this.enteredFromSettings ? 'settings' : 'preview';
    this.recordVideo = pref.getSync('recordVideoMode', 0) as number;
    this.recordBitrateKbps = `${pref.getSync('recordBitrateKbps', '4000')}`;
    this.recordGopMs = `${pref.getSync('recordGopMs', '2000')}`;
    this.allowLegacyVideoPassword = pref.getSync('allowLegacyVideoPassword', false) as boolean;
    this.recordAudio = pref.getSync('recordAudioMode', 1) as number;
    this.coldAutoPending = params?.coldAutoStart === true;
    this.loadOsdSettings();
    this.updateCameraLabel();
    // Native chooses the direct or composited surface when the camera starts.
    this.applyOsd(false);
    this.refreshUrl();
    // Keep one registration across stop/start. A recorder's final close is posted asynchronously.
    if (!this.server.setArchiveCallback((path: string) => this.onArchiveClosed(ctx, path))) {
      this.appendLog(this.msg('srv_archive_events_failed'));
    }
    this.server.setStatusCallback((event: number, clients: number, detail: string) => {
      if (!this.pageActive) return;
      if (event === RtspEvent.CameraSizeChanged) {
        // A broadcast invalidates the snapshot; neither detail nor clients is session state.
        this.syncRemoteCameraSize();
        return;
      }
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
      if (event === RtspEvent.Connected || event === RtspEvent.Disconnected) this.refreshViewerCounts();
      if (event === RtspEvent.Connected && this.camOnConnect && this.running &&
          !this.cameraRunning && !this.patternRunning) {
        // Android start_camera_on_connect: first client brings the camera up
        this.requestCameraStart();
      }
      if (event === RtspEvent.Error && detail.startsWith('rtmp:')) {
        this.publisher.nativeError(this.server.rtmpFailure(detail));
        return;
      }
      if (event === RtspEvent.Error && detail.startsWith('record:')) {
        this.syncRecordingState();
      }
      if (detail === 'MOTION detected' || detail === 'MOTION cleared') {
        // Status delivery is lossy; only the native-state timer controls recording.
        this.appendLog(detail);
      } else if (detail.startsWith('snapshot')) {
        if (detail.startsWith('snapshot saved ')) {
          const path = detail.substring('snapshot saved '.length);
          if (this.alertSnapshots.delete(path)) this.completeDelivery(path, ctx);
        }
        this.appendLog(detail);
      } else {
        this.appendLog(`${name} ${detail} (clients=${clients})`);
      }
    });
    this.appendLog(this.msg('srv_ready'));
  }

  reportProblem(message: string): void {
    this.serviceNotice = message;
    this.appendLog(message);
  }

  async coldAutoStart(): Promise<void> {
    const intent = this.publishIntent;
    const validTarget = canPublish(this.rtmpUrl.trim());
    if (this.autoRtsp || (this.autoRtmp && validTarget)) this.onStartServer();
    if (!this.autoRtmp) return;
    if (!validTarget) {
      this.reportProblem(this.msg('srv_auto_target'));
      return;
    }
    if (!this.running) { this.reportProblem(this.msg('srv_auto_service')); return; }
    if (this.codecIdx !== 0) { this.reportProblem(this.msg('srv_auto_avc')); return; }
    await this.requestCameraStart();
    if (!this.pageActive || intent !== this.publishIntent || !this.running) return;
    if (!this.cameraRunning) {
      this.reportProblem(this.msg('srv_auto_camera'));
      return;
    }
    this.onStartRtmp();
  }

  onPageShow(): void {
    this.pageActive = true;
    // Coming back to the page (foreground return, or back from Playback):
    // the system closes the camera device while the app is backgrounded
    // (privacy_service + camera_service CloseDevice), and nothing rebuilds
    // the capture session on the way in — without this restart the stream
    // and the preview stay frozen on the last frame.
    this.pageVisible = true;
    if (this.running) this.refreshViewerCounts();
    const remoteChanged = this.syncRemoteCameraSize();
    this.startRemoteSizeTimer();
    if (!this.cameraRunning) this.refreshCameraSizesIfGranted();
    if (this.osdEnabled && this.osdShowGps) this.startGpsTimer(false);
    this.updateKeepScreenOn();
    const ctx = getContext(this) as common.UIAbilityContext;
    const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
    const selected = pref.getSync('rtmpUrl', this.savedRtmpUrl) as string;
    if (selected !== this.savedRtmpUrl) {
      this.onStopRtmp();
      this.rtmpUrl = selected;
      this.savedRtmpUrl = selected;
    }
    this.syncRecordingState();
    if (this.coldAutoPending) {
      this.coldAutoPending = false;
      this.coldAutoStart();
    } else if (this.cameraRunning && !remoteChanged) this.requestCameraStart(false, true);
  }

  openRtmpAddresses(): void {
    router.pushUrl({ url: 'pages/RtmpAddrs' }).catch(() => {
      this.appendLog(this.msg('srv_manage_failed'));
    });
  }

  onPageHide(): void {
    this.pageVisible = false;
    if (this.cameraStarting) { this.cameraGeneration++; this.cameraStarting = false; }
    if (this.recordStarting) this.releaseRecording();
    this.stopRemoteSizeTimer();
    if (this.rtmpState === 'waiting' || this.rtmpState === 'connecting') this.onStopRtmp();
    this.stopGpsTimer();  // No location polling while the page is hidden.
    this.updateKeepScreenOn();
  }

  updateKeepScreenOn(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    screenTaskQueue = screenTaskQueue.then(async () => {
      try {
        if (this.pageVisible && this.keepScreenOn) {
          const win = await window.getLastWindow(ctx);
          if (!this.pageVisible || !this.keepScreenOn || win.getWindowProperties().isKeepScreenOn) return;
          await win.setWindowKeepScreenOn(true);
          this.ownedScreenWindow = win;
        } else if (this.ownedScreenWindow) {
          // Restore only a flag enabled by this page, not a pre-existing window setting.
          await this.ownedScreenWindow.setWindowKeepScreenOn(false);
          this.ownedScreenWindow = undefined;
        }
      } catch (err) {
        hilog.warn(DOMAIN, TAG, 'setWindowKeepScreenOn failed');
      }
    });
  }

  aboutToDisappear(): void {
    this.pageActive = false;
    // Retirement cancels unclaimed completions. Only an ordinary stop drains files to delivery.
    this.copyGeneration++;
    this.copyDownloads = false;
    this.downloads.disable();
    this.pageVisible = false;
    this.updateKeepScreenOn();
    this.watermarkGeneration++;
    this.stopGpsTimer();
    this.stopBatteryTimer();
    this.stopMotionMode('motion detection OFF (page closed)');
    this.stopRemoteSizeTimer();
    leaseBindings.stopAll();
    lanServiceAnnouncements(getContext(this)).stopAll();
    this.server.stopHttp();
    this.httpRunning = false;
    this.releaseRecording();
    this.recording = false;
    this.onStopRtmp();
    this.endMicSession();
    this.cameraGeneration++;
    this.cameraStarting = false;
    this.server.stopCamera();
    this.markLocalCameraBoundary();
    this.server.stopTestPattern();
    this.server.stop();
    this.server.clearArchiveCallback();
    this.alertSnapshots.clear();
    this.server.clearStatusCallback();
    this.stopManualRecordTimer();
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
      const ip: number = info.ipAddress >>> 0;  // network-order (big-endian)
      if (ip !== 0) return `${(ip >>> 24) & 0xFF}.${(ip >>> 16) & 0xFF}.${(ip >>> 8) & 0xFF}.${ip & 0xFF}`;
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'getIpInfo failed');
    }
    // Local Wi-Fi data only. A link-local scope belongs to this host and cannot be copied to a peer.
    try {
      const info = wifiManager.getIpv6Info();
      const address = [info.uniqueIpv6Address ?? '', info.globalIpv6Address, info.randomGlobalIpv6Address]
        .find((v: string) => !!v && v.includes(':') && /^[a-f0-9:]+$/i.test(v) && v !== '::' &&
          v !== '::1' && !/^fe[89ab]/i.test(v));
      if (address) return `[${address}]`;
    } catch (_) {}
    return '0.0.0.0';
  }

  refreshUrl(): void {
    const cfg = this.activeRtspConfig ?? this.makeConfig();
    this.rtspUrl = `rtsp://${this.deviceIp()}:${cfg.port}/${cfg.path}`;
  }

  copyRtspAddress(withCredentials: boolean = false): void {
    const cfg = this.activeRtspConfig ?? this.makeConfig();
    try {
      const auth = withCredentials && cfg.user ? `${encodeURIComponent(cfg.user)}:${encodeURIComponent(cfg.password)}@` : '';
      const address = `rtsp://${auth}${this.rtspUrl.substring(7)}`;
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, address);
      const property = data.getProperty(); property.shareOption = pasteboard.ShareOption.LOCALDEVICE;
      data.setProperty(property); pasteboard.getSystemPasteboard().setDataSync(data);
      this.getUIContext().getPromptAction().showToast({ message: this.msg(withCredentials ? 'srv_copied' : 'srv_copied_safe') });
    } catch (_) {
      this.appendLog(this.msg('srv_copy_url_failed'));
    }
  }

  timestamp(): string {
    const d: Date = new Date();
    const p = (n: number): string => (n < 10 ? '0' : '') + n;
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  msg(key: string, ...args: Array<string | number>): string {
    return getContext(this).resourceManager.getStringByNameSync(key, ...args);
  }

  appendLog(msg: string): void {
    this.logs = [`[${this.timestamp()}] ${msg}`, ...this.logs].slice(0, 200);
  }

  makeConfig(): RtspStartConfig {
    const cfg: RtspStartConfig = {
      port: settingsPort(this.port) || 8554,
      path: this.streamPath.trim() === '' ? 'live' : this.streamPath.trim(),
      user: this.user,
      password: this.password
    };
    return cfg;
  }

  async requestCapturePermission(permission: Permissions): Promise<boolean> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const manager = abilityAccessCtrl.createAtManager();
      if (manager.checkAccessTokenSync(ctx.applicationInfo.accessTokenId, permission) === 0) return true;
      const result = await manager.requestPermissionsFromUser(ctx, [permission]);
      if (result.authResults.length === 1 && result.authResults[0] === 0) return true;
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'capture permission request failed');
    }
    if (this.pageActive) {
      this.reportProblem(permission === 'ohos.permission.CAMERA' ? this.msg('srv_camera_perm') : this.msg('srv_mic_perm'));
    }
    return false;
  }

  async startKeepAlive(uiCtx: common.UIAbilityContext, generation: number): Promise<void> {
    if (this.keepAliveOn || generation !== this.micGeneration) return;
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
      if (generation !== this.micGeneration || !this.micRunning) return;
      await backgroundTaskManager.startBackgroundRunning(uiCtx, backgroundTaskManager.BackgroundMode.AUDIO_RECORDING, agent);
      this.keepAliveOn = true;
      if (this.pageActive) this.appendLog('录音后台连续任务已启动');
    } catch (err) {
      const e = err as BusinessError;
      hilog.error(DOMAIN, TAG, 'startBackgroundRunning: %{public}d %{public}s', e.code, e.message);
      if (this.pageActive) this.reportProblem(this.msg('srv_bg_error', e.code));
    }
  }

  async stopKeepAlive(uiCtx: common.UIAbilityContext): Promise<void> {
    if (!this.keepAliveOn) return;
    try {
      await backgroundTaskManager.stopBackgroundRunning(uiCtx);
      this.keepAliveOn = false;
      if (this.pageActive) this.appendLog('录音后台连续任务已停止');
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'stopBackgroundRunning failed');
    }
  }

  onStartServer(): void {
    const cfg = this.makeConfig();
    leaseBindings.stop('RTSP');
    const ok: boolean = this.server.start(cfg);
    this.activeRtspConfig = ok ? cfg : undefined;
    this.running = ok;
    this.refreshUrl();
    if (ok) {
      const port = this.server.boundV4Port(false);
      const authenticated = leaseCredentials(cfg.user, cfg.password) && port === cfg.port;
      leaseBindings.set('RTSP', port, authenticated, () => this.server.boundV4Port(false) === port);
      lanServiceAnnouncements(getContext(this)).rtsp.setBoundPort(cfg.port);
      this.serviceNotice = '';
      this.appendLog(this.msg('srv_rtsp_up', this.rtspUrl));
    } else this.reportProblem(this.msg('srv_start_failed'));
  }

  onStopServer(): void {
    this.stopMotionMode('motion detection OFF (server stopped)');
    this.stopRemoteSizeTimer();
    leaseBindings.stopAll();
    lanServiceAnnouncements(getContext(this)).stopAll();
    this.server.stopHttp();
    this.httpRunning = false;
    this.releaseRecording();
    this.recording = false;
    this.onStopRtmp();
    this.endMicSession();
    this.cameraGeneration++;
    this.cameraStarting = false;
    this.server.stopCamera();
    this.markLocalCameraBoundary();
    this.server.stopTestPattern();
    this.patternRunning = false;
    this.cameraRunning = false;
    this.refreshFps();
    this.micRunning = false;
    this.server.stop();
    // The writer has joined; native atomically claims paths so queued callbacks cannot deliver them twice.
    const ctx = getContext(this) as common.UIAbilityContext;
    this.server.takePendingArchives().forEach((path: string) => this.onArchiveClosed(ctx, path));
    this.alertSnapshots.clear();
    this.running = false;
    this.activeRtspConfig = undefined;
    this.refreshUrl();
    this.clients = 0;
    this.viewerTotal = 0;
    this.appendLog('server stopped');
    this.stopManualRecordTimer();
  }

  cameraChangeBusy(): boolean {
    return this.recordStarting || this.recording || this.motionRecording || this.rtmpState !== 'idle';
  }

  rejectBusyCameraChange(): boolean {
    if (!this.cameraChangeBusy()) return false;
    this.reportProblem(this.msg('srv_camera_busy'));
    return true;
  }

  // Per-keystroke field validation: show a notice while invalid, clear it once fixed. No log spam.
  fieldNotice(valid: boolean, key: string): void {
    const message = this.msg(key);
    if (!valid) this.serviceNotice = message;
    else if (this.serviceNotice === message) this.serviceNotice = '';
  }

  // The running encoder keeps its codec; never show a codec the stream is not using.
  selectCodec(idx: number): void {
    if (idx === this.codecIdx || this.cameraStarting || this.rejectBusyCameraChange()) return;
    this.codecIdx = idx;
    this.saveSetting('codec', idx);
    this.loadCodecSettings();
    if (this.cameraRunning) this.requestCameraStart();
  }

  async requestCameraStart(armMotion: boolean = false, resume: boolean = false): Promise<void> {
    if (!this.running || !this.pageActive || !this.pageVisible || this.cameraStarting) return;
    if (this.cameraChangeBusy()) {
      if (!resume) this.reportProblem(this.msg('srv_camera_busy'));
      return;
    }
    this.cameraStarting = true;
    const generation = ++this.cameraGeneration;
    const granted = await this.requestCapturePermission('ohos.permission.CAMERA');
    if (generation !== this.cameraGeneration) return;
    this.cameraStarting = false;
    if (!granted) {
      if (this.cameraRunning) this.onStopCamera();
      return;
    }
    // A foreground repair is not a user request to start a failed remote session.
    if (resume && this.syncRemoteCameraSize()) return;
    if (!this.refreshCameraSizes()) {
      this.appendLog(this.msg('srv_no_sizes'));
      if (this.cameraRunning) this.onStopCamera();
      return;
    }
    if (this.cameraChangeBusy()) return;
    if (!resume) this.markLocalCameraBoundary();
    const desiredLayout = this.cameraRunning ? this.dualLayout : 0;
    this.cameraRunning = this.startCameraNow();
    this.refreshFps();
    if (this.cameraRunning) {
      const pref = preferences.getPreferencesSync(getContext(this), { name: 'ipcamSettings' });
      const layout = pref.getSync(`dualLayout_${this.activeDeviceId}`, desiredLayout) as number;
      if (this.dualAvailable && Number.isInteger(layout) && layout >= 1 && layout <= 3) this.setDualLayout(layout);
      if (this.cameraRunning) this.restoreCaptureControls(pref);
    }
    this.patternRunning = false; // Native replaces any test pattern even when camera setup fails.
    if (!this.cameraRunning && this.motionEnabled) this.stopMotionMode('相机未启动，移动侦测已停止');
    this.appendLog(this.cameraRunning
      ? `相机 · ${CODECS[this.codecIdx]} · ${this.cameraWidth}×${this.cameraHeight}`
      : this.msg('srv_camera_failed'));
    if (armMotion && this.cameraRunning && !this.motionEnabled) this.onToggleMotion();
  }

  // Local actions do not advance the native generation. Consume earlier remote changes
  // without applying them over the user's local start/stop when a queued broadcast arrives.
  markLocalCameraBoundary(): void {
    const state = this.server.cameraRemoteSizeState();
    if (state) this.remoteSizeGeneration = Math.max(this.remoteSizeGeneration, state.generation);
  }

  syncRemoteCameraSize(): boolean {
    if (!this.pageActive || !this.running || this.cameraStarting) return false;
    const state = this.server.cameraRemoteSizeState();
    if (!state || state.generation <= this.remoteSizeGeneration) return false;
    this.remoteSizeGeneration = state.generation;
    this.cameraRunning = state.running;
    if (state.running) {
      this.patternRunning = false;
      this.cameraWidth = state.width;
      this.cameraHeight = state.height;
      // Use capture pixels, not OSD/encoded dimensions. The picker needs a matching entry
      // even if this page's last enumeration was empty. Do not change the saved preference.
      if (!this.resolutions.some((s: CameraResolution) => s.width === state.width && s.height === state.height)) {
        const actual: CameraResolution = { width: state.width, height: state.height };
        this.resolutions = this.resolutions.concat([actual]);
      }
    } else if (this.motionEnabled) {
      this.stopMotionMode('相机未运行，移动侦测已停止');
    }
    // 0x0 means stopped, not a valid selection for the next local start.
    this.refreshFps();
    return true;
  }

  startRemoteSizeTimer(): void {
    if (this.remoteSizeTimerId >= 0 || !this.httpRunning || !this.pageVisible) return;
    this.remoteSizeTimerId = setInterval(() => {
      if (this.httpRunning && this.pageVisible) {
        this.syncRemoteCameraSize();
        this.refreshViewerCounts();
      }
    }, 1000);
  }

  stopRemoteSizeTimer(): void {
    if (this.remoteSizeTimerId < 0) return;
    clearInterval(this.remoteSizeTimerId);
    this.remoteSizeTimerId = -1;
  }

  loadPreferredSize(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
    const size = (pref.getSync(this.sizePreference(), this.legacySize) as string).split('x');
    this.cameraWidth = parseInt(size[0]) || 1280;
    this.cameraHeight = parseInt(size[1]) || 720;
  }

  refreshCameraSizesIfGranted(): void {
    if (this.cameraRunning) return; // Keep the active capture selection when opening settings.
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      if (abilityAccessCtrl.createAtManager().checkAccessTokenSync(ctx.applicationInfo.accessTokenId,
        'ohos.permission.CAMERA') === 0) {
        this.refreshCameraSizes();
      } else {
        this.resolutions = [];
      }
    } catch (_) {
      this.resolutions = [];
    }
  }

  refreshCameraSizes(): boolean {
    this.devices = parseCameraDevices(this.server.cameraDevices());
    this.resolutions = parseCameraResolutions(this.server.cameraResolutions(this.facingIdx === 1, this.deviceId));
    const size = selectCameraResolution(this.resolutions, this.cameraWidth, this.cameraHeight);
    if (!size) return false;
    this.cameraWidth = size.width;
    this.cameraHeight = size.height;
    return true;
  }

  selectResolution(size: CameraResolution): void {
    if (this.dualLayout !== 0 || this.cameraStarting || this.rejectBusyCameraChange()) return;
    if (!this.resolutions.some((v: CameraResolution) => v.width === size.width && v.height === size.height)) return;
    this.cameraWidth = size.width;
    this.cameraHeight = size.height;
    this.saveSetting(this.sizePreference(), `${size.width}x${size.height}`);
    if (this.cameraRunning) this.requestCameraStart();
  }

  selectFacing(idx: number): void {
    if (this.facingIdx === idx || this.dualLayout !== 0 || this.cameraStarting || this.rejectBusyCameraChange()) return;
    this.facingIdx = idx;
    this.saveSetting('facing', idx);
    const pref = preferences.getPreferencesSync(getContext(this), { name: 'ipcamSettings' });
    this.deviceId = pref.getSync(`cameraDevice_${idx}`, '') as string;
    this.updateCameraLabel();
    this.loadPreferredSize();
    this.resolutions = [];
    this.fpsInfo = { ranges: [] };
    if (this.cameraRunning) this.requestCameraStart();
    else this.refreshCameraSizesIfGranted();
  }

  sizePreference(): string {
    return this.deviceId ? `cameraSize_id_${this.deviceId}` : `cameraSize_${this.facingIdx}`;
  }

  updateCameraLabel(): void {
    this.server.setOsdDeviceName(this.deviceId || this.msg(this.facingIdx === 1 ? 'srv_front' : 'srv_rear'));
  }

  selectDevice(device: CameraDeviceOption): void {
    if (this.cameraStarting || this.dualLayout !== 0 || this.rejectBusyCameraChange() || (device.id !== '' &&
        !this.devices.some((d: CameraDeviceOption) => d.id === device.id && d.front === device.front))) return;
    if (device.id) this.facingIdx = device.front ? 1 : 0;
    this.deviceId = device.id;
    this.saveSetting('facing', this.facingIdx);
    this.saveSetting(`cameraDevice_${this.facingIdx}`, device.id);
    this.loadPreferredSize();
    this.updateCameraLabel();
    if (this.cameraRunning) this.requestCameraStart(); else this.refreshCameraSizesIfGranted();
  }

  videoMime(): string { return this.codecIdx === 1 ? 'video/hevc' : 'video/avc'; }

  loadCodecSettings(): void {
    const mime = this.videoMime();
    const pref = preferences.getPreferencesSync(getContext(this), { name: 'ipcamSettings' });
    const tuning = parseCodecTuning(pref.getSync(codecTuningKey(mime), '') as string,
      (1 << this.bitrateIdx) * 1000000, this.legacyIFrameMs);
    this.videoBitrate = `${tuning.bitrate / 1000}`;
    this.keyframeInterval = `${tuning.iFrameMs / 1000}`;
    this.encoders = parseEncoderOptions(this.server.encoderOptions(mime));
    const savedEncoder = pref.getSync(`encoder_${mime}`, '') as string;
    const encoder = this.encoders.find((e: EncoderOption) => e.name === savedEncoder);
    this.encoderName = encoder?.name ?? '';
    const profile = pref.getSync('avcProfile', -1) as number;
    this.avcProfile = mime === 'video/avc' && encoder && encoder.profiles.includes(profile) ? profile : -1;
  }

  selectEncoder(v: EncoderOption): void {
    if (this.cameraRunning || this.cameraStarting || (v.name && !this.encoders.some((e: EncoderOption) => e.name === v.name))) return;
    this.encoderName = v.name; this.saveSetting(`encoder_${this.videoMime()}`, v.name);
    this.avcProfile = -1;
    if (this.codecIdx === 0) this.saveSetting('avcProfile', -1);
  }

  selectProfile(profile: number): void {
    const encoder = this.encoders.find((e: EncoderOption) => e.name === this.encoderName);
    if (this.cameraRunning || this.cameraStarting || this.codecIdx !== 0 || !encoder ||
        (profile !== -1 && !encoder.profiles.includes(profile))) return;
    this.avcProfile = profile; this.saveSetting('avcProfile', profile);
  }

  tuning(): CodecTuning {
    const tuning: CodecTuning = { bitrate: Number(this.videoBitrate) * 1000,
      iFrameMs: Number(this.keyframeInterval) * 1000 };
    return parseCodecTuning(JSON.stringify(tuning), (1 << this.bitrateIdx) * 1000000, this.legacyIFrameMs);
  }

  saveTuning(): void { this.saveSetting(codecTuningKey(this.videoMime()), JSON.stringify(this.tuning())); }

  setFocusMode(mode: number): void {
    if (!this.cameraRunning || this.cameraStarting || !this.focusInfo.modes.includes(mode)) return;
    const applied = this.server.cameraSetFocusMode(mode);
    this.focusInfo = parseFocusInfo(this.server.cameraFocusInfo());
    if (!applied) this.reportProblem(this.msg('srv_focus_failed'));
    else if (this.activeDeviceId && this.focusInfo.active === mode) this.saveSetting(`focus_${this.activeDeviceId}`, mode);
  }

  focusCenter(): void {
    if (this.cameraRunning && !this.cameraStarting && !this.server.cameraFocusAt(0.5, 0.5)) {
      this.reportProblem(this.msg('srv_focus_failed'));
    }
    if (this.cameraRunning) this.focusInfo = parseFocusInfo(this.server.cameraFocusInfo());
  }

  refreshFps(): void {
    this.dualLayout = this.cameraRunning ? this.server.dualLayout() : 0;
    this.dualPlan = this.cameraRunning ? queryDualCamera(getContext(this), this.facingIdx === 1,
      this.cameraWidth, this.cameraHeight) : undefined;
    this.dualAvailable = !!this.dualPlan && this.dualPlan.primaryId === this.activeDeviceId;
    this.focusInfo = this.cameraRunning ? parseFocusInfo(this.server.cameraFocusInfo()) : { modes: [] };
    this.fpsInfo = this.cameraRunning ? parseCaptureFps(this.server.cameraCaptureFps()) : { ranges: [] };
  }

  restoreCaptureControls(pref: preferences.Preferences): void {
    if (!this.activeDeviceId) return;
    const focus = pref.getSync(`focus_${this.activeDeviceId}`, -1) as number;
    if (this.focusInfo.active !== focus && this.focusInfo.modes.includes(focus)) this.setFocusMode(focus);
    const saved = pref.getSync(this.fpsPreference(), '') as string;
    const range = this.fpsInfo.ranges.find((r: CaptureFpsRange) => `${r.min}-${r.max}` === saved);
    if (range && (this.fpsInfo.active?.min !== range.min || this.fpsInfo.active?.max !== range.max)) this.selectCaptureFps(range);
  }

  fpsPreference(): string { return `fps_${this.activeDeviceId}_${this.cameraWidth}x${this.cameraHeight}`; }

  selectCaptureFps(range: CaptureFpsRange): void {
    if (!this.cameraRunning || this.cameraStarting) return;
    this.fpsInfo = parseCaptureFps(this.server.cameraCaptureFps());
    if (!this.fpsInfo.ranges.some((v: CaptureFpsRange) => v.min === range.min && v.max === range.max)) return;
    const applied = this.server.cameraSetCaptureFps(range.min, range.max);
    this.fpsInfo = parseCaptureFps(this.server.cameraCaptureFps());
    if (!applied) this.reportProblem(this.msg('srv_fps_failed'));
    else if (this.activeDeviceId && this.fpsInfo.active?.min === range.min && this.fpsInfo.active?.max === range.max) {
      this.saveSetting(this.fpsPreference(), `${range.min}-${range.max}`);
    }
  }

  setDualLayout(layout: number): void {
    if (!this.cameraRunning || this.cameraStarting || layout === this.dualLayout ||
        (layout !== 0 && !this.dualAvailable)) return;
    const status = this.server.setDualLayout(layout, this.dualPlan);
    if (status === 3) {
      this.cameraRunning = false;
      this.stopMotionMode(this.msg('srv_camera_lost'));
    }
    this.refreshFps();
    if (status === 0 && this.dualLayout === layout && this.activeDeviceId) this.saveSetting(`dualLayout_${this.activeDeviceId}`, layout);
    if (status !== 0) this.reportProblem(this.msg(status === 3 ? 'srv_camera_lost' :
      status === 2 ? 'srv_dual_restored' : 'srv_dual_busy'));
  }

  startCameraNow(): boolean {
    const tuning = this.tuning();
    const selected = this.encoders.find((e: EncoderOption) => e.name === this.encoderName);
    const bitrate = resolveCameraBitrate(tuning, this.cameraWidth, this.cameraHeight, selected, this.videoMime());
    const device = this.devices.find((d: CameraDeviceOption) => d.front === (this.facingIdx === 1));
    const id = this.deviceId || device?.id || '';
    const ok = this.server.startCamera(this.cameraWidth, this.cameraHeight, bitrate, this.videoMime(),
      this.facingIdx === 1, tuning.iFrameMs, id, this.encoderName, this.avcProfile, this.captureRotation, this.captureMirror);
    this.activeDeviceId = ok ? id : '';
    if (ok && id) this.server.setOsdDeviceName(id);
    return ok;
  }

  onStopCamera(): void {
    if (this.rejectBusyCameraChange()) return;
    this.cameraGeneration++;
    this.cameraStarting = false;
    this.stopMotionMode('motion detection OFF (camera stopped)');
    this.server.stopCamera();
    this.markLocalCameraBoundary();
    this.cameraRunning = false;
    this.refreshFps();
    this.appendLog('camera stopped');
  }

  async onStartMic(): Promise<void> {
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    if (this.micStarting || this.micRunning || !this.pageActive) return;
    this.micStarting = true;
    const generation = ++this.micGeneration;
    const granted = await this.requestCapturePermission('ohos.permission.MICROPHONE');
    if (generation !== this.micGeneration) return;
    this.micStarting = false;
    if (!granted) return;
    this.startGrantedMic(generation);
  }

  startGrantedMic(generation: number): boolean {
    this.micRunning = this.server.startMic(this.micRate, this.micChannels);
    const fmt = this.server.micFormat();
    this.actualMicRate = fmt.sampleRate;
    this.actualMicChannels = fmt.channels;
    this.appendLog(this.micRunning ? this.msg('srv_mic_format', fmt.sampleRate, fmt.channels) : this.msg('srv_mic_failed'));
    if (this.micRunning) {
      const ctx = getContext(this) as common.UIAbilityContext;
      audioTaskQueue = audioTaskQueue.then(() => this.startKeepAlive(ctx, generation));
    }
    return this.micRunning;
  }

  endMicSession(): void {
    this.micGeneration++;
    this.micStarting = false;
    this.server.stopMic();
    this.actualMicRate = 0;
    this.actualMicChannels = 0;
    this.micRunning = false;
    const ctx = getContext(this) as common.UIAbilityContext;
    audioTaskQueue = audioTaskQueue.then(() => this.stopKeepAlive(ctx));
  }

  onStopMic(): void {
    this.endMicSession();
    this.appendLog(this.msg('srv_mic_stopped'));
  }

  onPublisherChanged(phase: RtmpPhase, retry: number, delayMs: number): void {
    if (this.rtmpPollTimerId >= 0) {
      clearInterval(this.rtmpPollTimerId);
      this.rtmpPollTimerId = -1;
    }
    if (!this.pageActive) return;
    this.rtmpState = phase;
    if ((phase === 'waiting' || phase === 'connecting') && (!this.pageVisible || !this.running)) {
      this.onStopRtmp();
      return;
    }
    if (phase === 'publishing') {
      this.rtmpStatus = this.msg('srv_publishing');
      this.rtmpPollTimerId = setInterval(() => {
        if (this.rtmpState !== 'publishing' || !this.pageActive) return;
        const error = this.server.rtmpFailure();
        if (error !== '') this.publisher.nativeError(error);
      }, 1000);
    } else if (phase === 'waiting') {
      this.rtmpStatus = this.msg('srv_rtmp_wait', Math.ceil(delayMs / 1000), retry);
    } else if (phase === 'connecting') {
      this.rtmpStatus = retry > 0 ? this.msg('srv_rtmp_retry', retry) : this.msg('srv_connecting');
    } else {
      this.rtmpStatus = this.stoppingPublisher ? this.msg('srv_rtmp_stopped') :
        this.msg(this.rtmpUrl.trim().startsWith('rtmps://') ? 'srv_tls_failed' : 'srv_rtmp_exhausted');
    }
    this.appendLog(`RTMP：${this.rtmpStatus}`);
  }

  onStartRtmp(): void {
    if (!this.pageActive || !this.pageVisible) return;
    if (!this.running) { this.reportProblem(this.msg('srv_publish_requires_service')); return; }
    const url = this.rtmpUrl.trim();
    if (!canPublish(url)) { this.reportProblem(this.msg('srv_bad_target')); return; }
    this.serviceNotice = '';
    this.publishIntent++;
    this.saveSetting('rtmpUrl', url);
    this.savedRtmpUrl = url;
    this.publisher.start(url);
  }

  onStopRtmp(): void {
    this.publishIntent++;
    this.stoppingPublisher = true;
    try { this.publisher.stop(); } finally { this.stoppingPublisher = false; }
  }

  stopManualRecordTimer(): void {
    if (this.manualRecordTimerId < 0) return;
    clearInterval(this.manualRecordTimerId);
    this.manualRecordTimerId = -1;
  }

  syncRecordingState(): void {
    if ((!this.recording && !this.motionRecording) || this.server.isRecording()) return;
    // A delayed failure event from a previous file must never stop a newer writer.
    this.releaseRecording();
    this.motionRecording = false;
    this.motionRecordPath = '';
    this.recording = false;
    this.stopManualRecordTimer();
    this.reportProblem(this.msg('srv_record_interrupted'));
  }

  releaseRecording(): void {
    const ticket = this.recordTicket;
    this.recordTicket = 0;
    this.recordStarting = false;
    this.recorder.stop(ticket);
  }

  recordingOptions(audio: boolean): RecordingSettings {
    const options: RecordingSettings = { includeAudio: audio, fourGbLimit: this.segFourGb,
      segmentMinutes: Number(this.segMinutes) };
    if (this.recordVideo === 1 || this.recordVideo === 2) {
      options.videoMime = this.recordVideo === 1 ? 'video/avc' : 'video/hevc';
      options.videoBitrate = Number(this.recordBitrateKbps) * 1000;
      options.gopMs = Number(this.recordGopMs);
    }
    return options;
  }

  validRecordingVideo(): boolean {
    if (this.recordVideo === 0) return true;
    const kbps = Number(this.recordBitrateKbps);
    const gopMs = Number(this.recordGopMs);
    if ((this.recordVideo === 1 || this.recordVideo === 2) && Number.isSafeInteger(kbps) &&
        kbps >= 250 && kbps <= 20000 && Number.isSafeInteger(gopMs) && gopMs >= 500 && gopMs <= 10000) return true;
    this.reportProblem(this.msg('srv_rec_invalid'));
    return false;
  }

  async onToggleRecord(): Promise<void> {
    if (this.recordStarting) { this.releaseRecording(); return; }
    if (this.recording) {
      this.syncRecordingState();
      if (!this.recording) return;
      this.releaseRecording();
      this.stopManualRecordTimer();
      this.recording = false;
      this.appendLog(this.msg('srv_record_done'));
      return;
    }
    if (!this.running || this.motionEnabled || !this.pageActive || !this.pageVisible || this.micStarting || !this.validRecordingVideo()) return;
    const ticket = this.recorder.reserve();
    if (!ticket) return;
    this.recordTicket = ticket;
    this.recordStarting = true;
    const audio = this.recordAudio === 2 || (this.recordAudio === 1 && this.micRunning);
    try {
      if (audio && !this.micRunning && !(await this.requestCapturePermission('ohos.permission.MICROPHONE'))) return;
      if (this.recordTicket !== ticket || !this.running || !this.pageActive || !this.pageVisible || this.motionEnabled) return;
      const ctx = getContext(this) as common.UIAbilityContext;
      this.recording = this.recorder.start(ticket, `${ctx.filesDir}/ipc_${this.motionTimestamp()}.mp4`, this.recordingOptions(audio));
      if (!this.recording) { this.reportProblem(this.msg('srv_record_failed')); return; }
      this.serviceNotice = '';
      this.manualRecordTimerId = setInterval(() => this.syncRecordingState(), 1000);
      this.appendLog(this.msg('srv_manual_active'));
    } finally {
      if (this.recordTicket === ticket) {
        this.recordStarting = false;
        if (!this.recording) this.releaseRecording();
      }
    }
  }

  async requestMotionSnapshot(generation: number): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const path = await deliveryMotionSnapshotPath(ctx, this.motionTimestamp());
      if (!path || !this.pageActive || !this.pageVisible || generation !== this.episodeGeneration || !this.motionEnabled ||
          !this.server.motionActive()) return;
      if (this.alertSnapshots.size >= 32) this.alertSnapshots.delete(Array.from(this.alertSnapshots)[0]);
      this.alertSnapshots.add(path);
      if (!this.server.takeSnapshot(path)) {
        this.alertSnapshots.delete(path); this.appendLog(this.msg('srv_snapshot_failed'));
      }
    } catch (_) { if (this.pageActive) this.appendLog(this.msg('srv_snapshot_failed')); }
  }

  async completeDelivery(path: string, ctx: common.UIAbilityContext): Promise<void> {
    try {
      const result = await onDeliveryCompleted(ctx, path);
      if (result && this.pageActive) this.appendLog(result);
    } catch (_) { if (this.pageActive) this.appendLog(this.msg('srv_delivery_failed')); }
  }

  onArchiveClosed(ctx: common.UIAbilityContext, path: string): void {
    if (!this.pageActive) return;
    this.completeDelivery(path, ctx);
    if (!this.copyDownloads) return;
    if (this.copyPending >= 8) { if (this.pageActive) this.reportProblem(this.msg('srv_copy_backlog')); return; }
    const generation = this.copyGeneration;
    this.copyPending++;
    this.copyQueue = this.copyQueue.then(async () => {
      try {
        if (this.pageActive && generation === this.copyGeneration && this.copyDownloads &&
            await this.downloads.copyCompleted(ctx, path) && this.pageActive) this.appendLog(this.msg('srv_copy_done'));
      } catch (_) { if (this.pageActive) this.reportProblem(this.msg('srv_copy_failed')); }
      finally { this.copyPending--; }
    });
  }

  async toggleDownloadCopies(): Promise<void> {
    if (this.authorizingDownload) return;
    this.copyGeneration++;
    if (this.copyDownloads) {
      this.downloads.disable(); this.copyDownloads = false; return;
    }
    this.authorizingDownload = true;
    try {
      const granted = await this.downloads.authorize(getContext(this) as common.UIAbilityContext);
      if (this.pageActive) this.copyDownloads = granted;
    } catch (_) { if (this.pageActive) this.reportProblem(this.msg('srv_download_denied')); }
    finally { this.authorizingDownload = false; }
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

  configureLegacyVideoPassword(): void {
    if (this.allowLegacyVideoPassword) {
      this.allowLegacyVideoPassword = false; this.saveSetting('allowLegacyVideoPassword', false); return;
    }
    this.getUIContext().showAlertDialog({ title: $r('app.string.srv_legacy_title'),
      message: $r('app.string.srv_legacy_hint'),
      primaryButton: { value: $r('app.string.srv_cancel'), action: () => {} },
      secondaryButton: { value: $r('app.string.srv_enable'), action: () => {
        if (this.pageActive) { this.allowLegacyVideoPassword = true; this.saveSetting('allowLegacyVideoPassword', true); }
      } } });
  }

  onToggleHttp(): void {
    if (this.httpRunning) {
      this.stopRemoteSizeTimer();
      leaseBindings.stop('HTTP');
      lanServiceAnnouncements(getContext(this)).http.setBoundPort(0);
      this.server.stopHttp();
      this.httpRunning = false;
      this.refreshViewerCounts();
      this.syncRemoteCameraSize(); // Drain the last change even if its notification was lost.
      this.appendLog('web console stopped');
      return;
    }
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    const ctx = getContext(this) as common.UIAbilityContext;
    if (!this.user.trim() || this.password.trim().length < 12 || this.password.trim().toLowerCase() === this.user.trim().toLowerCase() ||
        new Set(this.password.split('')).size < 4 || /^(admin|password|123456|qwerty)[0-9!@#$%]*$/i.test(this.password)) {
      this.reportProblem(this.msg('srv_http_password'));
      return;
    }
    const opts: HttpOptions = {
      port: settingsPort(this.httpPort) || 8081,
      user: this.user,
      password: this.password,
      allowLegacyVideoPassword: this.allowLegacyVideoPassword,
      filesDir: ctx.filesDir
    };
    leaseBindings.stop('HTTP');
    const ok: boolean = this.server.startHttp(opts);
    this.httpRunning = ok;
    if (ok) {
      const port = this.server.boundV4Port(true);
      const authenticated = !opts.allowLegacyVideoPassword && leaseCredentials(opts.user, opts.password) && port === opts.port;
      leaseBindings.set('HTTP', port, authenticated, () => this.server.boundV4Port(true) === port);
      lanServiceAnnouncements(getContext(this)).http.setBoundPort(opts.port);
      this.startRemoteSizeTimer();
    }
    this.httpUrl = ok ? `http://${this.deviceIp()}:${opts.port}` : '';
    this.appendLog(ok ? `web console on ${this.httpUrl}`
                      : 'web console failed');
  }

  onToggleMotion(): void {
    if (this.motionEnabled) {
      this.stopMotionMode('motion detection OFF');
      return;
    }
    if (!this.pageVisible || !this.pageActive) return;
    // Android switches motion mode on by bringing the server up first
    // (BaseServerActivity.G0 -> R0). Detection and the auto recording both read
    // the encoded stream, and the encoders only emit their parameter sets at
    // stream start, so the server has to be up before anything can be written.
    if (!this.running) this.onStartServer();
    if (!this.running) {
      this.appendLog('motion detection needs the RTSP server (start failed)');
      return;
    }
    if (!this.cameraRunning) {
      this.requestCameraStart(true);
      return;
    }
    if (this.recording || this.recordStarting) {
      this.releaseRecording();
      this.recording = false;
      this.stopManualRecordTimer();
      this.appendLog('manual recording stopped (motion detection takes over)');
    }
    // Android clamps motion_timeout to 10-120 s (SeekBarPreference min/max).
    const timeout = Math.min(120, Math.max(10, parseInt(this.motionTimeout) || 15));
    this.motionTimeout = `${timeout}`;
    this.saveSetting('motionTimeout', timeout);
    this.motionRecordGate.rearm();
    this.motionEnabled = true;
    this.server.setMotion(true, timeout);
    this.startMotionTimer();
    this.appendLog(`motion detection ON (timeout=${timeout}s, arms in 10s, auto record MD/)`);
  }

  // Android timestamp shape for motion records: yyyy-MM-dd.HH.mm.ss.SSSS
  // (BaseServerActivity.F0 -> DCIM/IPCamera/MD/IPS_<date>.mp4).
  // Android clamps motion_timeout to 10-120 s (SeekBarPreference min/max).
  normalizeMotionTimeout(): void {
    const timeout = Math.min(120, Math.max(10, parseInt(this.motionTimeout) || 15));
    this.motionTimeout = `${timeout}`;
    this.saveSetting('motionTimeout', timeout);
    if (this.motionEnabled) this.server.setMotionTimeout(timeout);
  }

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
      this.syncRecordingState();
      if (this.server.motionActive()) {
        if (!this.episodeStarted) {
          this.episodeStarted = true;
          if (this.pageVisible) this.requestMotionSnapshot(++this.episodeGeneration);
        }
        this.onMotionStart();
      } else {
        if (this.episodeStarted) { this.episodeGeneration++; this.motionRecordGate.rearm(); }
        this.episodeStarted = false;
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
    if (!this.motionRecordGate.claim(this.pageVisible)) return;
    const dir = `${ctx.filesDir}/MD`;
    try {
      if (!fileIo.accessSync(dir)) fileIo.mkdirSync(dir, true);
    } catch (err) {
      this.reportProblem('motion: cannot create the MD folder');
      return;
    }
    if (!this.validRecordingVideo()) return;
    const stamp = this.motionTimestamp();
    const path = `${dir}/IPS_${stamp}.mp4`;
    const ticket = this.recorder.reserve();
    if (!ticket) return;
    this.recordTicket = ticket;
    // Never raise a permission prompt or open a microphone for a motion event.
    const ok: boolean = this.recorder.start(ticket, path, this.recordingOptions(this.micRunning && this.recordAudio !== 0));
    if (!ok) this.releaseRecording();
    this.motionRecording = ok;
    this.motionRecordPath = ok ? path : '';
    if (ok) this.appendLog(`motion record started (MD/IPS_${stamp}.mp4)`);
    else this.reportProblem(this.msg('srv_record_failed'));
  }

  onMotionStop(): void {
    this.syncRecordingState();
    if (!this.motionRecording) return;
    const name = this.motionRecordPath.substring(this.motionRecordPath.lastIndexOf('/') + 1);
    this.releaseRecording();
    this.motionRecording = false;
    this.motionRecordPath = '';
    this.appendLog(`motion record stopped (MD/${name})`);
  }

  // Leaving motion mode (server stop, page teardown, camera stop) closes the
  // auto recording instead of leaving the file open.
  stopMotionMode(reason: string): void {
    const wasEnabled = this.motionEnabled;
    this.episodeGeneration++;
    this.episodeStarted = false;
    this.motionEnabled = false;
    this.stopMotionTimer();
    try {
      this.onMotionStop();
    } finally {
      this.server.setMotion(false);
    }
    if (wasEnabled) this.appendLog(reason);
  }

  onStartPattern(): void {
    if (this.rejectBusyCameraChange()) return;
    if (!this.running) {
      this.appendLog('start the server first');
      return;
    }
    if (this.cameraRunning) {
      this.onStopCamera();
      this.appendLog('camera source replaced by test pattern');
    }
    const ok: boolean = this.server.startTestPattern(640, 480, 15, 1500000);
    this.patternRunning = ok;
    this.appendLog(ok ? 'test pattern (640x480@15 H.264) streaming'
                      : 'test pattern failed to start');
  }

  onStopPattern(): void {
    if (this.rejectBusyCameraChange()) return;
    this.server.stopTestPattern();
    this.patternRunning = false;
    this.appendLog('test pattern stopped');
  }

  async checkBootStart(): Promise<void> {
    try {
      const granted: boolean = await autoStartupManager.getAutoStartupStatusForSelf();
      if (this.pageActive) this.bootStatus = granted ? this.msg('srv_boot_ok') : this.msg('srv_boot_no');
    } catch (err) {
      if (this.pageActive) this.bootStatus = this.msg('srv_boot_unknown');
    }
  }

  // ---------- OSD (text overlay) ----------

  loadOsdSettings(): void {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
      this.osdFont = { fontSize: pref.getSync('osd_font_size', 0) as number,
        fontFamily: pref.getSync('text_font', '') as string, fontPath: pref.getSync('osd_font_path', '') as string };
      this.osdEnabled = pref.getSync('osdEnabled', false) as boolean;
      this.osdPositionIdx = pref.getSync('text_position', 0) as number;
      this.osdFontStyleIdx = pref.getSync('font_style', 0) as number;
      this.osdPadding = `${pref.getSync('osd_padding_int', 1) as number}`;
      this.osdColor = pref.getSync('text_color', '#FFFFFFFF') as string;
      this.osdColorSaved = osdColorValue(this.osdColor) || '#FFFFFFFF';
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
      this.segFourGb = pref.getSync('four_gb_limit', true) as boolean;
      this.segMinutes = `${pref.getSync('each_segment_length', 10) as number}`;
      this.autoRtsp = pref.getSync('auto_rtsp_after_turn_on', false) as boolean;
      this.autoRtmp = pref.getSync('auto_rtmp_after_turn_on', false) as boolean;
      this.keepScreenOn = pref.getSync('keepScreenOn', false) as boolean;
      this.camOnConnect = pref.getSync('start_camera_on_connect', true) as boolean;
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'loadOsdSettings failed');
    }
  }

  buildOsdConfig(): OsdConfig {
    const cfg: OsdConfig = {
      enabled: this.osdEnabled,
      padding: parseInt(this.osdPadding) || 0,
      position: this.osdPositionIdx,
      fontStyle: this.osdFontStyleIdx,
      fontSize: this.osdFont.fontSize, fontFamily: this.osdFont.fontFamily, fontPath: this.osdFont.fontPath,
      color: osdColorValue(this.osdColor) || this.osdColorSaved,
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

  // Only a user turning on the location overlay (or the overlay itself) may show
  // the permission dialog; other edits and page entry use an existing grant only.
  applyOsd(needCameraRestart: boolean, promptGps: boolean = false): void {
    this.server.setOsdConfig(this.buildOsdConfig());
    if (this.osdEnabled && this.osdShowBattery) {
      this.startBatteryTimer();
    } else {
      this.stopBatteryTimer();
    }
    if (this.osdShowGps && this.osdEnabled) {
      this.startGpsTimer(promptGps);
    } else {
      this.stopGpsTimer();
    }
    if (needCameraRestart && this.cameraRunning) {
      // enabling/disabling switches the camera between the direct encoder
      // surface and the GL interception pipeline
      this.requestCameraStart();
    }
  }

  onToggleWatermark(on: boolean): void {
    const restart = osdRestartRequired(this.osdEnabled, this.wmEnabled, this.osdEnabled, on);
    if (restart && this.cameraRunning && this.rejectBusyCameraChange()) return;
    this.wmEnabled = on; this.saveSetting('watermark_overlay', on); this.applyOsd(restart);
  }

  onToggleOsd(on: boolean): void {
    const restart = osdRestartRequired(this.osdEnabled, this.wmEnabled, on, this.wmEnabled);
    if (restart && this.cameraRunning && this.rejectBusyCameraChange()) return;
    this.osdEnabled = on;
    this.saveSetting('osdEnabled', on);
    this.applyOsd(restart, on);
    this.appendLog(on ? 'OSD overlay ON (text baked into the stream)'
                      : 'OSD overlay OFF');
  }

  pushBattery(): void {
    try {
      const level = parseInt(`${batteryInfo.batterySOC}`);
      this.server.setOsdBattery(isNaN(level) ? -1 : level, osdChargeSource(batteryInfo.chargingStatus, batteryInfo.pluggedType));
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

  async startGpsTimer(prompt: boolean = false): Promise<void> {
    if (this.gpsTimerId >= 0 || this.gpsPermissionPending) return;
    const ctx = getContext(this) as common.UIAbilityContext;
    const atManager = abilityAccessCtrl.createAtManager();
    const perms: Array<Permissions> = ['ohos.permission.APPROXIMATELY_LOCATION',
      'ohos.permission.LOCATION'];
    let alreadyGranted = false;
    try {
      // Approximate location is enough for the OSD position/speed readout.
      alreadyGranted = atManager.checkAccessTokenSync(ctx.applicationInfo.accessTokenId, perms[0]) === 0;
    } catch (_) {}
    // No grant and no explicit user action: show "GPS Waiting" without asking.
    if (!alreadyGranted && !prompt) return;
    const generation = ++this.gpsGeneration;
    if (alreadyGranted) {
      this.pollGps(generation);
      this.gpsTimerId = setInterval(() => this.pollGps(generation), 5000);
      return;
    }
    this.gpsPermissionPending = true;
    try {
      const result = await atManager.requestPermissionsFromUser(ctx, perms);
      if (generation !== this.gpsGeneration) return;
      let granted = result.authResults.length === perms.length;
      result.authResults.forEach((r: number) => {
        if (r !== 0) granted = false;
      });
      if (!granted) {
        this.appendLog('GPS permission denied (OSD shows "GPS Waiting")');
        return;
      }
    } catch (err) {
      if (this.pageActive) this.appendLog('GPS permission request failed');
      return;
    } finally {
      if (generation === this.gpsGeneration) this.gpsPermissionPending = false;
    }
    this.pollGps(generation);
    this.gpsTimerId = setInterval(() => this.pollGps(generation), 5000);
  }

  stopGpsTimer(): void {
    this.gpsGeneration++;
    this.gpsPermissionPending = false;
    if (this.gpsTimerId >= 0) {
      clearInterval(this.gpsTimerId);
      this.gpsTimerId = -1;
    }
    this.server.setOsdGps(0, 0, 0, false, this.osdSpeedMph);
  }

  async pollGps(generation: number): Promise<void> {
    try {
      const loc: geoLocationManager.Location =
        await geoLocationManager.getCurrentLocation();
      if (generation !== this.gpsGeneration) return;
      const speedKmh: number = (loc.speed ?? 0) * 3.6;
      this.server.setOsdGps(loc.latitude, loc.longitude, speedKmh, true, this.osdSpeedMph);
    } catch (err) {
      if (generation !== this.gpsGeneration) return;
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
      try { fileIo.copyFileSync(srcFile.fd, dest); } finally { fileIo.closeSync(srcFile); }
      this.saveSetting('watermark_src', dest);
      if (await this.decodeWatermark(dest)) this.appendLog('watermark loaded');
    } catch (err) {
      this.appendLog('watermark pick failed');
    }
  }

  async decodeWatermark(path: string): Promise<boolean> {
    const generation = ++this.watermarkGeneration;
    try {
      const src = image.createImageSource(path);
      try {
        const info = await src.getImageInfo();
        const scale = Math.min(1, 1280 / Math.max(info.size.width, info.size.height));
        const pm = await src.createPixelMap({ desiredPixelFormat: image.PixelMapFormat.RGBA_8888,
          desiredSize: { width: Math.max(1, Math.round(info.size.width * scale)),
            height: Math.max(1, Math.round(info.size.height * scale)) } });
        try {
          if (!this.pageActive || generation !== this.watermarkGeneration) return false;
          // Native copies the pixels. Subsequent text/style edits must not upload this map again.
          if (this.server.setOsdWatermark(pm)) return true;
        } finally { await pm.release(); }
      } finally { await src.release(); }
    } catch (_) {}
    if (this.pageActive) this.appendLog('watermark decode failed');
    return false;
  }

  refreshViewerCounts(): ViewerCounts {
    const counts: ViewerCounts = this.running ? this.server.viewerCounts() : NO_VIEWERS;
    this.viewerTotal = counts.mjpeg + counts.flv + counts.opus + counts.rtsp;
    return counts;
  }

  showViewerCounts(): void {
    // One native snapshot for both the title count and all four detail rows.
    const counts = this.refreshViewerCounts();
    this.getUIContext().showAlertDialog({ title: $r('app.string.svc_ui_clients', this.viewerTotal),
      message: `MJPEG: ${counts.mjpeg}
FLV: ${counts.flv}
OPUS: ${counts.opus}
RTSP: ${counts.rtsp}`,
      confirm: { value: $r('app.string.svc_ui_done'), action: () => {} } });
  }

  torchAvailable(): boolean {
    return this.running && this.cameraRunning && !this.cameraStarting && !this.patternRunning && this.dualLayout === 0;
  }

  togglePreviewTorch(): void {
    if (!this.torchAvailable()) return;
    if (!this.server.toggleTorch()) {
      this.getUIContext().getPromptAction().showToast({ message: $r('app.string.svc_ui_torch_failed') });
    }
  }

  openPanel(panel: ServicePanel): void {
    this.overflowOpen = false;
    this.settingsGroup = 'root';
    this.panel = panel;
    if (panel === 'share') this.shareWeb = false;
  }

  openPreference(group: PreferenceGroup): void {
    this.openPanel('settings');
    this.settingsGroup = group;
    if (group === 'camera') this.refreshCameraSizesIfGranted();
  }

  onBackPress(): boolean {
    if (this.overflowOpen) { this.overflowOpen = false; return true; }
    if (this.panel === 'settings' && this.settingsGroup !== 'root') {
      this.settingsGroup = 'root'; return true;
    }
    if (this.panel === 'settings' && this.enteredFromSettings) return false;
    if (this.panel !== 'preview') { this.openPanel('preview'); return true; }
    if (this.exitConfirmed) return false;
    // Android BaseServerActivity.onBackPressed: leaving stops every service, so confirm first.
    this.getUIContext().showAlertDialog({ title: $r('app.string.svc_ui_exit_title'),
      message: $r('app.string.svc_ui_exit_prompt'),
      primaryButton: { value: $r('app.string.srv_cancel'), action: () => {} },
      secondaryButton: { value: $r('app.string.svc_ui_exit'), action: () => {
        if (!this.pageActive) return;
        this.exitConfirmed = true;
        router.back();
      } } });
    return true;
  }

  panelTitle(): Resource {
    switch (this.panel) {
      case 'capture': return $r('app.string.srv_capture');
      case 'addresses': return $r('app.string.srv_addresses');
      case 'share': return $r('app.string.svc_ui_share');
      case 'diagnostics': return $r('app.string.svc_ui_diagnostics');
      case 'publish': return $r('app.string.srv_rtmp');
      default:
        switch (this.settingsGroup) {
          case 'camera': return $r('app.string.svc_ui_camera');
          case 'encoder': return $r('app.string.svc_ui_encoder');
          case 'audio': return $r('app.string.srv_audio');
          case 'record': return $r('app.string.svc_ui_format');
          case 'storage': return $r('app.string.svc_ui_storage');
          case 'overlay': return $r('app.string.srv_overlay');
          case 'network': return $r('app.string.srv_network');
          case 'startup': return $r('app.string.srv_startup');
          default: return $r('app.string.srv_settings');
        }
    }
  }

  @Builder
  heading(title: ResourceStr, hint: () => ResourceStr) {
    Column({ space: 3 }) {
      Text(title).fontSize(18).fontColor(INK).fontWeight(FontWeight.Bold)
      if (hint() !== '') { Text(hint()).fontSize(13).fontColor(MUTED) }
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  @Builder
  action(label: () => ResourceStr, enabled: () => boolean, run: () => void) {
    Button(label()).fontSize(14).fontColor(INK).backgroundColor('#494949')
      .margin({ right: 6, top: 4 }).enabled(enabled()).onClick(run)
  }

  @Builder
  choice(label: Resource, choices: ResourceStr[], idx: () => number, change: (index: number) => void) {
    Row({ space: 8 }) {
      Text(label).fontSize(15).fontColor(INK).layoutWeight(1)
      Select(toSelectOptions(choices)).constraintSize({ maxWidth: '58%' }).accessibilityText(label).selected(idx()).value(choices[idx()]).onSelect(change)
    }.width('100%')
  }

  @Builder
  toggle(label: Resource, value: () => boolean, change: (on: boolean) => void) {
    Row({ space: 8 }) {
      Text(label).fontSize(15).fontColor(INK).layoutWeight(1)
      Toggle({ type: ToggleType.Switch, isOn: value() }).selectedColor(ACCENT).accessibilityText(label).onChange(change)
    }.width('100%')
  }

  @Builder
  field(label: Resource, value: () => string, kind: InputType, change: (v: string) => void) {
    Row({ space: 8 }) {
      Text(label).fontSize(14).fontColor(INK).width('40%')
      TextInput({ text: value() }).layoutWeight(1).fontSize(14).type(kind).accessibilityText(label).enableAutoFill(false).onChange(change)
    }.width('100%')
  }

  @Builder
  capturePanel() {
    Column({ space: 8 }) {
      this.heading($r('app.string.srv_capture'), () => $r('app.string.srv_capture_hint'))
      Flex({ wrap: FlexWrap.Wrap }) {
        this.action(() => this.cameraStarting ? $r('app.string.srv_camera_starting') : (this.cameraRunning ? $r('app.string.srv_camera_stop') : $r('app.string.srv_camera_start')),
          () => this.running && !this.cameraStarting, () => {
            if (this.cameraRunning) this.onStopCamera(); else this.requestCameraStart();
          })
        this.action(() => this.micStarting ? $r('app.string.srv_mic_starting') : (this.micRunning ? $r('app.string.srv_mic_stop') : $r('app.string.srv_mic_start')),
          () => this.running && !this.micStarting && !this.recordStarting, () => {
            if (this.micRunning) this.onStopMic(); else this.onStartMic();
          })
        this.action(() => this.recordStarting ? $r('app.string.srv_cancel_record') : this.recording ? $r('app.string.srv_record_stop') : $r('app.string.srv_record_start'), () => this.recording || (this.running && !this.motionEnabled), () => this.onToggleRecord())
        this.action(() => $r('app.string.srv_snapshot'), () => this.running, () => this.onSnapshot())
        this.action(() => this.motionEnabled ? $r('app.string.srv_motion_stop') : $r('app.string.srv_motion_start'), () => !this.cameraStarting, () => this.onToggleMotion())
      }
      if (this.motionEnabled || this.recording) {
        Text(this.recording ? $r('app.string.srv_manual_active') : (this.motionRecording ? $r('app.string.srv_motion_recording') : $r('app.string.srv_motion_active')))
          .fontSize(13).fontColor(ACCENT)
      }
    }.width('100%').padding(16).backgroundColor(PAPER).alignItems(HorizontalAlign.Start)
  }

  @Builder
  addressPanel() {
    Column({ space: 8 }) {
      this.heading($r('app.string.srv_addresses'), () => $r('app.string.srv_addresses_hint'))
      Text(this.rtspUrl).fontSize(14).fontColor(INK).copyOption(CopyOptions.LocalDevice)
      this.action(() => $r('app.string.srv_copy_safe'), () => true, () => this.copyRtspAddress())
      this.action(() => $r('app.string.srv_copy_url'), () => true, () => this.copyRtspAddress(true))
      Divider()
      Text(this.httpRunning ? this.httpUrl : $r('app.string.srv_web_idle')).fontSize(14).fontColor(MUTED).copyOption(CopyOptions.LocalDevice)
      this.action(() => this.httpRunning ? $r('app.string.srv_web_stop') : $r('app.string.srv_web_start'), () => this.running, () => this.onToggleHttp())
    }.width('100%').padding(16).backgroundColor(PAPER).alignItems(HorizontalAlign.Start)
  }

  @Builder
  sharePanel() {
    Column({ space: 12 }) {
      Text($r('app.string.svc_ui_qr_hint')).fontSize(14).fontColor(MUTED)
      if (this.httpRunning) {
        this.choice($r('app.string.svc_ui_protocol'), ['RTSP', 'HTTP'], () => this.shareWeb ? 1 : 0,
          (idx: number) => { this.shareWeb = idx === 1; })
      }
      QRCode(this.shareWeb && this.httpRunning ? this.httpUrl : this.rtspUrl)
        .size({ width: 200, height: 200 }).constraintSize({ maxWidth: '100%' }).padding(12)
        .color('#000000').backgroundColor('#FFFFFF').accessibilityLevel('no')
      Text(this.shareWeb && this.httpRunning ? this.httpUrl : this.rtspUrl).fontColor(INK).fontSize(15)
        .copyOption(CopyOptions.LocalDevice).textAlign(TextAlign.Center)
    }.width('100%').padding(16)
  }

  @Builder
  publishPanel() {
    Column({ space: 8 }) {
      this.heading($r('app.string.srv_rtmp'), () => this.rtmpStatus)
      TextInput({ text: this.rtmpUrl, placeholder: $r('app.string.srv_rtmp_hint') }).fontSize(14)
        .type(InputType.Password).enableAutoFill(false).onChange((v: string) => {
          if (this.rtmpUrl !== v) this.onStopRtmp();
          this.rtmpUrl = v;
        })
      Flex({ wrap: FlexWrap.Wrap }) {
        this.action(() => this.rtmpState === 'idle' ? $r('app.string.srv_publish_start') :
          (this.rtmpState === 'publishing' ? $r('app.string.srv_publish_stop') : $r('app.string.srv_publish_cancel')), () => this.running, () => {
            if (this.rtmpState === 'idle') this.onStartRtmp(); else this.onStopRtmp();
          })
        this.action(() => $r('app.string.srv_manage_targets'), () => true, () => this.openRtmpAddresses())
      }
    }.width('100%').padding(16).backgroundColor(PAPER).alignItems(HorizontalAlign.Start)
  }

  @Builder
  diagnosticsPanel() {
    Column({ space: 8 }) {
      this.action(() => this.patternRunning ? $r('app.string.srv_pattern_stop') : $r('app.string.srv_pattern_start'),
        () => this.running && !this.cameraStarting, () => {
          if (this.patternRunning) this.onStopPattern(); else this.onStartPattern();
        })
      this.heading($r('app.string.srv_log'), () => $r('app.string.srv_log_hint'))
      ForEach(this.logs, (item: string) => {
        Text(item).fontSize(12).fontFamily('monospace').fontColor(MUTED)
      }, (item: string, idx: number) => `${idx}:${item}`)
    }.width('100%').padding(16).backgroundColor(PAPER).alignItems(HorizontalAlign.Start)
  }

  @Builder
  category(title: Resource) {
    Text(title).fontSize(14).fontWeight(FontWeight.Bold).fontColor(ACCENT)
      .width('100%').padding({ left: 16, top: 16, bottom: 6 })
  }

  @Builder
  preference(title: Resource, summary: ResourceStr, run: () => void) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 10 }) {
        Column({ space: 2 }) {
          Text(title).fontSize(18).fontColor(INK)
          if (summary !== '') { Text(summary).fontSize(14).fontColor(MUTED) }
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      }.width('100%').padding({ left: 16, right: 16, top: 14, bottom: 14 })
    }.width('100%').height('auto').constraintSize({ minHeight: 48 }).padding(0)
      .borderRadius(0).backgroundColor(PAPER).accessibilityText(title).onClick(run)
    Divider().color(ANDROID_DIVIDER)
  }

  @Builder
  settingsHome() {
    this.preference($r('app.string.srv_cloud'), $r('app.string.svc_ui_cloud_hint'), () => router.pushUrl({ url: 'pages/CloudArchive' }))
    this.preference($r('app.string.svc_ui_format'), this.recordVideo === 0 ? $r('app.string.srv_shared') : CODECS[this.recordVideo - 1],
      () => this.openPreference('record'))
    this.preference($r('app.string.svc_ui_storage'), $r('app.string.srv_download_hint'), () => this.openPreference('storage'))
    this.category($r('app.string.svc_ui_motion_category'))
    this.motionPreferences()
    this.category($r('app.string.svc_ui_viewer'))
    this.preference($r('app.string.srv_view_live'), '', () => router.pushUrl({ url: 'pages/Player' }))
    this.preference($r('app.string.srv_playback'), '', () => router.pushUrl({ url: 'pages/Playback' }))
    this.preference($r('app.string.srv_onvif'), '', () => router.pushUrl({ url: 'pages/OnvifScan' }))
    this.category($r('app.string.svc_ui_server'))
    this.preference($r('app.string.svc_ui_camera'), $r('app.string.svc_ui_camera_hint'), () => this.openPreference('camera'))
    this.preference($r('app.string.svc_ui_encoder'), CODECS[this.codecIdx], () => this.openPreference('encoder'))
    this.preference($r('app.string.srv_overlay'), $r('app.string.svc_ui_overlay_hint'), () => this.openPreference('overlay'))
    this.preference($r('app.string.srv_audio'), '', () => this.openPreference('audio'))
    this.preference($r('app.string.srv_manage_targets'), '', () => this.openRtmpAddresses())
    this.preference($r('app.string.srv_startup'), '', () => this.openPreference('startup'))
    this.preference($r('app.string.srv_network'), '', () => this.openPreference('network'))
    this.preference($r('app.string.srv_lan'), $r('app.string.svc_ui_lan_hint'), () => router.pushUrl({ url: 'pages/LanServiceSettings' }))
    this.category($r('app.string.srv_external'))
    this.preference($r('app.string.srv_delivery'), '', () => router.pushUrl({ url: 'pages/Delivery' }))
  }

  @Builder
  audioSettings() {
    Column({ space: 10 }) {
      this.heading($r('app.string.srv_audio'), () => $r('app.string.srv_audio_hint'))
      MicProfilePicker({ sampleRate: this.micRate, channels: this.micChannels, running: this.micRunning || this.micStarting,
        actualRate: this.actualMicRate, actualChannels: this.actualMicChannels,
        onSampleRate: (v: number) => { this.micRate = v; this.saveSetting('micRate', v); },
        onChannels: (v: number) => { this.micChannels = v; this.saveSetting('micChannels', v); } })
    }.width('100%').padding(16).backgroundColor(PAPER)
  }

  @Builder
  videoSettings() {
    Column({ space: 10 }) {
      this.heading($r('app.string.svc_ui_camera'), () => $r('app.string.svc_ui_camera_hint'))
      this.choice($r('app.string.srv_facing'), FACING, () => this.facingIdx, (idx: number) => this.selectFacing(idx))
      CameraProfilePicker({ resolutions: this.resolutions, selectedWidth: this.cameraWidth,
        selectedHeight: this.cameraHeight, cameraRunning: this.cameraRunning, fpsInfo: this.fpsInfo,
        onResolution: (size: CameraResolution) => this.selectResolution(size),
        onCaptureFps: (range: CaptureFpsRange) => this.selectCaptureFps(range), devices: this.devices,
        deviceId: this.deviceId, focusInfo: this.focusInfo, onDevice: (v: CameraDeviceOption) => this.selectDevice(v),
        onFocusMode: (v: number) => this.setFocusMode(v), onFocusCenter: () => this.focusCenter() })
      if (this.dualAvailable || this.dualLayout !== 0) {
        this.choice($r('app.string.srv_dual'), [$r('app.string.srv_single'), $r('app.string.srv_pip'),
          $r('app.string.srv_side'), $r('app.string.srv_stack')], () => this.dualLayout,
          (idx: number) => this.setDualLayout(idx))
      }
      this.choice($r('app.string.srv_rotation'), [$r('app.string.srv_auto'), '0°', '90°', '180°', '270°'],
        () => [-1,0,90,180,270].indexOf(this.captureRotation), (idx: number) => {
          this.captureRotation = [-1,0,90,180,270][idx]; this.saveSetting('captureRotation', this.captureRotation);
        })
      this.toggle($r('app.string.srv_mirror'), () => this.captureMirror, (on: boolean) => {
        this.captureMirror = on; this.saveSetting('captureMirror', on);
      })
      Text($r('app.string.srv_orientation_hint')).fontSize(13).fontColor(MUTED)
    }.width('100%').padding(16).backgroundColor(PAPER).enabled(!this.cameraStarting)
  }

  @Builder
  encoderSettings() {
    Column({ space: 10 }) {
      this.heading($r('app.string.svc_ui_encoder'), () => $r('app.string.srv_video_hint'))
      this.choice($r('app.string.srv_codec'), CODECS, () => this.codecIdx, (idx: number) => this.selectCodec(idx))
      CameraProfilePicker({ encoderOnly: true, cameraRunning: this.cameraRunning, encoders: this.encoders,
        encoderName: this.encoderName, avcProfile: this.avcProfile, onEncoder: (v: EncoderOption) => this.selectEncoder(v),
        onAvcProfile: (v: number) => this.selectProfile(v) })
      this.field($r('app.string.srv_bitrate'), () => this.videoBitrate, InputType.Number, (v: string) => {
        this.videoBitrate = v; this.saveTuning();
      })
      this.field($r('app.string.srv_key_interval'), () => this.keyframeInterval, InputType.Number, (v: string) => {
        this.keyframeInterval = v; this.saveTuning();
      })
    }.width('100%').padding(16).backgroundColor(PAPER).enabled(!this.cameraStarting)
  }

  @Builder
  networkSettings() {
    Column({ space: 10 }) {
      this.heading($r('app.string.srv_network'), () => $r('app.string.srv_network_hint'))
      this.field($r('app.string.srv_rtsp_port'), () => this.port, InputType.Number, (v: string) => {
        this.port = v;
        const port = settingsPort(v);
        this.fieldNotice(port > 0, 'srv_port_invalid');
        if (port) { this.saveSetting('port', port); this.refreshUrl(); }
      })
      this.field($r('app.string.srv_http_port'), () => this.httpPort, InputType.Number, (v: string) => {
        this.httpPort = v;
        const port = settingsPort(v);
        this.fieldNotice(port > 0, 'srv_port_invalid');
        if (port) this.saveSetting('httpPort', port);
      })
      this.field($r('app.string.srv_path'), () => this.streamPath, InputType.Normal, (v: string) => {
        this.streamPath = v; this.saveSetting('path', v); this.refreshUrl();
      })
      this.field($r('app.string.srv_user'), () => this.user, InputType.Normal, (v: string) => { this.user = v; this.saveSetting('user', v); })
      this.field($r('app.string.srv_password'), () => this.password, InputType.Password, (v: string) => { this.password = v; this.saveSetting('password', v); })
      Divider()
      Text($r('app.string.srv_legacy_hint')).fontSize(13).fontColor(MUTED)
      this.action(() => this.allowLegacyVideoPassword ? $r('app.string.srv_legacy_disable') : $r('app.string.srv_legacy_enable'),
        () => true, () => this.configureLegacyVideoPassword())
    }.width('100%').padding(16).backgroundColor(PAPER)
  }

  @Builder
  recordSettings() {
    Column({ space: 10 }) {
      this.heading($r('app.string.svc_ui_format'), () => $r('app.string.srv_record_hint'))
      this.choice($r('app.string.srv_record_video'), [$r('app.string.srv_shared'), 'H.264 / AVC', 'H.265 / HEVC'],
        () => this.recordVideo, (idx: number) => { this.recordVideo = idx; this.saveSetting('recordVideoMode', idx); })
      if (this.recordVideo !== 0) {
        Text($r('app.string.srv_reencode_hint')).fontSize(13).fontColor(MUTED)
        this.field($r('app.string.srv_rec_bitrate'), () => this.recordBitrateKbps, InputType.Number, (v: string) => {
          this.recordBitrateKbps = v; this.saveSetting('recordBitrateKbps', v);
        })
        this.field($r('app.string.srv_rec_gop'), () => this.recordGopMs, InputType.Number, (v: string) => {
          this.recordGopMs = v; this.saveSetting('recordGopMs', v);
        })
      }
      this.choice($r('app.string.srv_record_audio'), [$r('app.string.srv_silent'), $r('app.string.srv_open_mic'),
        $r('app.string.srv_record_mic')], () => this.recordAudio,
        (idx: number) => { this.recordAudio = idx; this.saveSetting('recordAudioMode', idx); })
      Text($r('app.string.srv_record_audio_hint')).fontSize(13).fontColor(MUTED)
    }.width('100%').padding(16).backgroundColor(PAPER)
  }

  @Builder
  motionPreferences() {
    Column({ space: 12 }) {
      this.toggle($r('app.string.srv_limit'), () => this.segFourGb, (on: boolean) => {
        this.segFourGb = on; this.saveSetting('four_gb_limit', on);
      })
      this.field($r('app.string.srv_segment'), () => this.segMinutes, InputType.Number, (v: string) => {
        // Android: up to 4 digits. An empty or non-numeric field keeps the saved length.
        this.segMinutes = v;
        if (/^[0-9]{1,4}$/.test(v.trim())) this.saveSetting('each_segment_length', Number(v.trim()));
      })
      Row({ space: 8 }) {
        Text($r('app.string.srv_motion_hold')).fontSize(14).fontColor(INK).layoutWeight(1)
        TextInput({ text: this.motionTimeout }).width('40%').type(InputType.Number).accessibilityText($r('app.string.srv_motion_hold'))
          .onChange((v: string) => { this.motionTimeout = v; })
          .onSubmit(() => this.normalizeMotionTimeout()).onBlur(() => this.normalizeMotionTimeout())
      }.width('100%')
    }.width('100%').padding(16)
  }

  @Builder
  storageSettings() {
    Column({ space: 10 }) {
      Text($r('app.string.srv_download_hint')).fontSize(13).fontColor(MUTED)
      this.action(() => this.copyDownloads ? $r('app.string.srv_download_off') : $r('app.string.srv_download_on'),
        () => !this.authorizingDownload, () => this.toggleDownloadCopies())
    }.width('100%').padding(16).backgroundColor(PAPER)
  }

  @Builder
  startupSettings() {
    Column({ space: 10 }) {
      this.heading($r('app.string.srv_startup'), () => $r('app.string.srv_startup_hint'))
      this.toggle($r('app.string.srv_auto_rtsp'), () => this.autoRtsp, (on: boolean) => {
        this.autoRtsp = on; this.saveSetting('auto_rtsp_after_turn_on', on);
      })
      this.toggle($r('app.string.srv_auto_rtmp'), () => this.autoRtmp, (on: boolean) => {
        this.autoRtmp = on; this.saveSetting('auto_rtmp_after_turn_on', on);
      })
      this.toggle($r('app.string.srv_on_connect'), () => this.camOnConnect, (on: boolean) => {
        this.camOnConnect = on; this.saveSetting('start_camera_on_connect', on);
      })
      this.toggle($r('app.string.srv_keep_screen'), () => this.keepScreenOn, (on: boolean) => {
        this.keepScreenOn = on; this.saveSetting('keepScreenOn', on); this.updateKeepScreenOn();
      })
      Divider()
      Text($r('app.string.srv_boot_state', this.bootStatus)).fontSize(14).fontColor(MUTED)
      this.action(() => $r('app.string.srv_boot_query'), () => true, () => this.checkBootStart())
    }.width('100%').padding(16).backgroundColor(PAPER).alignItems(HorizontalAlign.Start)
  }

  @Builder
  overlaySettings() {
    Column({ space: 10 }) {
      this.heading($r('app.string.srv_overlay'), () => $r('app.string.srv_ov_hint'))
      this.toggle($r('app.string.srv_text_on'), () => this.osdEnabled, (on: boolean) => this.onToggleOsd(on))
      this.choice($r('app.string.srv_text_pos'), OSD_POSITIONS, () => this.osdPositionIdx, (idx: number) => {
        this.osdPositionIdx = idx; this.saveSetting('text_position', idx); this.applyOsd(false);
      })
      this.choice($r('app.string.srv_style'), OSD_FONT_STYLES, () => this.osdFontStyleIdx, (idx: number) => {
        this.osdFontStyleIdx = idx; this.saveSetting('font_style', idx); this.applyOsd(false);
      })
      OsdFontSettings({ font: this.osdFont, onChange: (f: OsdFontOptions) => {
        this.osdFont = f; this.saveSetting('osd_font_size', f.fontSize); this.saveSetting('text_font', f.fontFamily);
        this.saveSetting('osd_font_path', f.fontPath); this.applyOsd(false);
      } })
      this.field($r('app.string.srv_text_pad'), () => this.osdPadding, InputType.Number, (v: string) => {
        this.osdPadding = v; this.saveSetting('osd_padding_int', parseInt(v) || 0); this.applyOsd(false);
      })
      this.field($r('app.string.srv_color'), () => this.osdColor, InputType.Normal, (v: string) => {
        this.osdColor = v;  // Keep the user's text while typing; save only a complete color.
        const color = osdColorValue(v);
        this.fieldNotice(!!color, 'srv_color_invalid');
        if (color) { this.osdColorSaved = color; this.saveSetting('text_color', color); this.applyOsd(false); }
      })
      this.field($r('app.string.srv_custom'), () => this.osdCustomText, InputType.Normal, (v: string) => {
        this.osdCustomText = v; this.saveSetting('text_custom', v); this.applyOsd(false);
      })
      this.toggle($r('app.string.srv_time'), () => this.osdShowTimestamp, (on: boolean) => {
        this.osdShowTimestamp = on; this.saveSetting('display_timestamp', on); this.applyOsd(false);
      })
      this.toggle($r('app.string.srv_device'), () => this.osdShowDevName, (on: boolean) => {
        this.osdShowDevName = on; this.saveSetting('display_dev_name', on); this.applyOsd(false);
      })
      this.toggle($r('app.string.srv_battery'), () => this.osdShowBattery, (on: boolean) => {
        this.osdShowBattery = on; this.saveSetting('display_battery_info', on); this.applyOsd(false);
      })
      this.toggle($r('app.string.srv_location'), () => this.osdShowGps, (on: boolean) => {
        this.osdShowGps = on; this.saveSetting('display_gps_location', on); this.applyOsd(false, on);
      })
      this.choice($r('app.string.srv_speed'), ['km/h', 'mph'], () => this.osdSpeedUnitIdx, (idx: number) => {
        this.osdSpeedUnitIdx = idx; this.osdSpeedMph = idx === 1; this.saveSetting('speed_unit', idx); this.applyOsd(false);
      })
      Divider()
      this.toggle($r('app.string.srv_wm_on'), () => this.wmEnabled, (on: boolean) => this.onToggleWatermark(on))
      this.choice($r('app.string.srv_wm_pos'), OSD_POSITIONS, () => this.wmPositionIdx, (idx: number) => {
        this.wmPositionIdx = idx; this.saveSetting('watermark_position', idx); this.applyOsd(false);
      })
      this.field($r('app.string.srv_wm_pad'), () => this.wmPadding, InputType.Number, (v: string) => {
        this.wmPadding = v; this.saveSetting('watermark_padding_int', parseInt(v) || 0); this.applyOsd(false);
      })
      this.field($r('app.string.srv_wm_area'), () => this.wmMaxArea, InputType.Number, (v: string) => {
        this.wmMaxArea = v; this.saveSetting('watermark_max_area_occupied_int', parseInt(v) || 10); this.applyOsd(false);
      })
      this.action(() => $r('app.string.srv_wm_pick'), () => true, () => this.onPickWatermark())
    }.width('100%').padding(16).backgroundColor(PAPER).alignItems(HorizontalAlign.Start)
  }

  @Builder
  toolIcon(icon: Resource, label: () => Resource, run: () => void, enabled: () => boolean = () => true) {
    Button({ type: ButtonType.Normal }) {
      Image(icon).width(32).height(32).objectFit(ImageFit.Contain).accessibilityLevel('no')
    }.layoutWeight(1).constraintSize({ maxWidth: 52 }).height(48).padding(0).backgroundColor(Color.Transparent)
      .accessibilityText(label()).enabled(enabled()).onClick(run)
  }

  @Builder
  bottomButton(label: () => ResourceStr, spokenLabel: () => Resource, run: () => void) {
    Button(label(), { type: ButtonType.Normal }).layoutWeight(1).height(44).padding(2)
      .fontSize(14).fontColor('#EF9513').backgroundColor('#99000000').accessibilityText(spokenLabel()).onClick(run)
  }

  @Builder
  previewChrome() {
    Column() {
      Row() {
        Button('‹', { type: ButtonType.Normal }).width(44).height(48).padding(0).fontSize(28)
          .backgroundColor(Color.Transparent).fontColor('#FFFFFF').accessibilityText($r('app.string.svc_ui_back'))
          .onClick(() => { if (!this.onBackPress()) router.back(); })
        Blank().layoutWeight(1)
        this.toolIcon($r('app.media.svc_ui_switch_webcam'), () => $r('app.string.svc_ui_switch_lens'),
          () => this.selectFacing(1 - this.facingIdx), () => !this.cameraStarting && this.dualLayout === 0)
        this.toolIcon($r('app.media.svc_ui_motion_detection'), () => this.motionEnabled ? $r('app.string.srv_motion_stop') : $r('app.string.srv_motion_start'),
          () => this.onToggleMotion(), () => !this.cameraStarting)
        this.toolIcon($r('app.media.svc_ui_flash_on'), () => $r('app.string.svc_ui_torch'),
          () => this.togglePreviewTorch(), () => this.torchAvailable())
        this.toolIcon($r('app.media.svc_ui_imagesize'), () => $r('app.string.svc_ui_size_fps'), () => this.openPreference('camera'))
        Button('⋮', { type: ButtonType.Normal }).width(44).height(48).padding(0).fontSize(24)
          .fontColor('#FFFFFF').backgroundColor(Color.Transparent).accessibilityText($r('app.string.svc_ui_more'))
          .onClick(() => { this.overflowOpen = true; })
      }.width('100%').backgroundColor('#99000000')
      if (this.motionEnabled || this.recording) {
        Text(this.recording ? $r('app.string.srv_manual_active') : this.motionRecording ?
          $r('app.string.srv_motion_recording') : $r('app.string.srv_motion_active'))
          .alignSelf(ItemAlign.Start).padding(4).fontSize(14).fontColor('#FF4081').backgroundColor('#CC000000')
      }
      Column({ space: 6 }) {
        if (!this.cameraRunning) {
          Text(this.patternRunning ? $r('app.string.srv_pattern_preview') : $r('app.string.srv_no_preview'))
            .fontColor('#FFFFFF').fontSize(16).textAlign(TextAlign.Center)
          if (this.running && !this.patternRunning) {
            Button(this.cameraStarting ? $r('app.string.srv_camera_starting') : $r('app.string.srv_camera_start'))
              .enabled(!this.cameraStarting).onClick(() => this.requestCameraStart())
          }
        }
      }.layoutWeight(1).width('100%').justifyContent(FlexAlign.Center).clip(true)
      Scroll() {
        Column({ space: 2 }) {
          Row({ space: 8 }) {
            Text(this.running ? $r('app.string.srv_rtsp_active') : $r('app.string.srv_rtsp_idle'))
              .layoutWeight(1).fontSize(15).fontColor('#FFFFFF')
            Button(this.running ? $r('app.string.srv_stop') : $r('app.string.srv_start')).fontSize(14)
              .backgroundColor('#CC000000').fontColor('#EF9513')
              .onClick(() => { if (this.running) this.onStopServer(); else this.onStartServer(); })
          }.width('100%').padding({ left: 8, right: 8 })
          Text(this.rtspUrl).fontColor('#FFFFFF').fontSize(14).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          if (this.httpRunning) {
            Text(this.httpUrl).fontColor('#FFFFFF').fontSize(14).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          if (this.serviceNotice !== '') { Text(this.serviceNotice).fontColor('#FFFFFF').fontSize(13).padding(4).backgroundColor('#9A352D') }
          Row() {
            this.bottomButton(() => this.rtmpState === 'idle' ? 'RTMP' : $r('app.string.svc_ui_rtmp_active'),
              () => $r('app.string.srv_rtmp'), () => this.openPanel('publish'))
            this.bottomButton(() => $r('app.string.svc_ui_connect'), () => $r('app.string.srv_addresses'), () => this.openPanel('addresses'))
            this.bottomButton(() => $r('app.string.svc_ui_share'), () => $r('app.string.svc_ui_share'), () => this.openPanel('share'))
            this.bottomButton(() => $r('app.string.svc_ui_clients', this.viewerTotal),
              () => $r('app.string.srv_clients', this.viewerTotal), () => this.showViewerCounts())
          }.width('100%')
        }.width('100%').padding({ top: 4 }).backgroundColor('#66000000')
      }.width('100%').constraintSize({ maxWidth: 560, maxHeight: '50%' }).scrollBar(BarState.Auto)
    }.width('100%').height('100%').backgroundColor(this.cameraRunning ? Color.Transparent : '#000000').enabled(!this.overflowOpen)
      .accessibilityLevel(this.overflowOpen ? 'no-hide-descendants' : 'auto')
  }

  @Builder
  overflowMenu() {
    Stack({ alignContent: Alignment.TopEnd }) {
      Column().width('100%').height('100%').backgroundColor('#44000000').accessibilityLevel('no')
        .onClick(() => { this.overflowOpen = false; })
      Scroll() {
        Column() {
          this.preference($r('app.string.svc_ui_close_menu'), '', () => { this.overflowOpen = false; })
          this.preference($r('app.string.srv_capture'), '', () => this.openPanel('capture'))
          this.preference($r('app.string.svc_ui_size_fps'), '', () => this.openPreference('camera'))
          this.preference($r('app.string.srv_playback'), '', () => {
            this.overflowOpen = false; router.pushUrl({ url: 'pages/Playback' });
          })
          this.preference($r('app.string.svc_ui_diagnostics'), '', () => this.openPanel('diagnostics'))
          this.preference($r('app.string.srv_settings'), '', () => this.openPanel('settings'))
        }.width('100%').backgroundColor(PAPER)
      }.width('86%').constraintSize({ maxWidth: 340, maxHeight: '100%' }).scrollBar(BarState.Auto)
    }.width('100%').height('100%')
  }

  @Builder
  panelLayer() {
    Column() {
      Row({ space: 8 }) {
        Button('‹', { type: ButtonType.Normal }).fontSize(28).width(48).height(48).padding(0)
          .fontColor('#FFFFFF').backgroundColor(Color.Transparent).accessibilityText($r('app.string.svc_ui_back'))
          .onClick(() => { if (!this.onBackPress()) router.back(); })
        Text(this.panelTitle()).fontSize(19).fontWeight(FontWeight.Medium).fontColor('#FFFFFF').layoutWeight(1)
      }.width('100%').padding({ right: 12 }).constraintSize({ minHeight: ANDROID_TOOLBAR_HEIGHT }).backgroundColor(ANDROID_BAR)
      if (this.panel === 'settings' && this.settingsGroup === 'root') {
        Scroll() {
          Column() { this.settingsHome() }.width('100%').constraintSize({ maxWidth: 720 })
        }.width('100%').layoutWeight(1).align(Alignment.Top).scrollBar(BarState.Auto)
      } else {
        Scroll() {
          Column() {
            if (this.serviceNotice !== '') { Text(this.serviceNotice).width('100%').padding(12).fontColor(INK).backgroundColor('#87352B') }
            if (this.panel === 'addresses') { this.addressPanel() }
            else if (this.panel === 'share') { this.sharePanel() }
            else if (this.panel === 'capture') { this.capturePanel() }
            else if (this.panel === 'publish') { this.publishPanel() }
            else if (this.panel === 'diagnostics') { this.diagnosticsPanel() }
            else if (this.settingsGroup === 'camera') { this.videoSettings() }
            else if (this.settingsGroup === 'encoder') { this.encoderSettings() }
            else if (this.settingsGroup === 'overlay') { this.overlaySettings() }
            else if (this.settingsGroup === 'network') { this.networkSettings() }
            else if (this.settingsGroup === 'storage') { this.storageSettings() }
            else if (this.settingsGroup === 'record') { this.recordSettings() }
            else if (this.settingsGroup === 'audio') { this.audioSettings() }
            else if (this.settingsGroup === 'startup') { this.startupSettings() }
          }.width('100%').constraintSize({ maxWidth: 720 })
        }.layoutWeight(1).width('100%').align(Alignment.Top).scrollBar(BarState.Auto)
      }
    }.width('100%').height('100%').backgroundColor(PAPER)
  }

  build() {
    Stack() {
      // Native preview ownership belongs to this page, never to a panel, menu or scroll cell.
      XComponent({ id: 'cameraPreview', type: 'surface', libraryname: 'streaming' })
        .width('100%').height('100%').renderFit(RenderFit.RESIZE_CONTAIN).backgroundColor('#000000').accessibilityLevel('no')
      if (this.panel === 'preview') { this.previewChrome() } else { this.panelLayer() }
      if (this.overflowOpen) { this.overflowMenu() }
    }.width('100%').height('100%').backgroundColor('#000000')
  }
}
