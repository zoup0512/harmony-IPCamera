import { common } from '@kit.AbilityKit';
import { ANDROID_ACCENT, ANDROID_BG, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT } from '../components/AndroidTheme';
import { SettingsSection, SettingsToolbar, SettingsWarning, settingsAction } from '../components/ExtensionSettings';
import { lanServiceAnnouncements, ServiceAnnouncement } from '../model/LanServiceAnnouncements';

@Component
struct AnnouncementRow {
  http: boolean = false;
  supported: boolean = false;
  @State optedIn: boolean = false;
  @State status: string = '';
  @State failed: boolean = false;
  private channel?: ServiceAnnouncement;
  private refresh: () => void = () => {
    if (this.channel) {
      this.optedIn = this.channel.enabled;
      this.status = this.channel.status;
      this.failed = this.channel.failed;
    }
  };

  aboutToAppear(): void {
    const services = lanServiceAnnouncements(this.getUIContext().getHostContext() as common.UIAbilityContext);
    this.channel = this.http ? services.http : services.rtsp;
    this.channel.observe(this.refresh);
    this.refresh();
  }

  aboutToDisappear(): void {
    this.channel?.unobserve(this.refresh); // Page visibility does not own the listener.
  }

  build() {
    Column({ space: 6 }) {
      Row() {
        Text(this.http ? $r('app.string.addon_lan_http') : $r('app.string.addon_lan_rtsp')).fontSize(16).fontColor(ANDROID_TEXT).layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.optedIn }).selectedColor(ANDROID_ACCENT)
          .accessibilityText(this.http ? $r('app.string.addon_lan_announce_http') : $r('app.string.addon_lan_announce_rtsp'))
          .enabled(this.supported)
          .onChange((on: boolean) => { this.channel?.setEnabled(on); })
      }.width('100%')
      Text(this.supported ? this.status : $r('app.string.addon_lan_unsupported')).fontSize(14).fontColor(this.failed ? ANDROID_TEXT : ANDROID_SECONDARY)
      if (this.failed) {
        Button($r('app.string.addon_lan_retry')).attributeModifier(settingsAction).onClick(() => { this.channel?.retry(); })
      }
    }.width('100%').alignItems(HorizontalAlign.Start).padding({ top: 4, bottom: 4 })
  }
}

@Entry
@Component
struct LanServiceSettings {
  private supported: boolean = canIUse('SystemCapability.Communication.NetManager.MDNS');

  build() {
    WithTheme({ colorMode: ThemeColorMode.DARK }) {
      Column() {
        SettingsToolbar({ title: $r('app.string.addon_lan_title') })
        Scroll() {
          Column({ space: 12 }) {
            SettingsWarning($r('app.string.addon_lan_trust'))
            Text($r('app.string.addon_lan_lifetime')).fontSize(14).fontColor(ANDROID_SECONDARY)
            Divider().color(ANDROID_DIVIDER)
            AnnouncementRow({ http: true, supported: this.supported })
            Divider().color(ANDROID_DIVIDER)
            AnnouncementRow({ http: false, supported: this.supported })
            Divider().color(ANDROID_DIVIDER)
            SettingsSection($r('app.string.addon_lan_public'))
            Text($r('app.string.addon_lan_exposure')).fontSize(16).fontColor(ANDROID_TEXT)
            Button($r('app.string.addon_lan_lease')).attributeModifier(settingsAction).onClick(() => {
              this.getUIContext().getRouter().pushUrl({ url: 'pages/IgdLeaseSettings' });
            })
          }.width('100%').alignItems(HorizontalAlign.Start).padding(16)
        }.width('100%').layoutWeight(1).align(Alignment.TopStart)
      }.width('100%').height('100%').backgroundColor(ANDROID_BG)
    }
  }
}
