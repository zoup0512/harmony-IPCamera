import { router } from '@kit.ArkUI';
import { bundleManager } from '@kit.AbilityKit';

@Entry
@Component
struct About {
  @State ver: string = '';

  aboutToAppear(): void {
    try {
      bundleManager.getBundleInfoForSelf(bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION)
        .then((info) => { this.ver = info.versionName; })
        .catch(() => { this.ver = ''; });
    } catch { /* ignore */ }
  }

  build() {
    Column({ space: 12 }) {
      Row() {
        Button('<').width(34).height(30).onClick(() => router.back())
        Text($r('app.string.about')).fontSize(17).fontWeight(FontWeight.Bold).margin({ left: 8 })
      }.width('94%').margin({ top: 12 })
      Column({ space: 8 }) {
        Text($r('app.string.EntryAbility_label')).fontSize(20).fontWeight(FontWeight.Bold)
        Text(`${$r('app.string.version_label')}: ${this.ver}`).fontSize(13)
        Text(`${$r('app.string.author_label')}: shenyaocn (Android) / HarmonyOS port`).fontSize(13)
      }.margin({ top: 40 })
    }
    .width('100%').height('100%').backgroundColor('#FFFFFF')
  }
}
