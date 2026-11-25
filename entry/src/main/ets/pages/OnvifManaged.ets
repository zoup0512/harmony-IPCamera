import { OnvifManagedSession, ManagedProfile } from '../model/OnvifManagedSession';
import { EncoderEditor, EncoderValues } from '../model/OnvifEncoder';
import { publicEndpoint, saveDevice } from '../util/DeviceStore';
import { ANDROID_BG, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT } from '../components/AndroidTheme';
import { SettingsSection, SettingsToolbar, SettingsWarning, settingsAction, settingsField, settingsSelect } from '../components/ExtensionSettings';

function options(values: string[]): SelectOption[] { return values.map((v: string): SelectOption => { return { value: v }; }); }

@Entry
@Component
struct OnvifManaged {
  @State private address: string = '';
  @State private user: string = '';
  @State private password: string = '';
  @State private busy: boolean = false;
  @State private profiles: ManagedProfile[] = [];
  @State private index: number = -1;
  @State private notice: ResourceStr = '';
  @State private streamOrigin: string = '';
  @State private saved: boolean = false;
  @State private editor: EncoderEditor | undefined = undefined;
  @State private resolutionValue: string = '';
  @State private fps: string = '';
  @State private bitrate: string = '';
  @State private level: string = '';
  private streamUri: string = '';
  private session?: OnvifManagedSession;

  private label(res: Resource, ...args: string[]): string {
    return getContext(this).resourceManager.getStringSync(res.id, ...args);
  }

  private reset(): void {
    this.session?.close(); this.session = undefined;
    this.password = ''; this.user = ''; this.index = -1; this.profiles = []; this.editor = undefined;
    this.streamUri = ''; this.streamOrigin = ''; this.saved = false; this.busy = false; this.notice = '';
  }
  onPageHide(): void { this.reset(); }
  aboutToDisappear(): void { this.reset(); }

  private async login(): Promise<void> {
    if (this.busy || this.session) return;
    this.busy = true; this.notice = '';
    let session: OnvifManagedSession;
    try { session = new OnvifManagedSession(this.address.trim(), this.user, this.password); }
    catch (_) { this.notice = $r('app.string.addon_onvif_credentials'); this.busy = false; return; }
    this.session = session; this.password = ''; this.user = '';
    try {
      const result = await session.login();
      if (this.session === session) { this.profiles = result; this.notice = $r('app.string.addon_onvif_choose_hint'); }
    } catch (_) {
      if (this.session === session) {
        this.reset(); this.notice = $r('app.string.addon_onvif_login_failed');
      }
    } finally { if (this.session === session) this.busy = false; }
  }

  private async choose(i: number): Promise<void> {
    const s = this.session;
    if (!s || this.busy) return;
    this.busy = true; this.index = -1; this.streamUri = ''; this.streamOrigin = ''; this.editor = undefined;
    this.saved = false; this.notice = '';
    try {
      const uri = await s.choose(i);
      if (this.session === s) { this.index = i; this.streamUri = uri; this.streamOrigin = publicEndpoint(uri); }
    } catch (_) { if (this.session === s) this.notice = $r('app.string.addon_onvif_stream_failed'); }
    finally { if (this.session === s) this.busy = false; }
  }

  private async save(): Promise<void> {
    const s = this.session;
    if (!s || !this.streamUri || this.busy || this.saved) return;
    this.busy = true;
    try {
      await saveDevice('', this.profiles[this.index].name || `Profile ${this.index + 1}`, this.streamUri);
      if (this.session === s) { this.saved = true; this.notice = $r('app.string.addon_onvif_saved_notice'); }
    } catch (_) { if (this.session === s) this.notice = $r('app.string.addon_onvif_save_failed'); }
    finally { if (this.session === s) this.busy = false; }
  }

  private async read(): Promise<void> {
    const s = this.session;
    if (!s || this.busy || this.index < 0) return;
    this.busy = true; this.editor = undefined; this.notice = '';
    try {
      const e = await s.readEncoder();
      if (this.session !== s) return;
      this.resolutionValue = e.original.size; this.fps = e.original.fps; this.bitrate = e.original.bitrate;
      this.level = e.original.level; this.editor = e;
    } catch (_) { if (this.session === s) this.notice = $r('app.string.addon_onvif_read_failed'); }
    finally { if (this.session === s) this.busy = false; }
  }

  private review(): void {
    const s = this.session;
    const e = this.editor;
    if (!s || !e || this.busy) return;
    const draft: EncoderValues = { size: this.resolutionValue, fps: this.fps, bitrate: this.bitrate, level: this.level };
    let ticket: number;
    try { ticket = s.preview(draft); }
    catch (_) { this.notice = $r('app.string.addon_onvif_invalid'); return; }
    const old = e.original;
    const changes: string[] = [];
    if (old.size !== draft.size) changes.push(this.label($r('app.string.addon_onvif_change_size'), old.size, draft.size));
    if (old.fps !== draft.fps) changes.push(this.label($r('app.string.addon_onvif_change_fps'), old.fps, draft.fps));
    if (old.bitrate !== draft.bitrate) changes.push(this.label($r('app.string.addon_onvif_change_bitrate'), old.bitrate, draft.bitrate));
    if (old.level !== draft.level) changes.push(this.label($r('app.string.addon_onvif_change_level'), old.level, draft.level));
    this.getUIContext().showAlertDialog({ title: $r('app.string.addon_onvif_confirm'), autoCancel: false,
      message: $r('app.string.addon_onvif_impact', s.target.origin, this.index + 1, this.profiles[this.index].name, changes.join('\n')),
      primaryButton: { value: $r('app.string.addon_onvif_cancel'), action: () => s.cancel(ticket) },
      secondaryButton: { value: $r('app.string.addon_onvif_submit'), action: () => this.write(s, ticket) } });
  }
  private async write(s: OnvifManagedSession, ticket: number): Promise<void> {
    if (this.session !== s || this.busy || !this.editor) return;
    this.busy = true; this.editor = undefined;
    try { await s.confirm(ticket); if (this.session === s) this.notice = $r('app.string.addon_onvif_applied'); }
    catch (_) { if (this.session === s) this.notice = $r('app.string.addon_onvif_unknown'); }
    finally { if (this.session === s) this.busy = false; }
  }

  @Builder
  encoderFields(e: EncoderEditor) {
    Column({ space: 8 }) {
      SettingsSection($r('app.string.addon_onvif_codec', e.codec))
      Text($r('app.string.addon_onvif_resolution', this.resolutionValue)).fontSize(16).fontColor(ANDROID_TEXT)
      if (e.sizes.length) {
        Select(options(e.sizes)).attributeModifier(settingsSelect).value(this.resolutionValue).onSelect((i: number) => { this.resolutionValue = e.sizes[i]; })
      }
      Text($r('app.string.addon_onvif_fps', e.original.fps || this.label($r('app.string.addon_onvif_missing')))).fontSize(16).fontColor(ANDROID_TEXT)
      if (e.fpsRange && e.original.fps) {
        Text($r('app.string.addon_onvif_range', e.fpsRange.min, e.fpsRange.max)).fontSize(14).fontColor(ANDROID_SECONDARY)
        TextInput({ text: this.fps }).attributeModifier(settingsField).type(InputType.Number).maxLength(10).onChange((v: string) => { this.fps = v; })
      }
      Text($r('app.string.addon_onvif_bitrate', e.original.bitrate || this.label($r('app.string.addon_onvif_missing')))).fontSize(16).fontColor(ANDROID_TEXT)
      if (e.bitrateRange && e.original.bitrate) {
        Text($r('app.string.addon_onvif_range', e.bitrateRange.min, e.bitrateRange.max)).fontSize(14).fontColor(ANDROID_SECONDARY)
        TextInput({ text: this.bitrate }).attributeModifier(settingsField).type(InputType.Number).maxLength(10).onChange((v: string) => { this.bitrate = v; })
      }
      if (this.level) {
        Text($r('app.string.addon_onvif_level', this.level)).fontSize(16).fontColor(ANDROID_TEXT)
        if (e.levels.length) {
          Select(options(e.levels)).attributeModifier(settingsSelect).value(this.level).onSelect((i: number) => { this.level = e.levels[i]; })
        }
      }
      Button($r('app.string.addon_onvif_review')).attributeModifier(settingsAction).onClick(() => this.review())
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  build() {
    WithTheme({ colorMode: ThemeColorMode.DARK }) {
      Column() {
        SettingsToolbar({ title: $r('app.string.addon_onvif_title') })
        Scroll() {
          Column({ space: 12 }) {
            if (!this.profiles.length) {
              SettingsWarning($r('app.string.addon_onvif_scope'))
              TextInput({ text: this.address, placeholder: $r('app.string.addon_onvif_address') }).attributeModifier(settingsField).maxLength(2048)
                .enableAutoFill(false).enabled(!this.busy).onChange((v: string) => { this.address = v; })
              TextInput({ text: this.user, placeholder: $r('app.string.addon_onvif_user') }).attributeModifier(settingsField).maxLength(128)
                .enableAutoFill(false).enabled(!this.busy).onChange((v: string) => { this.user = v; })
              TextInput({ text: this.password, placeholder: $r('app.string.addon_onvif_password') }).attributeModifier(settingsField).maxLength(256)
                .type(InputType.Password).showPasswordIcon(false).copyOption(CopyOptions.None).enableAutoFill(false)
                .enabled(!this.busy).onChange((v: string) => { this.password = v; })
              Button($r('app.string.addon_onvif_login')).attributeModifier(settingsAction).enabled(!this.busy).onClick(() => this.login())
            } else {
              SettingsSection(this.session?.target.origin ?? '')
              Select(options(this.profiles.map((p: ManagedProfile, i: number) => `${i + 1} · ${p.name || this.label($r('app.string.addon_onvif_unnamed'))}`))).attributeModifier(settingsSelect)
                .value(this.index < 0 ? this.label($r('app.string.addon_onvif_select')) : `${this.index + 1} · ${this.profiles[this.index].name}`)
                .enabled(!this.busy).onSelect((i: number) => this.choose(i))
              if (this.streamOrigin) {
                Text($r('app.string.addon_onvif_stream', this.streamOrigin)).fontSize(16).fontColor(ANDROID_TEXT)
                Button(this.saved ? $r('app.string.addon_onvif_saved') : $r('app.string.addon_onvif_save')).attributeModifier(settingsAction).enabled(!this.busy && !this.saved).onClick(() => this.save())
                Button($r('app.string.addon_onvif_read')).attributeModifier(settingsAction).enabled(!this.busy && !!this.profiles[this.index].encoder).onClick(() => this.read())
              }
              if (this.editor) {
                Divider().color(ANDROID_DIVIDER)
                this.encoderFields(this.editor)
                Divider().color(ANDROID_DIVIDER)
              }
              Button($r('app.string.addon_onvif_logout')).attributeModifier(settingsAction).enabled(!this.busy).onClick(() => this.reset())
            }
            if (this.busy) { Text($r('app.string.addon_onvif_busy')).fontSize(16).fontColor(ANDROID_TEXT) }
            if (this.notice) { Text(this.notice).fontSize(16).fontColor(ANDROID_TEXT) }
          }.alignItems(HorizontalAlign.Start).padding(16).width('100%')
        }.width('100%').layoutWeight(1).align(Alignment.TopStart)
      }.width('100%').height('100%').backgroundColor(ANDROID_BG)
    }
  }
}
