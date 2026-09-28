import { webview } from '@kit.ArkWeb';
import { wifiManager } from '@kit.ConnectivityKit';
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { pasteboard } from '@kit.BasicServicesKit';
import { promptAction } from '@kit.ArkUI';

@Entry
@Component
struct Player {
  @State url: string = '';
  @State loadedUrl: string = '';
  @State webSrc: string = '';
  controller: WebController = new WebController();

  aboutToAppear(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    try {
      const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
      const port = pref.getSync('httpPort', 8081) as number;
      const pw = pref.getSync('password', 'admin') as string;
      const ip = this.deviceIp();
      this.url = `http://${ip}:${port}/video?pw=${pw}`;
    } catch (err) {
      this.url = '';
    }
  }

  deviceIp(): string {
    try {
      const info: wifiManager.IpInfo = wifiManager.getIpInfo();
      const ip: number = info.ipAddress >>> 0;  // network-order (big-endian)
      return `${(ip >>> 24) & 0xFF}.${(ip >>> 16) & 0xFF}.${(ip >>> 8) & 0xFF}.${ip & 0xFF}`;
    } catch (err) {
      return '127.0.0.1';
    }
  }

  onPlay(): void {
    const target = this.url.trim();
    if (target === '') return;
    if (target.startsWith('rtsp://') || target.startsWith('rtsps://')) {
      // HarmonyOS has no embeddable RTSP player; copy for VLC/ffplay instead.
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, target);
      pasteboard.getSystemPasteboard().setDataSync(data);
      promptAction.showToast({ message: '系统无内嵌 RTSP 播放器,地址已复制,请用 VLC 打开' });
      return;
    }
    this.webSrc = target;
    this.loadedUrl = target;
    // MJPEG streams never finish loading; force a render pass.
    this.controller.refresh();
  }

  build() {
    Scroll() {
      Column({ space: 10 }) {
        Text('实时画面播放')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .margin({ top: 14 })

        Row({ space: 8 }) {
          TextInput({ text: this.url, placeholder: 'http://ip:8081/video?pw=密码' })
            .fontSize(12)
            .layoutWeight(1)
            .onChange((v: string) => {
              this.url = v;
            })
          Button('播放').onClick(() => this.onPlay())
        }
        .width('92%')

        Text('默认为本机网页控制台的 MJPEG 流(?pw= 免 Basic 认证参数);'
          + 'rtsp:// 地址鸿蒙无内嵌播放器,点击播放会复制到剪贴板供 VLC 使用。')
          .fontSize(11)
          .fontColor('#757575')
          .width('88%')
          .textAlign(TextAlign.Center)

        if (this.webSrc !== '') {
          Web({ src: this.webSrc, controller: this.controller })
            .width('92%')
            .height(300)
            .borderRadius(8)
            .backgroundColor('#000000')
            .javaScriptAccess(false)
            .onlineImageAccess(true)
            .fileAccess(false)
        } else {
          Column() {
            Text('输入地址后点击播放').fontSize(12).fontColor('#9E9E9E')
          }
          .width('92%')
          .height(300)
          .borderRadius(8)
          .backgroundColor('#111111')
          .justifyContent(FlexAlign.Center)
        }
      }
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
