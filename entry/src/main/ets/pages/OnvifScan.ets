import { hilog } from '@kit.PerformanceAnalysisKit';
import { pasteboard } from '@kit.BasicServicesKit';
import { promptAction } from '@kit.ArkUI';
import { OnvifDevice, RtspServer } from '../model/RtspServer';

const TAG: string = 'OnvifScanPage';

interface ScanState {
  scanning: boolean;
  devices: OnvifDevice[];
}

@Entry
@Component
struct OnvifScan {
  @State scanning: boolean = false;
  @State devices: OnvifDevice[] = [];
  private server: RtspServer = new RtspServer();

  async onScan(): Promise<void> {
    if (this.scanning) return;
    this.scanning = true;
    try {
      const found: Array<OnvifDevice> = await this.server.scanOnvif(4000);
      this.devices = found;
      promptAction.showToast({ message: `发现 ${found.length} 台 ONVIF 设备` });
    } catch (err) {
      hilog.warn(0xC010, TAG, 'scan failed');
      promptAction.showToast({ message: '扫描失败' });
    }
    this.scanning = false;
  }

  copy(text: string): void {
    if (text === '') return;
    const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
    pasteboard.getSystemPasteboard().setDataSync(data);
    promptAction.showToast({ message: '已复制' });
  }

  build() {
    Scroll() {
      Column({ space: 10 }) {
        Text('ONVIF 设备扫描')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .margin({ top: 14 })

        Button(this.scanning ? '扫描中…' : '扫描网络 (WS-Discovery)')
          .width('80%')
          .enabled(!this.scanning)
          .onClick(() => this.onScan())

        Text('扫描同一 WLAN 内的 ONVIF 网络摄像头,读取媒体 Profile 与 RTSP 流地址。'
          + '流地址可复制后用 VLC/ffplay 打开,或填入其他支持 RTSP 的播放器。')
          .fontSize(11)
          .fontColor('#757575')
          .width('88%')
          .textAlign(TextAlign.Center)

        ForEach(this.devices, (dev: OnvifDevice, idx: number) => {
          Column({ space: 6 }) {
            Row({ space: 8 }) {
              Text(`#${idx + 1}`).fontSize(14).fontWeight(FontWeight.Bold).width(36)
              Column({ space: 2 }) {
                Text(dev.model !== '' ? dev.model : '未命名设备').fontSize(15)
                Text(dev.xaddrs).fontSize(11).fontColor('#1A56B0')
                  .copyOption(CopyOptions.LocalDevice)
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
            }
            .width('100%')

            if (dev.streamUri !== '') {
              Text(dev.streamUri)
                .fontSize(12)
                .fontColor('#0A7D32')
                .copyOption(CopyOptions.LocalDevice)
                .width('100%')
              Row({ space: 8 }) {
                Button('复制流地址').fontSize(12).height(32)
                  .onClick(() => this.copy(dev.streamUri))
                Text(`profile: ${dev.profile}`).fontSize(11).fontColor('#757575')
              }
            } else {
              Text(dev.error !== '' ? `流地址获取失败: ${dev.error}` : '无媒体 Profile')
                .fontSize(11)
                .fontColor('#B3261E')
                .width('100%')
            }
          }
          .padding(10)
          .borderRadius(8)
          .backgroundColor('#F5F5F5')
          .width('92%')
          .alignItems(HorizontalAlign.Start)
        }, (dev: OnvifDevice, idx: number) => `${dev.xaddrs}-${idx}`)

        if (!this.scanning && this.devices.length === 0) {
          Text('暂无设备 — 点击扫描开始').fontSize(12).fontColor('#9E9E9E')
        }
      }
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
