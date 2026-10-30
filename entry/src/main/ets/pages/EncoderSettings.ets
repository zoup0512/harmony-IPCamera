import { common } from '@kit.AbilityKit';
import { router } from '@kit.ArkUI';
import { Prefs } from '../model/Prefs';
import { RtspServer } from '../model/RtspServer';

// Android SettingsFragment "video encoder" sub-page: resolution, codec,
// bitrate, HEVC bitrate, keyframe interval, frame rate, profile, facing.
@Entry
@Component
struct EncoderSettings {
  private ctx = getContext(this) as common.UIAbilityContext;
  @State sizes: string[] = [];
  @State selOpts: SelectOption[] = [];
  @State sizeIdx: number = 0;
  @State codecIdx: number = 0;
  @State profileIdx: number = 0;
  @State front: boolean = false;
  @State bitrate: number = 2000;
  @State hevcBitrate: number = 2000;
  @State iFrame: number = 2;
  @State fps: number = 30;

  aboutToAppear(): void {
    this.front = Prefs.getBool(this.ctx, 'use_front_camera', false);
    this.codecIdx = Prefs.getStr(this.ctx, 'video_codec', 'video/avc') === 'video/hevc' ? 1 : 0;
    this.profileIdx = Prefs.getInt(this.ctx, 'hw_encoder_profile', 0);
    this.bitrate = Prefs.getInt(this.ctx, 'video_bitrate', 2000);
    this.hevcBitrate = Prefs.getInt(this.ctx, 'hevc_bitrate', 2000);
    this.iFrame = Prefs.getInt(this.ctx, 'iframe_interval', 2);
    this.fps = Prefs.getInt(this.ctx, 'video_frame_rate', 30);
    try {
      this.sizes = RtspServer.cameraSizes(this.front);
    } catch { this.sizes = []; }
    if (this.sizes.length === 0) this.sizes = ['1920x1080', '1280x720', '960x540', '640x480'];
    this.selOpts = this.sizes.map((s: string): SelectOption => ({ value: s }));
    const cur = `${Prefs.getInt(this.ctx, 'video_width', 1280)}x${Prefs.getInt(this.ctx, 'video_height', 720)}`;
    const i = this.sizes.indexOf(cur);
    this.sizeIdx = i >= 0 ? i : 0;
  }

  put(k: string, v: string | number | boolean): void { Prefs.put(this.ctx, k, v); }

  @Builder Row2(label: ResourceStr) {
    Text(label).fontSize(13).width(110)
  }

  build() {
    Column({ space: 10 }) {
      Row() {
        Button('<').width(34).height(30).onClick(() => router.back())
        Text($r('app.string.encoder_settings')).fontSize(17).fontWeight(FontWeight.Bold)
          .margin({ left: 8 })
      }.width('94%').margin({ top: 12 })

      Scroll() {
        Column({ space: 10 }) {
          Row({ space: 8 }) {
            this.Row2($r('app.string.resolution'))
            Select(this.selOpts)
              .value(this.sizes[this.sizeIdx]).font({ size: 13 })
              .onSelect((i: number) => {
                this.sizeIdx = i;
                const p = this.sizes[i].split('x');
                this.put('video_width', parseInt(p[0]));
                this.put('video_height', parseInt(p[1]));
              })
          }
          Row({ space: 8 }) {
            this.Row2($r('app.string.video_codec'))
            Select([{ value: 'H.264' }, { value: 'H.265/HEVC' }])
              .value(this.codecIdx === 1 ? 'H.265/HEVC' : 'H.264').font({ size: 13 })
              .onSelect((i: number) => {
                this.codecIdx = i;
                this.put('video_codec', i === 1 ? 'video/hevc' : 'video/avc');
              })
          }
          Row({ space: 8 }) {
            this.Row2($r('app.string.encoder_profile'))
            Select([{ value: 'Baseline' }, { value: 'Main' }, { value: 'High' }])
              .value(['Baseline', 'Main', 'High'][this.profileIdx]).font({ size: 13 })
              .onSelect((i: number) => { this.profileIdx = i; this.put('hw_encoder_profile', i); })
          }
          Row({ space: 8 }) {
            this.Row2($r('app.string.video_bitrate'))
            TextInput({ text: this.bitrate.toString() }).type(InputType.Number)
              .fontSize(13).layoutWeight(1)
              .onChange((v: string) => { this.bitrate = parseInt(v) || 0; this.put('video_bitrate', this.bitrate); })
          }
          Row({ space: 8 }) {
            this.Row2($r('app.string.hevc_bitrate'))
            TextInput({ text: this.hevcBitrate.toString() }).type(InputType.Number)
              .fontSize(13).layoutWeight(1)
              .onChange((v: string) => { this.hevcBitrate = parseInt(v) || 0; this.put('hevc_bitrate', this.hevcBitrate); })
          }
          Row({ space: 8 }) {
            this.Row2($r('app.string.iframe_interval'))
            TextInput({ text: this.iFrame.toString() }).type(InputType.Number)
              .fontSize(13).layoutWeight(1)
              .onChange((v: string) => { this.iFrame = parseInt(v) || 1; this.put('iframe_interval', this.iFrame); })
          }
          Row({ space: 8 }) {
            this.Row2($r('app.string.frame_rate'))
            TextInput({ text: this.fps.toString() }).type(InputType.Number)
              .fontSize(13).layoutWeight(1)
              .onChange((v: string) => { this.fps = parseInt(v) || 30; this.put('video_frame_rate', this.fps); })
          }
          Row({ space: 8 }) {
            this.Row2($r('app.string.camera_facing'))
            Select([{ value: 'Back' }, { value: 'Front' }])
              .value(this.front ? 'Front' : 'Back').font({ size: 13 })
              .onSelect((i: number) => { this.front = i === 1; this.put('use_front_camera', this.front); })
          }
        }
        .width('94%').alignItems(HorizontalAlign.Start).margin({ top: 8 })
      }.layoutWeight(1)
    }
    .width('100%').height('100%').backgroundColor('#FFFFFF')
  }
}
