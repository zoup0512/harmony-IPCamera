import { pasteboard } from '@kit.BasicServicesKit';
import { OnvifDevice, RtspServer } from '../model/RtspServer';
import { fitsDeviceStore, publicEndpoint, saveDevice } from '../util/DeviceStore';

import { ANDROID_BG, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT } from '../components/AndroidTheme';
import { MgrAction, MgrIcon, MgrMore, MgrScanProgress, MgrToolbar } from '../components/MgrToolbar';

@Entry
@Component
struct OnvifScan {
  @State scanning: boolean = false;
  @State saving: boolean = false;
  @State searched: boolean = false;
  @State devices: OnvifDevice[] = [];
  @State added: string[] = [];
  private server: RtspServer = new RtspServer();

  private generation: number = 0;

  onPageHide(): void { this.generation++; }

  notice(message: Resource): void {
    this.getUIContext().getPromptAction().showToast({ message: message });
  }

  async onScan(): Promise<void> {
    if (this.scanning || this.saving) return;
    const gen = ++this.generation;
    this.scanning = true;
    try {
      const result = await this.server.scanOnvif(4000);
      if (gen !== this.generation) return;
      this.devices = result;
      this.searched = true;
    } catch (_) {
      if (gen === this.generation) this.notice($r('app.string.scan_discovery_failed'));
    } finally {
      this.scanning = false;
    }
  }

  async add(dev: OnvifDevice): Promise<void> {
    if (this.saving || this.added.includes(dev.streamUri)) return;
    if (!fitsDeviceStore({ name: dev.model, url: dev.streamUri })) {
      this.notice($r('app.string.scan_too_long'));
      return;
    }
    this.saving = true;
    try {
      await saveDevice('', dev.model, dev.streamUri);
      this.added = this.added.concat([dev.streamUri]);
      this.notice($r('app.string.scan_device_added'));
    } catch (_) {
      this.notice($r('app.string.scan_store_error'));
    } finally {
      this.saving = false;
    }
  }

  async copy(value: string): Promise<void> {
    try {
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, value);
      const options = data.getProperty();
      options.shareOption = pasteboard.ShareOption.LOCALDEVICE;
      data.setProperty(options);
      await pasteboard.getSystemPasteboard().setData(data);
      this.notice($r('app.string.scan_copied'));
    } catch (_) {
      this.notice($r('app.string.scan_copy_error'));
    }
  }

  private inspect(dev: OnvifDevice): void {
    if (this.scanning || this.saving) return;
    const gen = this.generation;
    const endpoint = publicEndpoint(dev.streamUri);
    const service = publicEndpoint(dev.xaddrs.split(/\s+/)[0]);
    const added = this.added.includes(dev.streamUri);
    const details = dev.profile ? service + '\n' + getContext(this).resourceManager
      .getStringSync($r('app.string.scan_profile').id, dev.profile) : service;
    this.getUIContext().showAlertDialog({
      title: dev.model || $r('app.string.scan_unnamed'), subtitle: details,
      message: endpoint ? $r('app.string.mgr_ui_confirm_device', endpoint) : $r('app.string.scan_no_stream'),
      buttonDirection: DialogButtonDirection.VERTICAL,
      buttons: [
        { value: added ? $r('app.string.scan_added') : $r('app.string.scan_add_device'), enabled: !!endpoint && !added,
          action: () => { if (gen === this.generation) this.add(dev); } },
        { value: $r('app.string.scan_copy'), enabled: !!endpoint,
          action: () => { if (gen === this.generation) this.copy(dev.streamUri); } },
        { value: $r('app.string.pub_cancel'), action: () => {} }
      ]
    });
  }

  build() {
    WithTheme({ colorMode: ThemeColorMode.DARK }) {
      Column() {
        MgrToolbar({ title: $r('app.string.scan_discovery_title') }) {
          MgrIcon({ icon: $r('sys.symbol.magnifyingglass'), label: $r('app.string.scan_scan_network'),
            enabledAction: !this.scanning && !this.saving, action: () => this.onScan() })
          MgrMore({ enabledAction: !this.scanning && !this.saving }) { this.more() }
        }
        if (this.scanning) {
          MgrScanProgress()
        }
        if (this.devices.length) {
          List() {
            ForEach(this.devices, (dev: OnvifDevice) => {
              ListItem() {
                Row({ space: 10 }) {
                  Image($r('app.media.mgr_ui_onvif')).width(48).height(48).accessibilityLevel('no')
                  Column({ space: 2 }) {
                    Text(dev.model || $r('app.string.scan_unnamed')).fontSize(20).fontColor(ANDROID_TEXT)
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                    Text(publicEndpoint(dev.xaddrs.split(/\s+/)[0]) || publicEndpoint(dev.streamUri))
                      .fontSize(12).fontWeight(FontWeight.Bold).fontColor('#8080FF').maxLines(1)
                      .textOverflow({ overflow: TextOverflow.Ellipsis })
                  }.layoutWeight(1).alignItems(HorizontalAlign.Start)
                  if (this.added.includes(dev.streamUri)) {
                    Text($r('app.string.scan_added')).fontSize(12).fontColor(ANDROID_SECONDARY)
                  }
                }.width('100%').padding(5).enabled(!this.scanning && !this.saving).onClick(() => this.inspect(dev))
              }
            }, (_dev: OnvifDevice, index: number) => `${this.generation}-${index}`)
          }.layoutWeight(1).width('100%').divider({ strokeWidth: 1, color: ANDROID_DIVIDER })
        } else {
          Column({ space: 8 }) {
            Text(this.scanning ? $r('app.string.scan_scanning') : this.searched ? $r('app.string.scan_discovery_empty') : $r('app.string.scan_discovery_ready'))
              .fontSize(20).fontColor(ANDROID_TEXT).textAlign(TextAlign.Center)
            if (!this.scanning) {
              Text($r('app.string.scan_discovery_hint')).fontSize(14).fontColor(ANDROID_SECONDARY).textAlign(TextAlign.Center)
              MgrAction({ label: $r('app.string.scan_scan_network'), enabledAction: !this.saving, action: () => this.onScan() })
            }
          }.width('100%').layoutWeight(1).justifyContent(FlexAlign.Center).padding(20)
        }
      }.width('100%').height('100%').backgroundColor(ANDROID_BG)
    }
  }

  @Builder more() {
    Menu() {
      MenuItem({ content: $r('app.string.scan_manage'), labelInfo: 'HarmonyOS' })
        .onClick(() => this.getUIContext().getRouter().pushUrl({ url: 'pages/OnvifManaged' }))
    }
  }
}
