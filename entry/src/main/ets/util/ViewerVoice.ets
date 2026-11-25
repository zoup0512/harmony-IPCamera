import { abilityAccessCtrl, common } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { fileIo } from '@kit.CoreFileKit';
import { media } from '@kit.MediaKit';
import { http } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { LiveEndpoint } from './LiveEndpoint';

// One user gesture, one immutable HTTP origin, one clip. Never retry with a different
// container, downgrade HTTPS or send on timeout. A new page/device cannot inherit this clip.
export class ViewerVoice {
  private phase: string = 'idle';
  private cancelled: boolean = false;
  private recorder?: media.AVRecorder;
  private file?: fileIo.File;
  private req?: http.HttpRequest;
  private work: Promise<void> = Promise.resolve();
  private ctx?: common.UIAbilityContext;
  private text(key: string, arg: string = ''): string {
    return this.ctx!.resourceManager.getStringByNameSync('viewer_' + key).replace('{0}', arg);
  }
  private notify: (phase: string, text: string) => void;
  private target: LiveEndpoint;
  private adts: boolean;

  constructor(target: LiveEndpoint, adts: boolean, notify: (phase: string, text: string) => void) {
    this.target = target;
    this.adts = adts;
    this.notify = notify;
  }

  private report(phase: string, text: string): void {
    if (this.cancelled) return;
    this.phase = phase;
    this.notify(phase, text);
  }

  private async release(): Promise<void> {
    const rec = this.recorder;
    this.recorder = undefined;
    if (rec) { try { await rec.release(); } catch (_) {} }
    const file = this.file;
    this.file = undefined;
    if (file) { try { fileIo.closeSync(file); } catch (_) {} }
  }

  cancel(): void {
    if (this.cancelled) return;
    this.cancelled = true;
    if (this.req) this.req.destroy();
    this.req = undefined;
    this.work = this.work.then(() => this.release());
  }

  start(ctx: common.UIAbilityContext): void {
    if (this.cancelled || this.phase !== 'idle' || (this.target.scheme !== 'http' && this.target.scheme !== 'https')) return;
    this.ctx = ctx;
    this.report('pending', this.text('voice_permission'));
    this.work = this.work.then(async () => {
      try {
        if (this.cancelled) return;
        const result = await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(ctx, ['ohos.permission.MICROPHONE']);
        if (this.cancelled) return;
        if (result.authResults.length !== 1 || result.authResults[0] !== 0) {
          this.report('done', this.text('voice_denied')); return;
        }
        this.recorder = await media.createAVRecorder();
        if (this.cancelled) return;
        const rec = this.recorder;
        rec.on('error', () => { this.report('done', this.text('voice_error')); this.cancel(); });
        rec.on('stateChange', (state: media.AVRecorderState) => {
          if (state === 'stopped' && this.phase === 'recording') this.report('ready', this.text('voice_ready'));
        });
        // An unlinked private fd is reclaimed by the OS even on process death.
        const path = ctx.cacheDir + '/talk-' + util.generateRandomUUID();
        this.file = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.TRUNC);
        fileIo.unlinkSync(path);
        // Harmony's existing VoicePlayer accepts at most 64 AAC frames per body.
        // 3 seconds at 16 kHz is ~47 AAC-LC frames, including margin for priming.
        const seconds = this.adts ? 3 : 15;
        await rec.prepare({ audioSourceType: media.AudioSourceType.AUDIO_SOURCE_TYPE_MIC,
          profile: { audioCodec: media.CodecMimeType.AUDIO_AAC, audioBitrate: 32000, audioChannels: 1,
            audioSampleRate: 16000, fileFormat: this.adts ? media.ContainerFormatType.CFT_AAC : media.ContainerFormatType.CFT_MPEG_4A },
          maxDuration: seconds, url: 'fd://' + this.file.fd });
        if (this.cancelled) return;
        await rec.start();
        this.report('recording', this.text('voice_recording', String(seconds)));
      } catch (_) { this.report('done', this.text('voice_start_error')); this.cancel(); }
    });
  }

  send(): void {
    if (this.cancelled || (this.phase !== 'recording' && this.phase !== 'ready')) return;
    this.report('sending', this.text('voice_sending'));
    this.work = this.work.then(async () => {
      try {
        if (this.cancelled || !this.file || !this.recorder) return;
        if (this.recorder.state !== 'stopped') await this.recorder.stop();
        if (this.cancelled) return;
        const rec = this.recorder;
        this.recorder = undefined;
        await rec.release();
        if (this.cancelled) return;
        const fd = this.file.fd;
        const size = fileIo.statSync(fd).size;
        if (size < 1 || size > 256 * 1024) throw new Error('invalid clip size');
        const body = new ArrayBuffer(size);
        fileIo.lseek(fd, 0);
        if (fileIo.readSync(fd, body) !== size) throw new Error('incomplete clip');
        fileIo.closeSync(this.file);
        this.file = undefined;
        const headers: Record<string, string> = { 'Content-Type': this.adts ? 'audio/aac' : 'audio/m4a', 'Accept': 'text/plain' };
        if (this.target.token) headers['Authorization'] = 'Basic ' + this.target.token;
        this.req = http.createHttp();
        const response = await this.req.request(this.target.origin + '/put_voice', { method: http.RequestMethod.PUT,
          header: headers, extraData: body, maxRedirects: 0, usingProxy: false, usingCache: false,
          maxLimit: 16384, connectTimeout: 6000, readTimeout: 6000 });
        const code = response.responseCode;
        this.report('done', code === 200 ? this.text('voice_ok') : code === 401 || code === 403 ? this.text('voice_auth') :
          code >= 300 && code < 400 ? this.text('voice_redirect') : this.text('voice_rejected', String(code)));
      } catch (e) { this.report('done', (e as BusinessError).code === 2300047 ? this.text('voice_redirect') : this.text('voice_failed')); }
      finally { this.cancel(); }
    });
  }
}
