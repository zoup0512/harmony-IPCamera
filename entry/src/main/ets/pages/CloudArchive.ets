import { common } from '@kit.AbilityKit';
import { ANDROID_BG, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT } from '../components/AndroidTheme';
import { SettingsSection, SettingsToolbar, SettingsWarning, settingsAction, settingsField } from '../components/ExtensionSettings';
import { ArchiveRow, CloudArchiveService, recordings } from '../service/CloudArchiveService';
import { Challenge, validClient } from '../util/CloudArchiveProtocol';

@Entry
@Component
struct CloudArchive {
  @State client: string = '';
  @State tenant: string = '';
  @State connected: string = '';
  @State rows: ArchiveRow[] = [];
  @State busy: boolean = false;
  @State cleanKey: string = '';
  @State code: string = '';
  @State uri: string = '';
  @State status: ResourceStr = '';
  private service: CloudArchiveService = new CloudArchiveService();
  private ticket: number = 0;
  private cleanTimer: number = -1;
  private clearTimer(): void { if (this.cleanTimer !== -1) clearTimeout(this.cleanTimer); this.cleanTimer = -1; }

  private dir(): string { return (getContext(this) as common.UIAbilityContext).filesDir; }
  aboutToAppear(): void { this.restore(); this.refresh(); }
  aboutToDisappear(): void { this.stop(); }
  private restore(): void {
    try {
      this.connected = this.service.account();
      if (this.connected) this.client = this.connected;
    } catch (_) { this.connected = ''; this.status = $r('app.string.addon_cloud_vault'); }
  }
  private refresh(): void {
    this.rows = recordings(this.dir());
    if (this.cleanKey && !this.rows.some((row: ArchiveRow) => row.name + row.identity === this.cleanKey && this.service.canClean(row))) this.cleanKey = '';
  }
  onPageHide(): void { if (this.connected) this.stop(); } // Allow switching to the browser during device login.
  private stop(): void {
    this.clearTimer(); this.ticket++; this.service.cancel(); this.cleanKey = ''; this.busy = false; this.code = ''; this.uri = '';
  }
  private askConnect(): void {
    const id = this.client.trim();
    if (!validClient(id)) { this.status = $r('app.string.addon_cloud_client_invalid'); return; }
    const tenant = this.tenant.trim() || 'common';
    if (tenant !== 'common' && !validClient(tenant)) { this.status = $r('app.string.addon_cloud_tenant_invalid'); return; }
    AlertDialog.show({ title: $r('app.string.addon_cloud_connect_title'),
      message: $r('app.string.addon_cloud_connect_warning', id, tenant),
      primaryButton: { value: $r('app.string.addon_cloud_cancel'), action: () => {} },
      secondaryButton: { value: $r('app.string.addon_cloud_confirm_connect'), action: () => { this.connect(id, tenant); } } });
  }
  private async connect(id: string, tenant: string): Promise<void> {
    if (this.busy || this.connected) return;
    const ticket = ++this.ticket;
    this.busy = true; this.status = $r('app.string.addon_cloud_request_code');
    try {
      await this.service.connect(id, tenant, (d: Challenge) => {
        if (ticket !== this.ticket) return;
        this.uri = d.uri; this.code = d.code; this.status = $r('app.string.addon_cloud_authorize');
      });
      if (ticket === this.ticket) { this.client = id; this.connected = id; this.status = $r('app.string.addon_cloud_authorized'); }
    } catch (_) {
      if (ticket === this.ticket) this.status = $r('app.string.addon_cloud_connect_failed');
    } finally { if (ticket === this.ticket) { this.busy = false; this.code = ''; this.uri = ''; } }
  }
  private disconnect(): void {
    this.stop(); this.connected = '';
    try { this.service.disconnect(); this.status = $r('app.string.addon_cloud_signed_out'); }
    catch (_) { this.status = $r('app.string.addon_cloud_signout_failed'); }
  }
  private askUpload(row: ArchiveRow): void {
    if (!this.connected || this.busy) return;
    AlertDialog.show({ title: $r('app.string.addon_cloud_upload_title'),
      message: $r('app.string.addon_cloud_upload_warning', row.name, row.size),
      primaryButton: { value: $r('app.string.addon_cloud_cancel'), action: () => {} },
      secondaryButton: { value: $r('app.string.addon_cloud_confirm_upload'), action: () => { this.upload(row); } } });
  }
  private async upload(row: ArchiveRow): Promise<void> {
    if (!this.connected || this.busy) return;
    const ticket = ++this.ticket;
    this.clearTimer(); this.busy = true; this.cleanKey = ''; this.status = $r('app.string.addon_cloud_preparing');
    try {
      const verified = await this.service.upload(this.dir(), row, (n: number) => {
        if (ticket === this.ticket) this.status = $r('app.string.addon_cloud_progress', `${Math.floor(n / row.size * 100)}%`, row.name);
      });
      if (ticket === this.ticket) {
        this.cleanKey = verified ? row.name + row.identity : '';
        if (verified) this.cleanTimer = setTimeout(() => {
          if (ticket === this.ticket) { this.stop(); this.restore(); this.status = $r('app.string.addon_cloud_expired'); }
        }, Math.max(0, this.service.cleanupExpires() - Date.now()));
        this.status = verified ? $r('app.string.addon_cloud_verified', row.name) : $r('app.string.addon_cloud_uploaded', row.name);
      }
    } catch (e) {
      if (ticket !== this.ticket) return;
      if ((e as Error).message === 'expired') {
        this.connected = ''; this.status = $r('app.string.addon_cloud_expired');
      } else if ((e as Error).message === 'source') {
        this.status = $r('app.string.addon_cloud_source_failed');
      } else {
        this.status = $r('app.string.addon_cloud_upload_failed');
      }
    } finally { if (ticket === this.ticket) this.busy = false; }
  }
  private askClean(row: ArchiveRow): void {
    if (this.busy || !this.cleanKey || !this.service.canClean(row)) { this.cleanKey = ''; return; }
    const ticket = this.ticket;
    AlertDialog.show({ title: $r('app.string.addon_cloud_clean'),
      message: $r('app.string.addon_cloud_clean_warning', row.name, row.size),
      primaryButton: { value: $r('app.string.addon_cloud_cancel'), action: () => {} },
      secondaryButton: { value: $r('app.string.addon_cloud_clean'), action: () => {
        if (ticket === this.ticket && !this.busy) this.clean(row);
      } } });
  }
  private async clean(row: ArchiveRow): Promise<void> {
    const ticket = ++this.ticket;
    this.clearTimer(); this.busy = true; this.cleanKey = '';
    try {
      await this.service.cleanConfirmed(row);
      if (ticket === this.ticket) this.status = $r('app.string.addon_cloud_cleaned', row.name);
    } catch (_) {
      if (ticket === this.ticket) this.status = $r('app.string.addon_cloud_clean_failed');
    } finally { if (ticket === this.ticket) { this.busy = false; this.refresh(); } }
  }
  build() {
    WithTheme({ colorMode: ThemeColorMode.DARK }) {
      Column() {
        SettingsToolbar({ title: $r('app.string.addon_cloud_title') })
        Scroll() {
          Column({ space: 12 }) {
            Text($r('app.string.addon_cloud_setup')).fontSize(16).fontColor(ANDROID_TEXT)
            TextInput({ text: this.client, placeholder: $r('app.string.addon_cloud_client') }).attributeModifier(settingsField)
              .enabled(!this.busy && !this.connected).onChange((value: string) => { this.client = value; })
            if (!this.connected) {
              TextInput({ text: this.tenant, placeholder: $r('app.string.addon_cloud_tenant') }).attributeModifier(settingsField)
                .enabled(!this.busy).onChange((value: string) => { this.tenant = value; })
              Button($r('app.string.addon_cloud_connect')).attributeModifier(settingsAction).enabled(!this.busy).onClick(() => this.askConnect())
            } else {
              Text($r('app.string.addon_cloud_saved')).fontSize(16).fontColor(ANDROID_TEXT)
            }
            Button($r('app.string.addon_cloud_signout')).attributeModifier(settingsAction).onClick(() => this.disconnect())
            if (this.code) {
              Divider().color(ANDROID_DIVIDER)
              Text(this.uri).fontColor(ANDROID_TEXT).fontSize(16).fontWeight(FontWeight.Bold)
              Text($r('app.string.addon_cloud_device_code', this.code)).fontSize(20).fontWeight(FontWeight.Medium).fontColor(ANDROID_TEXT)
              SettingsWarning($r('app.string.addon_cloud_code_warning'))
            }
            if (this.busy) {
              Button($r('app.string.addon_cloud_stop')).attributeModifier(settingsAction).onClick(() => { this.stop(); this.status = $r('app.string.addon_cloud_stopped'); })
            }
            if (this.status) { Text(this.status).fontSize(16).fontColor(ANDROID_TEXT) }
            Divider().color(ANDROID_DIVIDER)
            SettingsSection($r('app.string.addon_cloud_recordings'))
            Text($r('app.string.addon_cloud_manual')).fontSize(14).fontColor(ANDROID_TEXT)
            Button($r('app.string.addon_cloud_refresh')).attributeModifier(settingsAction).enabled(!this.busy).onClick(() => { this.restore(); this.refresh(); })
            if (this.rows.length === 0) { Text($r('app.string.addon_cloud_empty')).fontSize(14).fontColor(ANDROID_SECONDARY) }
            ForEach(this.rows, (row: ArchiveRow) => {
              Column({ space: 8 }) {
                Divider().color(ANDROID_DIVIDER)
                Text(row.name).fontSize(16).fontColor(ANDROID_TEXT)
                if (!this.busy && this.connected && this.cleanKey === row.name + row.identity) {
                  Button($r('app.string.addon_cloud_clean')).attributeModifier(settingsAction).onClick(() => this.askClean(row))
                }
                Button($r('app.string.addon_cloud_upload', row.size)).attributeModifier(settingsAction).enabled(!!this.connected && !this.busy).onClick(() => this.askUpload(row))
              }.width('100%').alignItems(HorizontalAlign.Start)
            }, (row: ArchiveRow) => row.name + row.identity)
          }.alignItems(HorizontalAlign.Start).padding(16).width('100%')
        }.width('100%').layoutWeight(1).align(Alignment.TopStart)
      }.width('100%').height('100%').backgroundColor(ANDROID_BG)
    }
  }
}
