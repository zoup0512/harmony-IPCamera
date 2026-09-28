import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';

interface AddrEntry {
  url: string;
}

@Entry
@Component
struct RtmpAddrs {
  @State list: AddrEntry[] = [];
  @State input: string = '';
  @State current: string = '';

  aboutToAppear(): void {
    this.reload();
  }

  reload(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    try {
      const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
      const raw = pref.getSync('rtmpAddrList', '[]') as string;
      const parsed = JSON.parse(raw) as AddrEntry[];
      this.list = Array.isArray(parsed) ? parsed : [];
      this.current = pref.getSync('rtmpUrl', '') as string;
    } catch (err) {
      this.list = [];
    }
  }

  persist(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    try {
      const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
      pref.putSync('rtmpAddrList', JSON.stringify(this.list));
      pref.flush();
    } catch (err) {
      promptAction.showToast({ message: '保存失败' });
    }
  }

  add(): void {
    const url = this.input.trim();
    if (!url.startsWith('rtmp://')) {
      promptAction.showToast({ message: '地址需以 rtmp:// 开头' });
      return;
    }
    if (this.list.some((e: AddrEntry) => e.url === url)) {
      promptAction.showToast({ message: '地址已存在' });
      return;
    }
    this.list = [{ url: url }, ...this.list];
    this.persist();
    this.input = '';
    promptAction.showToast({ message: '已添加' });
  }

  setCurrent(url: string): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    try {
      const pref = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
      pref.putSync('rtmpUrl', url);
      pref.flush();
      this.current = url;
      promptAction.showToast({ message: '已设为推流地址(返回主页生效)' });
    } catch (err) {
      promptAction.showToast({ message: '设置失败' });
    }
  }

  remove(url: string): void {
    this.list = this.list.filter((e: AddrEntry) => e.url !== url);
    this.persist();
  }

  build() {
    Scroll() {
      Column({ space: 10 }) {
        Text('RTMP 地址管理')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .margin({ top: 14 })

        Row({ space: 8 }) {
          TextInput({ text: this.input, placeholder: 'rtmp://host:1935/app/streamkey' })
            .fontSize(12)
            .layoutWeight(1)
            .onChange((v: string) => {
              this.input = v;
            })
          Button('添加').onClick(() => this.add())
        }
        .width('92%')

        Text(`当前推流地址: ${this.current === '' ? '(未设置)' : this.current}`)
          .fontSize(11)
          .fontColor('#757575')
          .width('92%')

        ForEach(this.list, (entry: AddrEntry) => {
          Row({ space: 8 }) {
            Text(entry.url)
              .fontSize(12)
              .layoutWeight(1)
              .fontColor(entry.url === this.current ? '#0A7D32' : '#333333')
              .copyOption(CopyOptions.LocalDevice)
            if (entry.url !== this.current) {
              Button('使用').fontSize(12).height(30)
                .onClick(() => this.setCurrent(entry.url))
            } else {
              Text('当前').fontSize(11).fontColor('#0A7D32')
            }
            Button('删除').fontSize(12).height(30)
              .backgroundColor('#B3261E')
              .onClick(() => this.remove(entry.url))
          }
          .padding(10)
          .borderRadius(8)
          .backgroundColor('#F5F5F5')
          .width('92%')
        }, (entry: AddrEntry) => entry.url)

        if (this.list.length === 0) {
          Text('暂无保存的地址').fontSize(12).fontColor('#9E9E9E')
        }
      }
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
