import { ANDROID_ACCENT, ANDROID_BAR, ANDROID_BG, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT, ANDROID_TOOLBAR_HEIGHT } from './AndroidTheme';

@Component
export struct SettingsToolbar {
  title: ResourceStr = '';
  backLabel: Resource = $r('app.string.addon_delivery_back');
  @Prop canGoBack: boolean = true;

  build() {
    Row({ space: 12 }) {
      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.arrow_left')).fontSize(24).fontColor([ANDROID_TEXT])
      }.width(48).height(48).backgroundColor(Color.Transparent).accessibilityText(this.backLabel)
        .enabled(this.canGoBack).onClick(() => { this.getUIContext().getRouter().back(); })
      Text(this.title).layoutWeight(1).fontSize(20).fontWeight(FontWeight.Medium).fontColor(ANDROID_TEXT)
        .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
    }.width('100%').height(ANDROID_TOOLBAR_HEIGHT).padding({ left: 4, right: 16 }).backgroundColor(ANDROID_BAR)
  }
}

@Builder
export function SettingsWarning(message: ResourceStr) {
  Text(message).fontSize(16).fontWeight(FontWeight.Medium).fontColor(ANDROID_TEXT).width('100%')
    .padding(12).backgroundColor(ANDROID_BAR).border({ width: { left: 3 }, color: ANDROID_ACCENT })
}

@Builder
export function SettingsSection(title: ResourceStr) {
  Text(title).fontSize(16).fontWeight(FontWeight.Medium).fontColor(ANDROID_ACCENT)
}

class FieldStyle implements AttributeModifier<TextInputAttribute> {
  applyNormalAttribute(field: TextInputAttribute): void {
    field.fontSize(16).fontColor(ANDROID_TEXT).placeholderColor(ANDROID_SECONDARY).caretColor(ANDROID_ACCENT)
      .height(48).backgroundColor(ANDROID_BAR).borderRadius(4).border({ width: { bottom: 1 }, color: ANDROID_SECONDARY });
  }
}
export const settingsField = new FieldStyle();

class ActionStyle implements AttributeModifier<ButtonAttribute> {
  applyNormalAttribute(button: ButtonAttribute): void {
    button.fontSize(14).fontWeight(FontWeight.Medium).fontColor(ANDROID_ACCENT).backgroundColor(ANDROID_BAR)
      .borderRadius(4).constraintSize({ minHeight: 48, maxWidth: '100%' }).labelStyle({ maxLines: 2 });
  }
}
export const settingsAction = new ActionStyle();

class SelectStyle implements AttributeModifier<SelectAttribute> {
  applyNormalAttribute(select: SelectAttribute): void {
    select.font({ size: 16 }).fontColor(ANDROID_TEXT).backgroundColor(ANDROID_BAR).width('100%')
      .menuBackgroundColor(ANDROID_BAR).optionBgColor(ANDROID_BAR).optionFontColor(ANDROID_TEXT)
      .selectedOptionBgColor(ANDROID_BG).selectedOptionFontColor(ANDROID_ACCENT)
      .border({ width: { bottom: 1 }, color: ANDROID_DIVIDER }).borderRadius(4);
  }
}
export const settingsSelect = new SelectStyle();
