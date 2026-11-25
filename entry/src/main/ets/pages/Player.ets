import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { http } from '@kit.NetworkKit';
import { image } from '@kit.ImageKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { PiPWindow, window } from '@kit.ArkUI';
import streaming from 'libstreaming.so';
import { liveEndpoint, LiveEndpoint } from '../util/LiveEndpoint';
import { mjpegBoundary, MjpegFrames } from '../util/MjpegFrames';

import { media } from '@kit.MediaKit';
import { ViewerVoice } from '../util/ViewerVoice';
import { ANDROID_BG, ANDROID_BAR, ANDROID_TEXT, ANDROID_SECONDARY, ANDROID_DIVIDER, ANDROID_ACCENT, ANDROID_TOOLBAR_HEIGHT } from '../components/AndroidTheme';
import { ViewerPipSession, ViewerPipSurface, ViewerPipPhase, ViewerPipEnd, viewerPipEligible } from '../util/ViewerPipSession';
import { LiveFlvLoader } from '../util/LiveFlvLoader';

import { readDevice } from '../util/DeviceStore';
import { ViewerReconnect, ViewerFailure, httpViewerFailure, httpViewerRequestFailure, rtspViewerFailure } from '../util/ViewerReconnect';

interface PlayerParams { url?: string; deviceId?: string }

function localized(ctx: common.UIAbilityContext | undefined, key: string, fallback: string, arg: string = ''): string {
  const template = ctx ? ctx.resourceManager.getStringByNameSync('viewer_' + key) : fallback;
  return template.replace('{0}', arg);
}

function videoLabel(mime: string, ctx?: common.UIAbilityContext): string {
  if (mime === 'video/hevc') return 'H.265';
  if (mime === 'video/avc') return 'H.264';
  return localized(ctx, 'video', '视频');
}

function rtspFailure(code: number, mime: string, ctx?: common.UIAbilityContext): string {
  switch (code) {
    case -2: return localized(ctx, 'rtsp_auth', 'RTSP 鉴权失败，请检查用户名和密码');
    case -3: return mime ? localized(ctx, 'limits', '{0} 码流参数超出播放器支持范围', videoLabel(mime, ctx)) : localized(ctx, 'tracks', '未找到兼容的 H.264 或 H.265 视频轨');
    case -4: return localized(ctx, 'rtp_tcp', '摄像机不支持 RTP over TCP');
    case -5: return localized(ctx, 'rtsp_session', 'RTSP 会话无效或摄像机拒绝请求');
    case -6: return localized(ctx, 'codec', '当前设备无法解码此{0}画面', videoLabel(mime, ctx));
    case -7: return localized(ctx, 'frame_timeout', '等待视频帧超时');
    case -8: return localized(ctx, 'rtsp_digest', '设备的鉴权挑战与所选模式不兼容；不会切换鉴权方式');
    default: return localized(ctx, 'rtsp_error', 'RTSP 连接失败或连接已断开');
  }
}

@Entry
@Component
struct Player {
  @State endpointLabel: string = '';
  @State draft: string = '';
  @State user: string = '';
  @State password: string = '';
  @State status: string = '';
  @State active: boolean = false;
  @State failed: boolean = false;
  @State rtsp: boolean = false;
  @State rtmp: boolean = false;
  @State digest: boolean = false;
  @State udp: boolean = false;
  @State rendered: boolean = false;
  @State pipPhase: ViewerPipPhase = 'idle';
  @State pipEnd: ViewerPipEnd = '';
  private decoderSurface: string = '';
  private pip?: ViewerPipSession;
  private restoreTimer: number = -1;
  @State autoReconnect: boolean = false;
  @State waiting: boolean = false;
  private foreground: boolean = false;
  private route?: PlayerParams;
  private reconnect: ViewerReconnect = new ViewerReconnect();
  private attempt: number = 0;
  private mjpegConfirmed: boolean = false;
  @State audioState: number = -1;
  @State frame?: image.PixelMap = undefined;
  @State systemFormat: string = '';
  @State selectedHttp: boolean = false;
  @State adts: boolean = false;
  @State voicePhase: string = 'idle';
  @State voiceMessage: string = '';
  private selected?: LiveEndpoint;
  private av?: media.AVPlayer;
  private flv?: LiveFlvLoader;
  private avWork: Promise<void> = Promise.resolve();
  private avPlaying: boolean = false;
  private voice?: ViewerVoice;
  private address: string = '';
  private controller: XComponentController = new XComponentController();
  private surfaceId: string = '';
  private avPending?: LiveEndpoint;
  private epoch: number = 0;
  private req?: http.HttpRequest;
  private nativeHandle?: object;
  private timer: number = -1;
  private pending?: LiveEndpoint;
  private lastFrame: number = 0;
  private decoding: boolean = false;
  @State fullscreen: boolean = false;
  @State controlsVisible: boolean = true;
  @State panel: 'none' | 'menu' | 'settings' | 'info' = 'none';
  private barQueue: Promise<void> = Promise.resolve();
  private fullWindow?: window.Window;
  private previousLayout: boolean = false;
  private previousStatus: boolean = true;
  private previousNav: boolean = true;

  setFullscreen(enable: boolean): void {
    if (this.fullscreen === enable) return;
    if (enable) this.cancelVoice();
    this.panel = 'none';
    this.controlsVisible = !enable;
    this.fullscreen = enable;
    const ctx = getContext(this) as common.UIAbilityContext;
    // Serialize changes so leaving while getLastWindow() is pending still restores the old state.
    this.barQueue = this.barQueue.then(async () => {
      if (enable) {
        if (!this.fullscreen) return;
        const win = await window.getLastWindow(ctx);
        if (!this.fullscreen) return;
        this.previousLayout = win.getWindowProperties().isLayoutFullScreen;
        this.previousStatus = win.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM).visible;
        this.previousNav = win.getWindowAvoidArea(window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR).visible;
        this.fullWindow = win;
        await win.setWindowLayoutFullScreen(true).catch(() => {});
        await win.setSpecificSystemBarEnabled('status', false).catch(() => {});
        await win.setSpecificSystemBarEnabled('navigationIndicator', false).catch(() => {});
      } else if (this.fullWindow) {
        const win = this.fullWindow;
        this.fullWindow = undefined;
        await win.setSpecificSystemBarEnabled('status', this.previousStatus).catch(() => {});
        await win.setSpecificSystemBarEnabled('navigationIndicator', this.previousNav).catch(() => {});
        await win.setWindowLayoutFullScreen(this.previousLayout).catch(() => {});
      }
    }).catch(() => {});
  }

  onBackPress(): boolean {
    if (this.panel !== 'none') {
      this.panel = 'none';
      return true;
    }
    if (this.fullscreen) {
      this.setFullscreen(false);
      return true;
    }
    if (!this.controlsVisible) {
      this.controlsVisible = true;
      return true;
    }
    this.stop(); // Router back is a departure, not a PiP background transition.
    return false;
  }

  toolbarBack(): void {
    if (!this.onBackPress()) this.getUIContext().getRouter().back();
  }

  showPanel(panel: 'menu' | 'settings' | 'info'): void {
    this.cancelVoice();
    this.controlsVisible = true;
    this.panel = panel;
  }

  tapVideo(): void {
    if (this.panel !== 'none') this.panel = 'none';
    else if (this.active) {
      if (this.controlsVisible) this.cancelVoice();
      this.controlsVisible = !this.controlsVisible;
    }
  }

  private confirmAuth: boolean = false;
  connectFromControls(): void {
    if (this.confirmAuth || !this.endpointLabel) this.showPanel('settings');
    else this.play();
  }

  private ctx?: common.UIAbilityContext;

  text(key: string, fallback: string, arg: string = ''): string { return localized(this.ctx, key, fallback, arg); }

  aboutToAppear(): void {
    this.ctx = getContext(this) as common.UIAbilityContext;
    this.status = this.text('initial', '点击播放以连接');
    const params = this.getUIContext().getRouter().getParams() as PlayerParams;
    if (params && (typeof params.deviceId === 'string' || typeof params.url === 'string')) {
      this.route = params;
      return;
    }
    // Direct entry still defaults to this camera. Loopback also works without Wi-Fi.
    try {
      const pref = preferences.getPreferencesSync(getContext(this) as common.UIAbilityContext, { name: 'ipcamSettings' });
      const port = pref.getSync('httpPort', 8081) as number;
      const name = pref.getSync('user', 'admin') as string;
      const pw = pref.getSync('password', 'admin') as string;
      this.address = `http://${encodeURIComponent(name)}:${encodeURIComponent(pw)}@127.0.0.1:${port}/video`;
      this.select(liveEndpoint(this.address));
    } catch (_) { this.status = this.text('enter', '请输入播放地址'); }
  }

  clearRestoreTimer(): void {
    if (this.restoreTimer >= 0) clearTimeout(this.restoreTimer);
    this.restoreTimer = -1;
  }

  pipSurface(): ViewerPipSurface | undefined {
    if (!this.rendered || !this.decoderSurface) return undefined;
    const gen = this.epoch, id = this.decoderSurface;
    return { controller: this.controller, surfaceId: id, isRendering: () =>
      this.active && this.rendered && this.epoch === gen && this.surfaceId === id && this.decoderSurface === id };
  }

  canOfferPip(): boolean {
    if (this.pipPhase !== 'idle' || !this.rendered || !viewerPipEligible(this.pipSurface())) return false;
    try {
      return canIUse('SystemCapability.Window.SessionManager') &&
        canIUse('SystemCapability.WindowManager.WindowManager.Core') && PiPWindow.isPiPEnabled();
    } catch (_) { return false; }
  }

  startPip(): void {
    if (!this.ctx || !this.foreground || this.pipPhase !== 'idle') return;
    const surface = this.pipSurface();
    if (!surface || !viewerPipEligible(surface)) return;
    this.cancelVoice();
    if (!this.pip) this.pip = new ViewerPipSession((phase: ViewerPipPhase, end: ViewerPipEnd) => this.pipChanged(phase, end));
    this.pip.start(this.ctx, surface);
  }

  pipChanged(phase: ViewerPipPhase, end: ViewerPipEnd): void {
    this.pipPhase = phase;
    this.pipEnd = end;
    if (phase !== 'idle' || !this.active) return;
    if (end === 'restored' && !this.foreground) {
      // STOPPED can arrive before onPageShow. Keep this same decoder briefly; never reopen one.
      this.clearRestoreTimer();
      const gen = this.epoch;
      const timer = setTimeout(() => {
        if (this.restoreTimer === timer && gen === this.epoch && !this.foreground) this.stop();
      }, 2000);
      this.restoreTimer = timer;
    } else if (!this.foreground || end === 'closed' || end === 'interrupted') this.stop();
  }

  onPageShow(): void {
    this.clearRestoreTimer();
    this.foreground = true;
    this.reconnect.setForeground(true);
    const route = this.route;
    this.route = undefined;
    if (!route) return;
    const gen = this.epoch;
    if (route.deviceId) {
      readDevice(route.deviceId).then(draft => {
        if (!this.foreground || gen !== this.epoch) return;
        this.openRoute(draft.url);
      }).catch(() => this.fail(gen, this.text('invalid', '地址或凭据无效，请检查后重试')));
    } else if (route.url) this.openRoute(route.url);
  }

  openRoute(address: string): void {
    this.address = address;
    // A saved password is not consent to send preemptive Basic before selecting Digest.
    if (/^rtsp:\/\/[^/?#]*@/i.test(address)) {
      try {
        this.select(liveEndpoint(address));
        this.confirmAuth = true;
        this.showPanel('settings');
        this.status = this.text('choose_auth', '请先选择 RTSP 鉴权方式（Basic 或 Digest），再点击播放');
      }
      catch (_) { this.fail(this.epoch, this.text('invalid', '地址或凭据无效，请检查后重试')); }
    } else this.play();
  }

  onPageHide(): void {
    this.foreground = false;
    this.panel = 'none';
    this.controlsVisible = true;
    this.route = undefined;
    this.reconnect.setForeground(false);
    this.setFullscreen(false);
    this.cancelVoice();
    if (!this.pip?.keepsSurface()) this.stop();
  }

  aboutToDisappear(): void {
    this.foreground = false;
    this.panel = 'none';
    this.controlsVisible = true;
    this.route = undefined;
    this.reconnect.setForeground(false);
    this.setFullscreen(false);
    this.pip?.dispose();
    this.stop();
  }

  stop(): void {
    this.reconnect.cancel();
    if (this.waiting) this.status = this.text('stopped', '已停止');
    this.waiting = false;
    this.releaseAttempt();
  }

  releaseAttempt(): void {
    ++this.epoch;
    this.clearRestoreTimer();
    this.pip?.stop();
    this.rendered = false;
    this.decoderSurface = '';
    this.mjpegConfirmed = false;
    this.cancelVoice();
    this.avPending = undefined;
    this.flv?.dispose();
    this.flv = undefined;
    this.avPlaying = false;
    this.systemFormat = '';
    this.avWork = this.avWork.then(async () => {
      const player = this.av;
      this.av = undefined;
      if (player) await player.release().catch(() => {});
    });
    this.pending = undefined;
    if (this.timer >= 0) clearInterval(this.timer);
    this.timer = -1;
    if (this.req) this.req.destroy();
    this.req = undefined;
    if (this.nativeHandle) {
      if (this.rtmp) streaming.rtmpViewerStop(this.nativeHandle);
      else streaming.liveAvcStop(this.nativeHandle);
    }
    this.rtmp = false;
    this.rtsp = false;
    this.nativeHandle = undefined;
    this.audioState = -1;
    this.decoding = false;
    const old = this.frame;
    this.frame = undefined;
    if (old) old.release().catch(() => {});
    if (this.active) this.status = this.text('stopped', '已停止');
    this.active = false;
    this.controlsVisible = true;
  }

  fail(gen: number, message: string, reason: ViewerFailure = 'fatal'): void {
    if (gen !== this.epoch) return;
    const delay = this.reconnect.failed(this.attempt, reason);
    this.setFullscreen(false);
    this.releaseAttempt();
    this.waiting = delay > 0;
    this.failed = true;
    this.status = delay ? this.text('reconnecting', '{0} 秒后重连', String(delay / 1000)) : message;
  }

  play(): void {
    if (!this.foreground) return;
    this.failed = false;
    this.confirmAuth = false;
    this.panel = 'none';
    this.controlsVisible = true;
    if (this.draft.trim()) {
      this.address = this.draft.trim();
      this.draft = '';
      this.endpointLabel = '';
    }
    try {
      const target = liveEndpoint(this.address, this.user, this.password, this.digest);
      this.select(target);
      this.reconnect.start((id: number) => this.openAttempt(target, id), this.autoReconnect);
    } catch (_) {
      this.select();
      const message = /^rtmps:/i.test(this.address) ? this.text('rtmps', '不支持 RTMPS，不会降级为明文') :
        /^rtmp:/i.test(this.address) ? this.text('rtmp', 'RTMP 仅接受 rtmp://主机/应用/流，不支持用户名、密码或 Digest') :
        this.digest ? this.text('digest_login', 'Digest 仅用于 RTSP，需要用户名和密码') : this.text('invalid', '地址或凭据无效，请检查后重试');
      this.fail(this.epoch, message);
    }
  }

  openAttempt(target: LiveEndpoint, id: number): void {
    if (!this.foreground || !this.reconnect.current(id)) return;
    this.attempt = id;
    // Never bind a replacement source while the old PiP window may still own this surface.
    if (this.pip && this.pipPhase !== 'idle') {
      this.waiting = true; // Stop remains available while the replacement waits for the surface.
      this.pip.stop().then(() => {
        if (!this.foreground || !this.reconnect.current(id)) return;
        if (this.pipPhase === 'idle') this.openAttempt(target, id);
        else {
          this.reconnect.cancel();
          this.waiting = false;
          this.failed = true;
          this.status = this.text('pip_busy', '请先退出画中画，再播放其他视频');
        }
      });
      return;
    }
    const gen = this.epoch;
    this.waiting = false;
    this.failed = false;
    try {
      this.active = true;
      this.status = this.text('connecting', '正在连接…');
      this.lastFrame = Date.now();
      this.rtsp = target.scheme === 'rtsp' || target.scheme === 'rtsps';
      if (target.scheme === 'rtsps') { this.fail(gen, this.text('rtsps', '此版本不支持 RTSPS，不会降级为明文')); return; }
      this.rtmp = target.scheme === 'rtmp';
      if (this.rtsp || this.rtmp) {
        this.pending = target;
        this.queueNative();
      } else if (/\.(m3u8|flv)$/i.test(target.address.split('?')[0])) {
        // AVPlayer controls redirects, playlists and subrequests. Never supply it Basic credentials
        // or a potentially signed query, even if the top-level URL happens to be same-origin.
        if ((target.token && !/\.flv$/i.test(target.address)) || target.address.includes('?')) { this.fail(gen, this.text('public_url', '系统媒体播放仅接受无凭据、无查询参数的公开地址')); return; }
        this.systemFormat = /\.flv$/i.test(target.address) ? 'HTTP-FLV' : 'HLS';
        this.avPending = target;
        this.startSystem();
      } else {
        this.startHttp(gen, target);
      }
      if (gen === this.epoch) this.timer = setInterval(() => this.poll(gen), 350);
    } catch (_) { this.fail(gen, this.text('invalid', '地址或凭据无效，请检查后重试')); }
  }

  editAddress(value: string): void {
    if (value === this.draft) return;
    this.select();
    this.address = '';
    this.user = '';
    this.password = '';
    this.draft = value;
  }

  select(target?: LiveEndpoint): void {
    this.stop();
    this.selected = target;
    this.selectedHttp = !!target && (target.scheme === 'http' || target.scheme === 'https');
    this.endpointLabel = target ? target.origin : '';
  }

  openArchives(): void {
    const origin = this.selectedHttp && this.selected ? this.selected.origin : '';
    this.stop();
    const params: PlayerParams = {};
    if (origin) params.url = origin;
    this.getUIContext().getRouter().pushUrl({ url: 'pages/RemoteArchives', params: params })
      .catch(() => { this.status = this.text('archives_error', '无法打开远端录像'); });
  }

  cancelVoice(): void {
    if (this.voice) this.voice.cancel();
    this.voice = undefined;
    this.voicePhase = 'idle';
    this.voiceMessage = '';
  }

  beginVoice(): void {
    if (!this.foreground || this.pipPhase !== 'idle' || !this.selected || !this.selectedHttp || this.draft || this.fullscreen || this.voice) return;
    const voice = new ViewerVoice(this.selected, this.adts, (phase: string, text: string) => {
      if (this.voice !== voice) return;
      this.voicePhase = phase;
      this.voiceMessage = text;
      if (phase === 'done') this.voice = undefined;
    });
    this.voice = voice;
    voice.start(getContext(this) as common.UIAbilityContext);
  }

  startSystem(): void {
    const dest = this.avPending, gen = this.epoch, surface = this.surfaceId, format = this.systemFormat;
    if (!dest || !surface || !this.active) return;
    this.avPending = undefined;
    const error = (): void => this.fail(gen, this.text('system_error', '{0} 播放失败，请检查流格式、网络和系统解码能力', format));
    this.avWork = this.avWork.then(async () => {
      if (gen !== this.epoch) return;
      try {
        const player = await media.createAVPlayer();
        this.av = player;
        if (gen !== this.epoch) return; // stop() has already queued release
        player.on('error', error);
        player.on('startRenderFrame', () => {
          if (gen === this.epoch && this.av === player && this.decoderSurface === surface && this.surfaceId === surface) this.rendered = true;
        });
        player.on('stateChange', (state: string) => {
          if (gen !== this.epoch) return;
          try {
            if (state === 'initialized') {
              player.surfaceId = surface;
              this.decoderSurface = surface;
              player.prepare().catch(error);
            } else if (state === 'prepared') {
              player.setVolume(0);
              this.audioState = 0;
              player.play().catch(error);
            } else if (state === 'playing') {
              this.avPlaying = true;
              this.status = this.text('playing', '正在播放 · ') + format;
            } else if (state === 'completed') {
              this.stop();
              this.setFullscreen(false);
              this.status = this.text('completed', '播放已结束');
            }
          } catch (_) { error(); }
        });
        // Do not serialize release behind an unresponsive network operation.
        const source = media.createMediaSourceWithUrl(dest.address);
        if (format === 'HTTP-FLV' && dest.token) {
          this.flv = new LiveFlvLoader(dest, (code: number) => {
            if (code === 401 || code === 403) this.fail(gen, this.text('http_auth', 'HTTP {0}：鉴权失败', String(code)));
            else if (code >= 300 && code < 400) this.fail(gen, this.text('redirect', '为保护凭据，已阻止 HTTP 重定向'));
            else error();
          });
          source.setMediaResourceLoaderDelegate(this.flv);
        }
        player.setMediaSource(source).catch(error);
      } catch (_) { error(); }
    });
  }

  surfaceLost(): void {
    this.surfaceId = '';
    if (this.active && (this.rtsp || this.rtmp || this.systemFormat)) this.stop();
  }

  queueNative(): void {
    const gen = this.epoch;
    if (!this.pending || !this.surfaceId) return;
    this.avWork.then(() => { if (this.epoch === gen) this.startNative(); });
  }

  startNative(): void {
    const dest = this.pending;
    if (!dest || !this.active || !this.surfaceId || !this.foreground) return;
    this.pending = undefined;
    try {
      if (this.rtmp) this.nativeHandle = streaming.rtmpViewerStart(dest.address, this.surfaceId);
      else if (this.udp) {
        if (dest.digest) this.nativeHandle = streaming.liveAvcStartDigestUdp(dest.address, dest.digest.user, dest.digest.password, this.surfaceId);
        else this.nativeHandle = streaming.liveAvcStartUdp(dest.address, dest.token, this.surfaceId);
      }
      else if (dest.digest) this.nativeHandle = streaming.liveAvcStartDigest(dest.address, dest.digest.user, dest.digest.password, this.surfaceId, true);
      else this.nativeHandle = streaming.liveAvcStart(dest.address, dest.token, this.surfaceId, true);
      this.decoderSurface = this.surfaceId;
    } catch (_) {
      this.fail(this.epoch, this.rtmp ? this.text('rtmp_start', '无法启动 RTMP 播放器') :
        this.udp ? this.text('udp_unavailable', 'RTSP UDP 单播不可用，不会回退到 TCP') : this.text('rtsp_start', '无法启动 RTSP 播放器'));
    }
  }

  nativeAudio(): number {
    if (!this.nativeHandle) return -1;
    return this.rtmp ? streaming.rtmpViewerAudioStatus(this.nativeHandle) : streaming.liveAvcAudioStatus(this.nativeHandle);
  }

  toggleAudio(): void {
    if (this.av && this.systemFormat && this.audioState >= 0) {
      const next = this.audioState === 0 ? 1 : 0;
      try { this.av.setVolume(next); this.audioState = next; } catch (_) {}
      return;
    }
    if (!this.nativeHandle || this.audioState < 0 || (this.rtsp && this.udp)) return;
    if (this.rtmp) streaming.rtmpViewerSetAudio(this.nativeHandle, this.audioState === 0);
    else streaming.liveAvcSetAudio(this.nativeHandle, this.audioState === 0);
    this.audioState = this.nativeAudio();
  }

  poll(gen: number): void {
    if (gen !== this.epoch) return;
    if (this.nativeHandle) {
      const code = this.rtmp ? streaming.rtmpViewerStatus(this.nativeHandle) : streaming.liveAvcStatus(this.nativeHandle);
      if (code < 0) {
        if (this.rtmp) this.fail(gen, this.rtmpFailure(code));
        else if (this.udp && code === -4) this.fail(gen, this.text('udp_unavailable', 'RTSP UDP 单播不可用，不会回退到 TCP'));
        else this.fail(gen, rtspFailure(code, streaming.liveVideoCodec(this.nativeHandle), this.ctx), rtspViewerFailure(code));
        return;
      }
      if (code === 1) {
        this.rendered = this.decoderSurface !== '' && this.surfaceId === this.decoderSurface;
        if (!this.rtmp) this.reconnect.playing(this.attempt);
        if (this.rtsp && this.udp) {
          this.audioState = -1;
          this.status = this.text('playing', '正在播放 · ') + videoLabel(streaming.liveVideoCodec(this.nativeHandle), this.ctx) +
            ' · ' + this.text('udp_only', 'UDP 单播 · 仅视频（无 AAC）');
          return;
        }
        this.audioState = this.nativeAudio();
        const audio = this.audioState === -3 ? this.text('rtmp_audio', '音轨不支持（仅 AAC-LC）') : this.audioState === -2 ? this.text('audio_unavailable', '音频输出不可用') : this.audioState === -1 ? this.text('no_audio', '无兼容音轨') :
          this.audioState === 0 ? this.text('muted', '已静音') : this.audioState === 1 ? this.text('waiting_audio', '等待 AAC 音频') : this.text('listening', 'AAC 监听中');
        const codec = this.rtmp ? 'RTMP · H.264' : videoLabel(streaming.liveVideoCodec(this.nativeHandle), this.ctx);
        this.status = this.text('playing', '正在播放 · ') + codec + ' · ' + audio;
        return;
      }
    }
    if (this.avPlaying) return;
    if (Date.now() - this.lastFrame > 15000) this.fail(gen, this.text('timeout', '等待可播放的视频帧超时'),
      this.mjpegConfirmed ? 'disconnect' : 'fatal');
  }

  rtmpFailure(code: number): string {
    switch (code) {
      case -1: return this.text('rtmp_network', 'RTMP 连接失败或已断开');
      case -2: return this.text('rtmp_denied', 'RTMP 服务器拒绝播放');
      case -3: return this.text('rtmp_codec', '不支持的 RTMP 视频编码或 AVC 参数');
      case -4: return this.text('rtmp_transport', '不支持的 RTMP 传输或握手');
      case -6: return this.text('rtmp_decode', 'RTMP 解码器或显示表面不可用');
      case -7: return this.text('frame_timeout', '等待视频帧超时');
      case -8: return this.text('rtmp_enhanced', '不支持增强 RTMP 视频');
      case -9: return this.text('rtmp_end', 'RTMP 流已结束');
      case -10: return this.text('stopped', '已停止');
      case -11: return this.text('rtmp_busy', 'RTMP 播放器忙');
      case -12: return this.text('rtmp_hevc', '不支持 RTMP HEVC，请使用 AVC');
      case -13: return this.text('rtmp_missing', '未找到 RTMP 流');
      case -14: return this.text('rtmp_aggregate', '不支持 RTMP 聚合消息');
      default: return this.text('rtmp_protocol', 'RTMP 地址或协议无效');
    }
  }

  startHttp(gen: number, dest: LiveEndpoint): void {
    const request = http.createHttp();
    this.req = request;
    let frames: MjpegFrames | undefined = undefined;
    request.on('headersReceive', (obj: Object) => {
      if (gen !== this.epoch) return;
      const fields = obj as Record<string, string>;
      for (const key of Object.keys(fields)) {
        if (key.toLowerCase() === 'content-type') {
          const boundary = mjpegBoundary(String(fields[key]));
          frames = boundary ? new MjpegFrames(boundary) : undefined;
        }
      }
    });
    request.on('dataReceive', (data: ArrayBuffer) => {
      if (!frames || gen !== this.epoch) return;
      try { frames.feed(new Uint8Array(data), (jpeg: ArrayBuffer) => { this.decode(gen, jpeg); }); }
      catch (_) { this.fail(gen, this.text('mjpeg_invalid', '无效的 MJPEG 分帧或图像过大')); }
    });
    let ended = false;
    let responseCode = 0;
    const finish = (): void => {
      if (ended && responseCode === 200) this.fail(gen, frames ? this.text('mjpeg_end', 'MJPEG 视频流已结束') : this.text('not_mjpeg', '不是 MJPEG 视频流；不会打开网页'), frames ? 'disconnect' : 'fatal');
    };
    request.on('dataEnd', () => { ended = true; finish(); });
    const headers: Record<string, string> = { 'Accept': 'multipart/x-mixed-replace' };
    if (dest.token) headers['Authorization'] = 'Basic ' + dest.token;
    request.requestInStream(dest.address, { method: http.RequestMethod.GET, header: headers, usingCache: false,
      maxRedirects: 0, usingProxy: false, connectTimeout: 6000, readTimeout: 0 }).then((code: number) => {
      if (code === 401 || code === 403) this.fail(gen, this.text('http_auth', 'HTTP {0}：鉴权失败，请检查用户名和密码或地址中的令牌', String(code)));
      else if (code >= 300 && code < 400) this.fail(gen, this.text('redirect', '为保护凭据，已阻止 HTTP 重定向'));
      else if (code !== 200) this.fail(gen, this.text('http_error', 'HTTP 请求失败（{0}）', String(code)), httpViewerFailure(code));
      else { responseCode = code; finish(); }
    }).catch((err: BusinessError) => {
      this.fail(gen, err.code === 2300047 ? this.text('redirect', '为保护凭据，已阻止 HTTP 重定向') : this.text('http_network', 'HTTP 连接失败，请检查地址、网络或 TLS 证书'), httpViewerRequestFailure(err.code));
    });
  }

  async decode(gen: number, jpeg: ArrayBuffer): Promise<void> {
    if (gen !== this.epoch || this.decoding) return;
    this.decoding = true;
    let source: image.ImageSource | undefined = undefined;
    let decoded: image.PixelMap | undefined = undefined;
    try {
      source = image.createImageSource(jpeg);
      const info = await source.getImageInfo();
      const w = info.size.width, h = info.size.height;
      if (w < 1 || h < 1 || w > 8192 || h > 8192 || w * h > 16000000) throw new Error('image limit');
      if (gen !== this.epoch) return;
      const scale = Math.min(1, 1280 / w, 1280 / h);
      decoded = await source.createPixelMap({ desiredSize: { width: Math.max(1, Math.floor(w * scale)),
        height: Math.max(1, Math.floor(h * scale)) } });
      if (gen !== this.epoch) return;
      const old = this.frame;
      this.frame = decoded;
      decoded = undefined;
      if (old) old.release().catch(() => {});
      this.lastFrame = Date.now();
      this.mjpegConfirmed = true;
      this.reconnect.playing(this.attempt);
      this.status = this.text('mjpeg_playing', '正在播放 · MJPEG');
    } catch (_) { this.fail(gen, this.text('jpeg_decode', 'JPEG 无法解码或尺寸过大')); }
    finally {
      if (decoded) decoded.release().catch(() => {});
      if (source) source.release().catch(() => {});
      if (gen === this.epoch) this.decoding = false;
    }
  }

  build() {
    Stack() {
      // This surface never moves into a conditional, sheet, or fullscreen replica.
      XComponent({ id: 'liveRtspPlayback', type: 'surface', controller: this.controller })
        .renderFit(RenderFit.RESIZE_CONTAIN)
        .onLoad(() => { this.surfaceId = this.controller.getXComponentSurfaceId(); this.queueNative(); this.startSystem(); })
        .onDestroy(() => this.surfaceLost())
        .visibility((this.rtsp || this.rtmp || this.systemFormat) && this.active ? Visibility.Visible : Visibility.Hidden)
        .width('100%').height('100%')
      if (this.frame) { Image(this.frame).objectFit(ImageFit.Contain).width('100%').height('100%') }
      Column() {
        if (this.controlsVisible) { this.viewerToolbar() }
        Column({ space: 12 }) {
          if (this.active && !this.rendered && !this.frame) { LoadingProgress().width(36).height(36).color(ANDROID_ACCENT) }
          if (!this.active && this.panel === 'none') {
            Text(this.status).fontColor(ANDROID_TEXT).textAlign(TextAlign.Center)
            if (!this.waiting) {
              Button(this.endpointLabel ? (this.failed ? $r('app.string.viewer_retry') : $r('app.string.viewer_play')) :
                $r('app.string.view_ui_connection')).backgroundColor(ANDROID_ACCENT).fontColor(ANDROID_BAR)
                .onClick(() => this.connectFromControls())
              if (this.endpointLabel) {
                this.action($r('app.string.view_ui_connection'), () => this.showPanel('settings'))
              }
            }
          }
        }.width('100%').layoutWeight(1).justifyContent(FlexAlign.Center).padding(12).onClick(() => this.tapVideo())
        if (this.controlsVisible && (this.active || this.waiting || this.selectedHttp)) { this.mediaControls() }
      }.width('100%').height('100%')
      if (this.panel !== 'none') {
        Stack({ alignContent: this.panel === 'menu' ? Alignment.TopEnd : Alignment.Center }) {
          Column().width('100%').height('100%').backgroundColor('#99000000').onClick(() => { this.panel = 'none'; })
          if (this.panel === 'menu') { this.moreMenu() }
          else if (this.panel === 'settings') { this.settings() }
          else {
            Column({ space: 12 }) {
              Text($r('app.string.view_ui_info')).fontSize(18).fontColor(ANDROID_TEXT)
              if (this.endpointLabel) { Text(this.endpointLabel).fontColor(ANDROID_SECONDARY) }
              Text(this.status).fontColor(ANDROID_TEXT)
              this.action($r('app.string.view_ui_close'), () => { this.panel = 'none'; })
            }.padding(20).width('90%').backgroundColor(ANDROID_BG).borderRadius(8)
          }
        }.width('100%').height('100%')
      }
    }.width('100%').height('100%').backgroundColor('#000000')
  }

  @Builder
  action(label: ResourceStr, click: () => void) {
    Button(label).height(44).fontSize(14).fontColor(ANDROID_TEXT).backgroundColor(Color.Transparent).onClick(click)
  }

  @Builder
  viewerToolbar() {
    Row() {
      Button('‹').width(48).height(48).fontSize(30).fontColor(ANDROID_TEXT).backgroundColor(Color.Transparent)
        .accessibilityText(getContext(this).resourceManager.getStringSync($r('app.string.view_ui_back').id)).onClick(() => this.toolbarBack())
      Column() {
        Text($r('app.string.viewer_title')).fontColor(ANDROID_TEXT).fontWeight(FontWeight.Medium).fontSize(18)
        if (this.endpointLabel) { Text(this.endpointLabel).fontColor(ANDROID_SECONDARY).fontSize(12).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis }) }
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      if (this.selectedHttp && !this.draft) { this.action($r('app.string.viewer_archives'), () => this.openArchives()) }
      Button('⋮').width(48).height(48).fontColor(ANDROID_TEXT).fontSize(24).backgroundColor(Color.Transparent)
        .accessibilityText(getContext(this).resourceManager.getStringSync($r('app.string.view_ui_more').id)).onClick(() => this.showPanel('menu'))
    }.height(ANDROID_TOOLBAR_HEIGHT).width('100%').backgroundColor(ANDROID_BAR)
  }

  @Builder
  moreMenu() {
    Scroll() {
      Column() {
        this.action($r('app.string.view_ui_connection'), () => this.showPanel('settings'))
        this.action($r('app.string.view_ui_info'), () => this.showPanel('info'))
        if (this.pipPhase === 'starting' || this.pipPhase === 'active' || this.pipPhase === 'stopFailed') {
          this.action($r('app.string.pip_exit'), () => { this.pip?.stop(); })
        } else if (this.canOfferPip()) {
          this.action($r('app.string.pip_enter'), () => this.startPip())
        }
        if (this.pipPhase === 'stopFailed') { Text($r('app.string.pip_stop_failed')).fontColor(ANDROID_SECONDARY) }
        else if (this.pipPhase === 'active') { Text($r('app.string.pip_external')).fontColor(ANDROID_SECONDARY) }
        else if (this.pipPhase !== 'idle') { Text($r('app.string.pip_transition')).fontColor(ANDROID_SECONDARY) }
        else if (this.pipEnd === 'unavailable' || this.pipEnd === 'error') { Text($r('app.string.pip_unavailable')).fontColor(ANDROID_SECONDARY) }
        else if (this.pipEnd === 'surfaceLost') { Text($r('app.string.pip_surface_lost')).fontColor(ANDROID_SECONDARY) }
        else if (this.pipEnd === 'interrupted') { Text($r('app.string.pip_interrupted')).fontColor(ANDROID_SECONDARY) }
      }.width('100%').padding(8).alignItems(HorizontalAlign.Start)
    }.width(240).constraintSize({ maxHeight: '90%' }).backgroundColor(ANDROID_BG).borderRadius(6).margin(8)
  }

  @Builder
  settings() {
    Scroll() {
      Column({ space: 8 }) {
        Row() {
          Text($r('app.string.view_ui_connection')).layoutWeight(1).fontSize(18).fontColor(ANDROID_TEXT)
          this.action($r('app.string.view_ui_close'), () => { this.panel = 'none'; })
        }.width('100%')
        if (this.endpointLabel) { Text(this.endpointLabel).fontSize(13).fontColor(ANDROID_SECONDARY) }
        TextInput({ text: this.draft, placeholder: $r('app.string.viewer_address') })
          .type(InputType.Password).showPasswordIcon(false).copyOption(CopyOptions.None).enableAutoFill(false).maxLength(4096).width('100%')
          .onChange((value: string) => this.editAddress(value))
        Row({ space: 8 }) {
          TextInput({ text: this.user, placeholder: $r('app.string.viewer_user') }).layoutWeight(1)
            .onChange((v: string) => { this.select(); this.user = v; })
          TextInput({ text: this.password, placeholder: $r('app.string.viewer_password') }).layoutWeight(1)
            .type(InputType.Password).showPasswordIcon(false).copyOption(CopyOptions.None).enableAutoFill(false)
            .onChange((v: string) => { this.select(); this.password = v; })
        }.width('100%')
        if (this.digest || /^rtsp:\/\//i.test(this.draft || this.address)) {
          Button(this.digest ? $r('app.string.viewer_rtsp_digest_mode') : $r('app.string.viewer_rtsp_basic_mode'))
            .onClick(() => { this.select(); this.digest = !this.digest; })
        }
        if (/^rtsp:\/\//i.test(this.draft || this.address)) {
          Button(this.udp ? $r('app.string.viewer_udp_only') : $r('app.string.viewer_tcp_transport'))
            .onClick(() => { this.stop(); this.udp = !this.udp; })
          Text($r('app.string.viewer_udp_hint')).fontSize(12).fontColor(ANDROID_SECONDARY).width('100%')
        }
        Row({ space: 8 }) {
          Toggle({ type: ToggleType.Switch, isOn: this.autoReconnect }).onChange((enabled: boolean) => {
            this.autoReconnect = enabled;
            this.reconnect.setEnabled(enabled);
            if (!enabled && this.waiting) this.stop();
          })
          Text($r('app.string.viewer_retry_option')).fontSize(13).fontColor(ANDROID_TEXT)
        }
        Text(this.status).fontSize(13).fontColor(ANDROID_TEXT)
        Button(this.failed ? $r('app.string.viewer_retry') : $r('app.string.viewer_play'))
          .enabled(!!this.draft.trim() || !!this.address).backgroundColor(ANDROID_ACCENT).fontColor(ANDROID_BAR)
          .onClick(() => this.play())
        Text($r('app.string.viewer_help')).fontSize(12).fontColor(ANDROID_SECONDARY)
      }.width('100%').padding(16)
    }.width('94%').constraintSize({ maxHeight: '94%' }).backgroundColor(ANDROID_BG).borderRadius(8)
  }

  @Builder
  mediaControls() {
    Column({ space: 2 }) {
      if (this.active) { Text(this.status).fontSize(12).fontColor(ANDROID_SECONDARY).width('100%') }
      Divider().color(ANDROID_DIVIDER)
      if (this.active || this.waiting || this.fullscreen) {
        Row({ space: 2 }) {
          if (this.active || this.waiting) { this.action($r('app.string.viewer_stop'), () => { this.setFullscreen(false); this.stop(); }) }
          if (this.active && this.audioState >= 0 && !(this.rtsp && this.udp)) {
            this.action(this.audioState === 0 ? $r('app.string.viewer_unmute') : $r('app.string.viewer_mute'), () => this.toggleAudio())
          }
          Blank()
          if (this.pipPhase === 'idle' && (this.active || this.fullscreen)) {
            this.action(this.fullscreen ? $r('app.string.viewer_exit_full') : $r('app.string.viewer_fullscreen'), () => this.setFullscreen(!this.fullscreen))
          }
        }.width('100%')
      }
      if (this.selectedHttp && !this.draft && this.pipPhase === 'idle' && !this.fullscreen) {
        Row({ space: 6 }) {
          if (this.voicePhase === 'idle' || this.voicePhase === 'done') {
            this.action(this.adts ? $r('app.string.viewer_adts') : $r('app.string.viewer_m4a'), () => { this.adts = !this.adts; })
            this.action($r('app.string.viewer_record_voice'), () => this.beginVoice())
          } else {
            Button($r('app.string.viewer_send_voice')).fontSize(14).enabled(this.voicePhase === 'recording' || this.voicePhase === 'ready')
              .onClick(() => { if (this.voice) this.voice.send(); })
            this.action($r('app.string.viewer_cancel'), () => this.cancelVoice())
          }
        }
        if (this.voiceMessage) { Text(this.voiceMessage).fontSize(12).fontColor(ANDROID_TEXT) }
      }
    }.width('100%').padding({ left: 10, right: 10, top: 6, bottom: 6 }).backgroundColor('#D9212121')
  }
}
