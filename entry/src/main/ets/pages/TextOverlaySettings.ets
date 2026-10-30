import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import { photoAccessHelper } from '@kit.MediaLibraryKit';
import { fileIo } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';
import { deviceInfo, batteryInfo } from '@kit.BasicServicesKit';
import { geoLocationManager } from '@kit.LocationKit';
import { Prefs } from '../model/Prefs';
import { AppServer } from '../model/AppServer';
import { OsdConfig } from '../model/RtspServer';

// Android text-overlay settings sub-page; drives osdSetConfig natively.
@Entry
@Component
struct TextOverlaySettings {
  private ctx = getContext(this) as common.UIAbilityContext;
  @State osdEn: boolean = false;
  @State padN: number = 2;
  @State posN: number = 0;
  @State fontStyle: number = 0;
  @State color: string = 'FFFFFFFF';
  @State showTs: boolean = true;
  @State showName: boolean = true;
  @State showBat: boolean = false;
  @State showGps: boolean = false;
  @State speedMph: boolean = false;
  @State customText: string = '';
  @State wmOn: boolean = false;
  @State wmPad: number = 2;
  @State wmMaxArea: number = 10;
  @State wmPos: number = 3;

  aboutToAppear(): void {
    const g = (k: string, d: boolean) => Prefs.getBool(this.ctx, k, d);
    const n = (k: string, d: number) => Prefs.getInt(this.ctx, k, d);
    this.osdEn = g('video_text_overlay', false);
    this.padN = n('osd_padding_int', 2);
    this.posN = n('text_position', 0);
    this.fontStyle = n('font_style', 0);
    this.color = Prefs.getStr(this.ctx, 'text_color', 'FFFFFFFF');
    this.showTs = g('display_timestamp', true);
    this.showName = g('display_dev_name', true);
    this.showBat = g('display_battery_info', false);
    this.showGps = g('display_gps_location', false);
    this.speedMph = g('speed_unit', false);
    this.customText = Prefs.getStr(this.ctx, 'text_custom', '');
    this.wmOn = g('watermark_overlay', false);
    this.wmPad = n('watermark_padding_int', 2);
    this.wmMaxArea = n('watermark_max_area_occupied_int', 10);
    this.wmPos = n('watermark_position', 3);
  }

  put(k: string, v: string | number | boolean): void { Prefs.put(this.ctx, k, v); }

  apply(): void {
    this.put('video_text_overlay', this.osdEn);
    this.put('osd_padding_int', this.padN);
    this.put('text_position', this.posN);
    this.put('font_style', this.fontStyle);
    this.put('text_color', this.color);
    this.put('display_timestamp', this.showTs);
    this.put('display_dev_name', this.showName);
    this.put('display_battery_info', this.showBat);
    this.put('display_gps_location', this.showGps);
    this.put('speed_unit', this.speedMph);
    this.put('text_custom', this.customText);
    this.put('watermark_overlay', this.wmOn);
    this.put('watermark_padding_int', this.wmPad);
    this.put('watermark_max_area_occupied_int', this.wmMaxArea);
    this.put('watermark_position', this.wmPos);
    const cfg: OsdConfig = {
      enabled: this.osdEn, padding: this.padN, position: this.posN,
      fontStyle: this.fontStyle, color: '#' + this.color.replace('#', ''),
      showTimestamp: this.showTs, showDevName: this.showName,
      showBattery: this.showBat, showGps: this.showGps, speedMph: this.speedMph,
      customText: this.customText, wmEnabled: this.wmOn, wmPadding: this.wmPad,
      wmMaxAreaPercent: this.wmMaxArea, wmPosition: this.wmPos
    };
    try {
      const sv = AppServer.server();
      sv.setOsdConfig(cfg);
      sv.setOsdDeviceName(deviceInfo.marketName ?? 'HarmonyOS');
      sv.setOsdBattery(batteryInfo.batterySOC, batteryInfo.chargingStatus as number);
      if (this.showGps) {
        geoLocationManager.getCurrentLocation().then((loc) => {
          sv.setOsdGps(loc.latitude, loc.longitude, loc.speed * 3.6, true, this.speedMph);
        }).catch(() => {});
      }
    } catch { /* server not started yet */ }
    promptAction.showToast({ message: $r('app.string.osd_saved') });
  }

  async pickWatermark(): Promise<void> {
    try {
      const picker = new photoAccessHelper.PhotoViewPicker();
      const res = await picker.select({
        MIMEType: photoAccessHelper.PhotoViewMIMETypes.IMAGE_TYPE, maxSelectNumber: 1 });
      const uri = res.photoUris[0];
      if (uri === undefined || uri.length === 0) return;
      const f = fileIo.openSync(uri, fileIo.OpenMode.READ_ONLY);
      const src = image.createImageSource(f.fd);
      const pm = await src.createPixelMap({ desiredPixelFormat: image.PixelMapFormat.RGBA_8888 });
      fileIo.closeSync(f);
      AppServer.server().setOsdWatermark(pm);
      this.wmOn = true;
      this.apply();
    } catch {
      promptAction.showToast({ message: $r('app.string.pick_failed') });
    }
  }

  @Builder Sw(label: ResourceStr, on: boolean, cb: (v: boolean) => void) {
    Row() {
      Text(label).fontSize(13).layoutWeight(1)
      Toggle({ type: ToggleType.Switch, isOn: on }).onChange((v: boolean) => { cb(v); this.apply(); })
    }.width('100%')
  }

  @Builder Num(label: ResourceStr, v: number, cb: (n: number) => void) {
    Row({ space: 8 }) {
      Text(label).fontSize(13).width(160)
      TextInput({ text: v.toString() }).type(InputType.Number).fontSize(13)
        .layoutWeight(1).onChange((s: string) => cb(parseInt(s) || 0))
    }.width('100%')
  }

  @Builder PosSel(label: ResourceStr, cur: string, cb: (i: number) => void) {
    Row({ space: 8 }) {
      Text(label).fontSize(13).width(160)
      Select([{ value: $r('app.string.pos_tl') }, { value: $r('app.string.pos_tr') },
              { value: $r('app.string.pos_bl') }, { value: $r('app.string.pos_br') }])
        .value(cur).font({ size: 13 })
        .onSelect((i: number) => { cb(i); this.apply(); })
    }.width('100%')
  }

  posLabel(i: number): string {
    return ['↖', '↗', '↙', '↘'][i] ?? '';
  }

  build() {
    Column({ space: 8 }) {
      Row() {
        Button('<').width(34).height(30).onClick(() => router.back())
        Text($r('app.string.overlay_settings')).fontSize(17).fontWeight(FontWeight.Bold)
          .margin({ left: 8 })
      }.width('94%').margin({ top: 12 })

      Scroll() {
        Column({ space: 10 }) {
          this.Sw($r('app.string.text_overlay'), this.osdEn, (v) => { this.osdEn = v; })
          this.Num($r('app.string.padding'), this.padN, (v) => { this.padN = v; })
          this.PosSel($r('app.string.text_position'), this.posLabel(this.posN), (v) => { this.posN = v; })
          Row({ space: 8 }) {
            Text($r('app.string.font_style')).fontSize(13).width(160)
            Select([{ value: $r('app.string.font_normal') }, { value: $r('app.string.font_bold') },
                    { value: $r('app.string.font_italic') }, { value: $r('app.string.font_bold_italic') }])
              .value(['N','B','I','BI'][this.fontStyle]).font({ size: 13 })
              .onSelect((i: number) => { this.fontStyle = i; this.apply(); })
          }
          Row({ space: 8 }) {
            Text($r('app.string.text_color')).fontSize(13).width(160)
            TextInput({ text: this.color }).fontSize(13).layoutWeight(1)
              .onChange((v: string) => { this.color = v; })
          }
          this.Sw($r('app.string.show_timestamp'), this.showTs, (v) => { this.showTs = v; })
          this.Sw($r('app.string.show_dev_name'), this.showName, (v) => { this.showName = v; })
          this.Sw($r('app.string.show_battery'), this.showBat, (v) => { this.showBat = v; })
          this.Sw($r('app.string.show_gps'), this.showGps, (v) => { this.showGps = v; })
          this.Sw($r('app.string.speed_mph'), this.speedMph, (v) => { this.speedMph = v; })
          Row({ space: 8 }) {
            Text($r('app.string.custom_text')).fontSize(13).width(160)
            TextInput({ text: this.customText }).fontSize(13).layoutWeight(1)
              .onChange((v: string) => { this.customText = v; })
          }
          Divider()
          this.Sw($r('app.string.wm_enabled'), this.wmOn, (v) => { this.wmOn = v; })
          this.Num($r('app.string.wm_padding'), this.wmPad, (v) => { this.wmPad = v; })
          this.Num($r('app.string.wm_max_area'), this.wmMaxArea, (v) => { this.wmMaxArea = v; })
          this.PosSel($r('app.string.wm_position'), this.posLabel(this.wmPos), (v) => { this.wmPos = v; })
          Row({ space: 8 }) {
            Button($r('app.string.pick_watermark')).fontSize(12).height(30)
              .onClick(() => { this.pickWatermark(); })
            Button($r('app.string.save')).fontSize(12).height(30)
              .onClick(() => this.apply())
          }
        }
        .width('94%').alignItems(HorizontalAlign.Start).margin({ top: 8 })
      }.layoutWeight(1)
    }
    .width('100%').height('100%').backgroundColor('#FFFFFF')
  }
}
