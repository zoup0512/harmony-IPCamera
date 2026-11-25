import { ConfigurationConstant, UIAbility } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const TAG: string = 'IPCameraEntry';
const DOMAIN: number = 0x0000;

export default class EntryAbility extends UIAbility {

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
      const main = windowStage.getMainWindowSync();
      main.setWindowBackgroundColor('#303030');
      main.setWindowSystemBarProperties({
        statusBarColor: '#000000', statusBarContentColor: '#FFFFFF',
        navigationBarColor: '#000000', navigationBarContentColor: '#FFFFFF'
      }).catch(() => hilog.warn(DOMAIN, TAG, 'System bar theme unavailable'));
    } catch (_) {
      hilog.warn(DOMAIN, TAG, 'Window theme unavailable');
    }
    windowStage.loadContent('pages/Devices', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, '%{public}s', 'loadContent done');
    });
  }

  onWindowStageDestroy(): void {
  }

  onForeground(): void {
  }

  onBackground(): void {
  }
}
