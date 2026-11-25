import { ANDROID_ACCENT, ANDROID_BAR, ANDROID_TEXT, ANDROID_TOOLBAR_HEIGHT } from './AndroidTheme';

@Component
export struct MgrIcon {
  @Prop icon: Resource = $r('sys.symbol.arrow_left');
  @Prop label: Resource = $r('app.string.mgr_ui_options');
  @Prop enabledAction: boolean = true;
  action: () => void = () => {};
  build() {
    Button({ type: ButtonType.Circle }) {
      SymbolGlyph(this.icon).fontSize(24).fontColor([ANDROID_TEXT])
    }.width(48).height(48).backgroundColor(Color.Transparent).enabled(this.enabledAction)
    .accessibilityText(this.label).onClick(this.action)
  }
}

// Bind the menu to the focusable button itself, not to a wrapping row.
@Component
export struct MgrMore {
  @Prop enabledAction: boolean = true;
  @BuilderParam items: () => void;
  build() {
    Button({ type: ButtonType.Circle }) {
      Text('⋮').fontSize(24).fontWeight(FontWeight.Bold).fontColor(ANDROID_TEXT)
    }.width(48).height(48).backgroundColor(Color.Transparent).enabled(this.enabledAction)
      .accessibilityText($r('app.string.mgr_ui_options')).bindMenu(this.items)
  }
}

@Component
export struct MgrAction {
  @Prop label: Resource = $r('app.string.mgr_ui_options');
  @Prop enabledAction: boolean = true;
  action: () => void = () => {};
  build() {
    Button(this.label, { type: ButtonType.Normal }).constraintSize({ minHeight: 44 })
      .fontSize(14).fontColor(ANDROID_ACCENT).backgroundColor(Color.Transparent)
      .enabled(this.enabledAction).onClick(this.action)
  }
}

@Component
export struct MgrToolbar {
  @Prop title: ResourceStr = '';
  @BuilderParam actions: () => void = this.noActions;
  @Builder noActions() {}
  build() {
    Row({ space: 4 }) {
      MgrIcon({ label: $r('app.string.mgr_ui_back'), action: () => this.getUIContext().getRouter().back() })
      Text(this.title).layoutWeight(1).fontColor(ANDROID_TEXT).fontSize(20).fontWeight(FontWeight.Medium)
        .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
      this.actions()
    }.width('100%').height(ANDROID_TOOLBAR_HEIGHT).padding({ left: 4, right: 4 }).backgroundColor(ANDROID_BAR)
  }
}

// The scanner has no fractional completion: animate a segment, not a percentage.
@Component
export struct MgrScanProgress {
  @State moving: boolean = false;
  build() {
    Stack() {
      Row().width('33%').height(3).backgroundColor(ANDROID_ACCENT)
        .position({ x: this.moving ? '100%' : '-33%', y: 0 })
    }.width('100%').height(3).clip(true).backgroundColor('#55452F')
      .accessibilityText($r('app.string.scan_scanning'))
      .onAppear(() => {
        this.getUIContext().animateTo({ duration: 1100, curve: Curve.Linear, iterations: -1 }, () => { this.moving = true; });
      })
  }
}
