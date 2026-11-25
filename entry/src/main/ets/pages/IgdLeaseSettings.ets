import { util } from '@kit.ArkTS';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { LeaseRequest, ManagedIgdProfile, managedProfile, ServiceKind } from '../model/IgdLease';
import { IgdGateway } from '../service/IgdGateway';
import { ANDROID_ACCENT, ANDROID_BG, ANDROID_DIVIDER, ANDROID_TEXT } from '../components/AndroidTheme';
import { SettingsToolbar, SettingsWarning, settingsAction, settingsField } from '../components/ExtensionSettings';

@Entry
@Component
struct IgdLeaseSettings {
  @State provisioned: boolean = false;
  @State optedIn: boolean = false;
  @State url: string = '';
  @State challenge: string = '';
  @State busy: boolean = false;
  @State status: ResourceStr = '';
  private profile?: ManagedIgdProfile;
  private draft?: LeaseRequest;

  aboutToAppear(): void {
    try {
      // Signed bundle only. A missing profile in standard builds is expected. No network on navigation.
      const bytes = getContext(this).resourceManager.getRawFileContentSync('igd-create-only.json');
      if (bytes.length > 2048) return;
      this.profile = managedProfile(util.TextDecoder.create('utf-8').decodeToString(bytes));
      this.provisioned = true;
    } catch (_) { this.provisioned = false; }
  }
  reset(): void {
    this.draft?.cancel(); this.draft = undefined;
    this.optedIn = false; this.busy = false; this.challenge = ''; this.url = '';
  }
  onPageHide(): void { this.reset(); }
  aboutToDisappear(): void { this.reset(); }
  enable(on: boolean): void {
    this.reset();
    if (!on || !this.profile) return;
    try {
      this.challenge = Array.from(cryptoFramework.createRandom().generateRandomSync(16).data)
        .map((n: number) => n.toString(16).padStart(2, '0')).join('');
      this.optedIn = true;
    } catch (_) { this.status = $r('app.string.igd_blocked'); }
  }
  cancel(d: LeaseRequest): void {
    if (this.draft === d) { d.cancel(); this.draft = undefined; this.busy = false; }
  }
  review(kind: ServiceKind): void {
    if (!this.optedIn || this.busy || !this.profile) return;
    try {
      const d = new LeaseRequest(kind, this.url, this.profile, this.challenge, new IgdGateway());
      this.draft = d; this.busy = true;
      this.getUIContext().showAlertDialog({ title: $r('app.string.igd_first'), message: $r('app.string.igd_risk'),
        cancel: () => this.cancel(d), primaryButton: { value: $r('app.string.igd_cancel'), action: () => this.cancel(d) },
        secondaryButton: { value: $r('app.string.igd_review'), action: () => this.confirm(d) } });
    } catch (_) { this.busy = false; this.status = $r('app.string.igd_blocked'); }
  }
  confirm(d: LeaseRequest): void {
    if (this.draft !== d || !this.optedIn) return;
    try {
      d.firstConfirmation();
      this.getUIContext().showAlertDialog({ title: $r('app.string.igd_final'),
        message: $r('app.string.igd_details', d.kind, d.route.local, d.service.port, d.endpoint.host, d.endpoint.port),
        cancel: () => this.cancel(d), primaryButton: { value: $r('app.string.igd_cancel'), action: () => this.cancel(d) },
        secondaryButton: { value: $r('app.string.igd_send'), action: () => this.send(d) } });
    } catch (_) { this.cancel(d); this.status = $r('app.string.igd_blocked'); }
  }
  async send(d: LeaseRequest): Promise<void> {
    if (this.draft !== d || !this.optedIn) return;
    // Consume the dialog callback synchronously. Hiding the page still cancels the live draft.
    this.optedIn = false;
    this.status = $r('app.string.igd_pending');
    try {
      const port = await d.finalConfirmation();
      this.status = $r('app.string.igd_success', port);
    } catch (_) { this.status = d.attempted ? $r('app.string.igd_unknown') : $r('app.string.igd_blocked'); }
    finally { if (this.draft === d) this.reset(); }
  }
  build() {
    WithTheme({ colorMode: ThemeColorMode.DARK }) {
      Column() {
        SettingsToolbar({ title: $r('app.string.igd_title') })
        Scroll() {
          Column({ space: 12 }) {
            SettingsWarning($r('app.string.igd_risk'))
            Text($r('app.string.igd_lifetime')).fontSize(15).fontColor(ANDROID_TEXT)
            Divider().color(ANDROID_DIVIDER)
            if (!this.provisioned) {
              Text($r('app.string.igd_unprovisioned')).fontSize(16).fontColor(ANDROID_TEXT).fontWeight(FontWeight.Medium)
            } else {
              Row() {
                Text($r('app.string.igd_enable')).fontSize(16).fontColor(ANDROID_TEXT).layoutWeight(1)
                Toggle({ type: ToggleType.Switch, isOn: this.optedIn }).selectedColor(ANDROID_ACCENT).enabled(!this.busy)
                  .onChange((on: boolean) => { if (!this.busy) this.enable(on); })
              }.width('100%')
              if (this.optedIn) {
                SettingsWarning($r('app.string.igd_pair'))
                Text(this.challenge).fontSize(18).fontColor(ANDROID_TEXT).copyOption(CopyOptions.LocalDevice)
                TextInput({ text: this.url, placeholder: $r('app.string.igd_url') }).attributeModifier(settingsField).enabled(!this.busy)
                  .enableAutoFill(false).onChange((v: string) => { this.url = v; })
                Text($r('app.string.igd_auth')).fontSize(16).fontColor(ANDROID_TEXT)
                Button($r('app.string.igd_http')).attributeModifier(settingsAction).enabled(!this.busy).onClick(() => this.review('HTTP'))
                Button($r('app.string.igd_rtsp')).attributeModifier(settingsAction).enabled(!this.busy).onClick(() => this.review('RTSP'))
              }
            }
            Text(this.status).fontSize(16).fontColor(ANDROID_TEXT)
          }.width('100%').padding(16).alignItems(HorizontalAlign.Start)
        }.width('100%').layoutWeight(1).align(Alignment.TopStart)
      }.width('100%').height('100%').backgroundColor(ANDROID_BG)
    }
  }
}
