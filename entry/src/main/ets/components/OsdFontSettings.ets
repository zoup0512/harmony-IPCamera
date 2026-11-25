import { AndroidPreferenceValue } from './AndroidPreferenceValue';
import { ANDROID_TEXT, ANDROID_SECONDARY } from './AndroidTheme';
import { common } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { fileIo, picker } from '@kit.CoreFileKit';
import { defaultOsdFont, OsdFontOptions } from '../util/OsdSettings';

@Component
export struct OsdFontSettings {
  @Prop font: OsdFontOptions = defaultOsdFont();
  onChange: (font: OsdFontOptions) => void = () => {};
  @State private families: string[] = [];
  @State private picking: boolean = false;
  @State private errorText: string = '';
  private alive: boolean = false;

  aboutToAppear(): void {
    this.alive = true;
    try {
      const fonts = this.getUIContext().getFont();
      const families = new Set<string>();
      for (const name of fonts.getSystemFontList()) {
        const family = fonts.getFontByName(name).family;
        if (family) families.add(family);
      }
      this.families = Array.from(families).sort();
    } catch (_) { this.families = []; }
  }

  aboutToDisappear(): void { this.alive = false; }

  private removeImport(dir: string, path: string): void {
    const prefix = `${dir}/osd-font-`;
    // Only remove a file we created, never a path supplied by preferences.
    if (path.startsWith(prefix) && /^[a-fA-F0-9-]{36}\.font$/.test(path.substring(prefix.length))) {
      try { fileIo.unlinkSync(path); } catch (_) {}
    }
  }

  private selectFamily(family: string): void {
    const previous = this.font.fontPath;
    this.onChange({ fontSize: this.font.fontSize, fontFamily: family, fontPath: '' });
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    this.removeImport(ctx.filesDir, previous);
    this.errorText = '';
  }

  private familyMenu(): MenuElement[] {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const options: MenuElement[] = [{ value: ctx.resourceManager.getStringSync($r('app.string.osdf_default').id),
      action: () => this.selectFamily('') }];
    this.families.forEach((family: string) => {
      options.push({ value: family, action: () => this.selectFamily(family) });
    });
    return options;
  }

  private async pickFont(): Promise<void> {
    if (this.picking) return;
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    this.picking = true;
    this.errorText = '';
    let imported: string = '';
    try {
      const uris = await new picker.DocumentViewPicker(ctx).select({
        maxSelectNumber: 1,
        fileSuffixFilters: [`${ctx.resourceManager.getStringSync($r('app.string.osdf_filter').id)}|.ttf,.otf`]
      });
      if (uris.length === 0 || !this.alive) return;
      const file = await fileIo.open(uris[0], fileIo.OpenMode.READ_ONLY);
      try {
        const size = (await fileIo.stat(file.fd)).size;
        if (size < 12 || size > 32 * 1024 * 1024) throw new Error('invalid font size');
        imported = `${ctx.filesDir}/osd-font-${util.generateRandomUUID()}.font`;
        await fileIo.copyFile(file.fd, imported);
      } finally { await fileIo.close(file); }
      const copy = await fileIo.open(imported, fileIo.OpenMode.READ_ONLY);
      try {
        const header = new ArrayBuffer(4);
        if (await fileIo.read(copy.fd, header) !== 4) throw new Error('short font');
        const signature = new DataView(header).getUint32(0);
        // OpenType sfnt: TrueType outlines or CFF/OTTO outlines.
        if (signature !== 0x00010000 && signature !== 0x4F54544F) throw new Error('not a font');
      } finally { await fileIo.close(copy); }
      if (!this.alive) return;
      const previous = this.font.fontPath;
      this.onChange({ fontSize: this.font.fontSize, fontFamily: this.font.fontFamily, fontPath: imported });
      imported = '';
      this.removeImport(ctx.filesDir, previous);
    } catch (_) {
      if (this.alive) this.errorText = ctx.resourceManager.getStringSync($r('app.string.osdf_failure').id);
    } finally {
      this.removeImport(ctx.filesDir, imported);
      if (this.alive) this.picking = false;
    }
  }

  build() {
    Column() {
      Column({ space: 4 }) {
        Text($r('app.string.osdf_size')).fontSize(16).fontColor(ANDROID_TEXT)
        TextInput({ text: `${this.font.fontSize}` }).type(InputType.Number).maxLength(3)
          .fontColor(ANDROID_TEXT).backgroundColor('#424242').width('100%').onChange((value: string) => {
            if (/^\d{0,3}$/.test(value)) {
              this.onChange({ fontSize: Math.min(128, Number(value)),
                fontFamily: this.font.fontFamily, fontPath: this.font.fontPath });
            }
          })
      }.alignItems(HorizontalAlign.Start).width('100%').padding({ top: 8, bottom: 8, left: 16, right: 16 })
      AndroidPreferenceValue({ title: $r('app.string.osdf_family'), available: !this.picking,
        summary: this.font.fontPath ? $r('app.string.osdf_local') : (this.font.fontFamily || $r('app.string.osdf_default')) })
        .enabled(!this.picking).bindMenu(this.familyMenu())
      AndroidPreferenceValue({ title: $r('app.string.osdf_pick'), available: !this.picking })
        .enabled(!this.picking).onClick(() => this.pickFont())
      if (this.errorText) { Text(this.errorText).fontSize(14).fontColor(ANDROID_SECONDARY).padding(8) }
    }.alignItems(HorizontalAlign.Start).width('100%')
  }
}
