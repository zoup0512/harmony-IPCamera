import { ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT } from './AndroidTheme';

@Component
export struct AndroidPreferenceValue {
  @Prop title: ResourceStr = '';
  @Prop summary: ResourceStr = '';
  @Prop available: boolean = true;

  build() {
    Column() {
      Column({ space: 3 }) {
        Text(this.title).fontSize(16).fontColor(ANDROID_TEXT).width('100%')
        if (this.summary !== '') {
          Text(this.summary).fontSize(14).fontColor(ANDROID_SECONDARY).width('100%')
        }
      }.width('100%').alignItems(HorizontalAlign.Start).justifyContent(FlexAlign.Center)
        .constraintSize({ minHeight: 56 }).padding({ top: 8, bottom: 8, left: 16, right: 16 })
        .opacity(this.available ? 1 : 0.52)
      Divider().color(ANDROID_DIVIDER).strokeWidth(1)
    }.width('100%')
  }
}
