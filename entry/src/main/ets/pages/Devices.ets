import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { BusinessError } from '@kit.BasicServicesKit';
import { scanBarcode, scanCore } from '@kit.ScanKit';
import { DeviceDraft, DeviceItem, deleteDevice, fitsDeviceStore, listDevices, publicEndpoint, readDevice,
  readDeviceQr, saveDevice } from '../util/DeviceStore';

import { ANDROID_ACCENT, ANDROID_BAR, ANDROID_BG, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT,
  ANDROID_TOOLBAR_HEIGHT } from '../components/AndroidTheme';

// A process launch preference, not an on-resume or every-navigation preference.
let checkedAutoStart = false;
const FAB_SIZE = 56;
// The Android widget's shadow insets add to main.xml's 10/18dp layout margins.
const FAB_RIGHT = 15;
const FAB_BOTTOM = 25;
const SERVICE_HEIGHT = 54;

@Entry
@Component
struct Devices {
  @State items: DeviceItem[] = [];
  @State query: string = '';
  @State editing: boolean = false;
  @State name: string = '';
  @State address: string = '';
  @State busy: boolean = false;
  @State loading: boolean = true;
  @State loadFailed: boolean = false;
  @State canScan: boolean = false;
  @State adding: boolean = false;
  @State searching: boolean = false;
  @State rowMenuId: string = '';
  @State fabHidden: boolean = false;
  @State pageWidth: number = 360;
  private firstRow: number = 0;
  private editId: string = '';
  private generation: number = 0;

  aboutToAppear(): void {
    this.canScan = canIUse('SystemCapability.Multimedia.Scan.ScanBarcode') &&
      canIUse('SystemCapability.Multimedia.Scan.Core');
  }

  onPageShow(): void {
    if (!checkedAutoStart) {
      checkedAutoStart = true;
      try {
        const ctx = getContext(this) as common.UIAbilityContext;
        const settings = preferences.getPreferencesSync(ctx, { name: 'ipcamSettings' });
        const autoRtsp = settings.getSync('auto_rtsp_after_turn_on', false) === true;
        const autoRtmp = settings.getSync('auto_rtmp_after_turn_on', false) === true;
        if (autoRtsp || autoRtmp) {
          this.open('pages/Index', true);
        }
      } catch (_) {
        this.notice($r('app.string.dm_settings_error'));
      }
    }
    this.refresh();
  }

  onBackPress(): boolean {
    if (this.busy) return true;
    if (this.rowMenuId !== '') {
      this.rowMenuId = '';
    } else if (this.adding) {
      this.adding = false;
    } else if (this.editing) {
      this.closeEditor();
    } else if (this.searching) {
      this.closeSearch();
    } else {
      return false;
    }
    return true;
  }

  closeSearch(): void {
    this.query = '';
    this.searching = false;
    this.fabHidden = false;
  }

  onPageHide(): void {
    this.adding = false;
    this.rowMenuId = '';
    this.fabHidden = false;
  }

  rowActions(item: DeviceItem): MenuElement[] {
    return [
      { value: $r('app.string.dm_edit'), action: () => { this.rowMenuId = ''; this.edit(item); } },
      { value: $r('app.string.dm_delete'), action: () => { this.rowMenuId = ''; this.confirmDelete(item); } }
    ];
  }

  // As in the reference, get out of the way when moving down the list. Restore the
  // button on scroll stop too: reaching the last row must not strand the add action.
  onListPosition(first: number): void {
    if (first !== this.firstRow && !this.adding) this.fabHidden = first > this.firstRow;
    this.firstRow = first;
  }

  notice(message: Resource): void {
    this.getUIContext().getPromptAction().showToast({ message: message });
  }

  async refresh(): Promise<void> {
    const generation = ++this.generation;
    this.loading = true;
    this.loadFailed = false;
    try {
      const rows = await listDevices();
      if (generation === this.generation) this.items = rows;
    } catch (_) {
      if (generation === this.generation) this.loadFailed = true;
    } finally {
      if (generation === this.generation) this.loading = false;
    }
  }

  matches(): DeviceItem[] {
    const needle = this.query.trim().toLowerCase();
    return this.items.filter((item: DeviceItem) =>
      item.name.toLowerCase().includes(needle) || item.endpoint.toLowerCase().includes(needle));
  }

  async open(page: string, coldAutoStart: boolean = false): Promise<void> {
    try {
      const router = this.getUIContext().getRouter();
      if (coldAutoStart) {
        await router.pushUrl({ url: page, params: { coldAutoStart: true } });
      } else {
        await router.pushUrl({ url: page });
      }
    } catch (_) {
      this.notice($r('app.string.dm_open_error'));
    }
  }

  async openSettings(): Promise<void> {
    try {
      await this.getUIContext().getRouter().pushUrl({ url: 'pages/Index', params: { section: 'settings' } });
    } catch (_) {
      this.notice($r('app.string.dm_open_error'));
    }
  }

  async play(item: DeviceItem): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    try {
      // Never put a saved URL (stream keys, signed queries, userinfo) in navigation state.
      await this.getUIContext().getRouter().pushUrl({ url: 'pages/Player', params: { deviceId: item.id } });
    } catch (_) {
      this.notice($r('app.string.dm_open_error'));
    } finally {
      this.busy = false;
    }
  }

  startEditor(id: string, draft: DeviceDraft): void {
    this.editId = id;
    this.name = draft.name;
    this.address = draft.url;
    this.editing = true;
  }

  closeEditor(): void {
    this.editing = false;
    this.editId = '';
    this.address = '';
    this.name = '';
  }

  async edit(item: DeviceItem): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    try {
      this.startEditor(item.id, await readDevice(item.id));
    } catch (_) {
      this.notice($r('app.string.dm_store_error'));
    } finally {
      this.busy = false;
    }
  }

  async scan(): Promise<void> {
    if (!this.canScan || this.busy) return;
    this.busy = true;
    try {
      const result = await scanBarcode.startScanForResult(getContext(this) as common.UIAbilityContext,
        { scanTypes: [scanCore.ScanType.QR_CODE], enableAlbum: true });
      try {
        this.startEditor('', readDeviceQr(result.originalValue));
      } catch (_) {
        this.notice($r('app.string.dm_invalid_qr'));
      }
    } catch (err) {
      const code = (err as BusinessError).code;
      if (code === 801) this.canScan = false;
      if (code !== scanCore.ScanErrorCode.SCAN_SERVICE_CANCELED) this.notice($r('app.string.dm_scan_error'));
    } finally {
      this.busy = false;
    }
  }

  async save(): Promise<void> {
    if (this.busy) return;
    const draft: DeviceDraft = { name: this.name.trim(), url: this.address.trim() };
    if (!publicEndpoint(draft.url)) {
      this.notice($r('app.string.viewer_device_invalid_url'));
      return;
    }
    if (!fitsDeviceStore(draft)) {
      this.notice($r('app.string.dm_too_long'));
      return;
    }
    this.busy = true;
    try {
      await saveDevice(this.editId, draft.name, draft.url);
      this.query = '';
      this.closeEditor();
      await this.refresh();
    } catch (_) {
      this.notice($r('app.string.dm_store_error'));
    } finally {
      this.busy = false;
    }
  }

  confirmDelete(item: DeviceItem): void {
    this.getUIContext().showAlertDialog({
      title: $r('app.string.dm_confirm_delete'), message: item.name,
      primaryButton: { value: $r('app.string.dm_cancel'), action: () => {} },
      secondaryButton: { value: $r('app.string.dm_delete'), fontColor: '#AD3131', action: () => this.remove(item.id) }
    });
  }

  async remove(id: string): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    try {
      await deleteDevice(id);
      await this.refresh();
    } catch (_) {
      this.notice($r('app.string.dm_store_error'));
    } finally {
      this.busy = false;
    }
  }

  @Builder
  editor() {
    Column({ space: 12 }) {
      Text(this.editId ? $r('app.string.dm_edit_device') : $r('app.string.dm_add_device'))
        .fontSize(22).fontWeight(FontWeight.Medium).fontColor(ANDROID_TEXT)
      Text($r('app.string.dm_name')).fontSize(14).fontColor(ANDROID_SECONDARY)
      TextInput({ text: this.name, placeholder: $r('app.string.dm_name_hint') })
        .accessibilityText($r('app.string.dm_name'))
        .maxLength(80).onChange((value: string) => { this.name = value; })
      Text($r('app.string.dm_url')).fontSize(14).fontColor(ANDROID_SECONDARY)
      TextInput({ text: this.address, placeholder: $r('app.string.viewer_device_url_hint') })
        .type(InputType.Password).showPasswordIcon(false).enableAutoFill(false)
        .accessibilityText($r('app.string.dm_url'))
        .onChange((value: string) => { this.address = value; })
      Text($r('app.string.dm_auth_hint')).fontSize(14).fontColor(ANDROID_SECONDARY)
      Button($r('app.string.dm_save')).width('100%').backgroundColor(ANDROID_ACCENT).fontColor(ANDROID_BAR)
        .onClick(() => this.save())
      Button($r('app.string.dm_cancel')).width('100%').backgroundColor(ANDROID_BAR).fontColor(ANDROID_TEXT)
        .onClick(() => this.closeEditor())
    }.padding(16).width('100%').alignItems(HorizontalAlign.Start).enabled(!this.busy)
  }

  @Builder
  icon(symbol: Resource, size: number, color: string) {
    SymbolGlyph(symbol).fontSize(`${size}vp`).fontColor([color]).accessibilityLevel('no')
  }

  @Builder
  homeToolbar() {
    Row({ space: 4 }) {
      if (this.searching) {
        Button({ type: ButtonType.Circle }) { this.icon($r('sys.symbol.arrow_left'), 23, ANDROID_TEXT) }
          .width(48).height(48).padding(0).backgroundColor(Color.Transparent)
          .accessibilityText($r('app.string.home_ui_close_search')).onClick(() => this.closeSearch())
        Search({ value: this.query, placeholder: $r('app.string.dm_search') })
          .layoutWeight(1).defaultFocus(true).accessibilityText($r('app.string.dm_search'))
          .onChange((value: string) => { this.query = value; this.fabHidden = false; })
      } else {
        Text($r('app.string.home_ui_title')).fontSize(20).fontWeight(FontWeight.Medium).fontColor(ANDROID_TEXT)
          .layoutWeight(1)
        Button({ type: ButtonType.Circle }) { this.icon($r('sys.symbol.magnifyingglass'), 24, ANDROID_TEXT) }
          .width(48).height(48).padding(0).backgroundColor(Color.Transparent)
          .accessibilityText($r('app.string.dm_search')).onClick(() => { this.searching = true; })
        Button({ type: ButtonType.Circle }) { this.icon($r('sys.symbol.gearshape'), 24, ANDROID_TEXT) }
          .width(48).height(48).padding(0).backgroundColor(Color.Transparent)
          .accessibilityText($r('app.string.home_ui_settings')).onClick(() => this.openSettings())
      }
    }.width('100%').constraintSize({ minHeight: ANDROID_TOOLBAR_HEIGHT })
      .padding({ left: this.searching ? 4 : 16, right: 4 }).backgroundColor(ANDROID_BAR)
  }

  @Builder
  deviceRow(item: DeviceItem) {
    Row() {
      Button({ type: ButtonType.Normal }) {
        Row({ space: 10 }) {
          Image($r('app.media.home_ui_camera')).width(48).height(48).objectFit(ImageFit.Contain)
            .accessibilityLevel('no')
          Column() {
            Text(item.name).fontSize(20).fontColor(ANDROID_TEXT)
              .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
            Text(item.endpoint).fontSize(12).fontWeight(FontWeight.Bold).fontColor('#A6A6FF')
              .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        }.width('100%').padding(5)
      }.layoutWeight(1).height('auto').padding(0).backgroundColor(Color.Transparent)
        .accessibilityText($r('app.string.home_ui_play_camera', item.name, item.endpoint))
        .onClick(() => { if (!this.rowMenuId) this.play(item); })
        .gesture(LongPressGesture({ repeat: false }).onAction(() => { this.rowMenuId = item.id; }))
      Button({ type: ButtonType.Circle }) { this.icon($r('sys.symbol.more'), 22, ANDROID_SECONDARY) }
        .width(48).height(48).padding(0).backgroundColor(Color.Transparent)
        .accessibilityText($r('app.string.home_ui_camera_actions', item.name))
        .onClick(() => { this.rowMenuId = item.id; })
        .bindMenu(this.rowMenuId === item.id, this.rowActions(item), {
          onDisappear: () => { if (this.rowMenuId === item.id) this.rowMenuId = ''; }
        })
    }.width('100%')
  }

  @Builder
  listArea() {
    if (this.loading || this.loadFailed || this.matches().length === 0) {
      Scroll() {
        Column({ space: 8 }) {
          if (this.loading) {
            Text($r('app.string.dm_loading')).fontColor(ANDROID_SECONDARY)
          } else if (this.loadFailed) {
            Text($r('app.string.dm_store_error')).textAlign(TextAlign.Center).fontColor(ANDROID_TEXT)
            Button($r('app.string.dm_retry')).onClick(() => this.refresh())
          } else if (this.items.length > 0) {
            Text($r('app.string.dm_no_matches')).textAlign(TextAlign.Center).fontColor(ANDROID_TEXT)
            Text($r('app.string.dm_search_hint')).textAlign(TextAlign.Center).fontColor(ANDROID_SECONDARY)
          } else {
            Image($r('app.media.home_ui_empty')).width(112).height(112).objectFit(ImageFit.Contain).opacity(0.8)
              .accessibilityText($r('app.string.dm_empty'))
          }
        }.width('100%').padding(12)
      }.width('100%').height('100%').align(Alignment.Center)
        .contentEndOffset(this.loadFailed ? FAB_SIZE + FAB_BOTTOM : 0)
    } else {
      List() {
        ForEach(this.matches(), (item: DeviceItem) => {
          ListItem() { this.deviceRow(item) }
        }, (item: DeviceItem) => JSON.stringify(item))
      }.width('100%').height('100%').divider({ strokeWidth: 1, color: ANDROID_DIVIDER })
        .contentEndOffset(FAB_SIZE + FAB_BOTTOM - SERVICE_HEIGHT + 8)
        .onScrollIndex((first: number) => this.onListPosition(first))
        .onScrollStop(() => { this.fabHidden = false; })
    }
  }

  @Builder
  addChoice(label: Resource, image: Resource, iconSize: number, available: boolean, action: () => void) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 10 }) {
        Text(label).fontSize(14).fontColor(ANDROID_TEXT).backgroundColor('#333333')
          .borderRadius(4).padding({ left: 8, right: 8, top: 6, bottom: 6 }).flexShrink(1)
        Stack() {
          Image(image).width(iconSize).height(iconSize).objectFit(ImageFit.Contain).accessibilityLevel('no')
        }.width(40).height(40).borderRadius(20).backgroundColor(ANDROID_ACCENT)
      }.width('100%').justifyContent(FlexAlign.End).padding({ top: 4, bottom: 4 })
    }.width('100%').height('auto').padding(0).backgroundColor(Color.Transparent)
      .enabled(available).accessibilityText(label)
      .onClick(() => { this.adding = false; action(); })
  }

  @Builder
  addChoices() {
    Scroll() {
      Column({ space: 6 }) {
        this.addChoice($r('app.string.home_ui_add_onvif'), $r('app.media.home_ui_onvif'), 24, true,
          () => { this.open('pages/OnvifScan'); })
        this.addChoice($r('app.string.home_ui_add_ip'), $r('app.media.home_ui_add'), 32, true,
          () => { this.startEditor('', { name: '', url: '' }); })
        this.addChoice(this.canScan ? $r('app.string.home_ui_scan_qr') : $r('app.string.home_ui_scan_unavailable'),
          $r('app.media.home_ui_qrcode'), 32, this.canScan, () => { this.scan(); })
      }.width('100%').padding({ left: 4, right: 8 })
    }.scrollBar(BarState.Auto)
  }

  @Builder
  fab() {
    Button({ type: ButtonType.Circle }) {
      this.icon(this.adding ? $r('sys.symbol.xmark') : $r('sys.symbol.plus'), 20, ANDROID_TEXT)
    }.width(FAB_SIZE).height(FAB_SIZE).padding(0).backgroundColor(ANDROID_ACCENT).shadow(ShadowStyle.OUTER_DEFAULT_MD)
      .margin({ right: FAB_RIGHT, bottom: FAB_BOTTOM })
      .accessibilityText(this.adding ? $r('app.string.home_ui_close_add') : $r('app.string.dm_add_device'))
      .onClick(() => { this.adding = !this.adding; })
      .bindPopup(this.adding, { builder: this.addChoices, placement: Placement.TopRight, targetSpace: 10,
        width: Math.max(1, Math.min(252, this.pageWidth - 20)), enableArrow: false, popupColor: Color.Transparent, backgroundBlurStyle: BlurStyle.NONE,
        shadow: { radius: 0 }, radius: 0, mask: { color: Color.Transparent }, focusable: true, autoCancel: true,
        onStateChange: (event) => { if (!event.isVisible) this.adding = false; } })
  }

  @Builder
  serviceBar() {
    Divider().strokeWidth(1).color(ANDROID_DIVIDER)
    Button({ type: ButtonType.Normal }) {
      Row() {
        Image($r('app.media.home_ui_wifi')).width(32).height(32).objectFit(ImageFit.Contain).accessibilityLevel('no')
        Text($r('app.string.home_ui_local_service')).fontSize(14).fontColor(ANDROID_TEXT)
          .textAlign(TextAlign.Center).layoutWeight(1).margin({ left: this.pageWidth >= 320 ? 67 : 6 })
      }.width('100%').padding({ left: 10, right: FAB_RIGHT + FAB_SIZE + 6, top: 6, bottom: 6 })
    }.width('100%').height('auto').constraintSize({ minHeight: SERVICE_HEIGHT }).padding(0).backgroundColor(ANDROID_BG)
      .accessibilityText($r('app.string.home_ui_local_service')).onClick(() => this.open('pages/Index'))
  }

  build() {
    // Keep the default system/keyboard safe area. In the reference the 56dp FAB
    // straddles the footer divider. Only trailing, unlabelled footer space is covered.
    Column() {
      if (this.editing) {
        Scroll() { this.editor() }.width('100%').height('100%').align(Alignment.TopStart)
      } else {
        this.homeToolbar()
        Stack({ alignContent: Alignment.BottomEnd }) {
          Column() {
            Stack() { this.listArea() }.layoutWeight(1).width('100%').clip(true)
            this.serviceBar()
          }.height('100%').width('100%')
          if (!this.fabHidden) { this.fab() }
        }.layoutWeight(1).width('100%')
      }
    }.width('100%').height('100%').backgroundColor(ANDROID_BG).enabled(!this.busy)
      .onAreaChange((_old: Area, area: Area) => {
        this.pageWidth = Number(area.width);
      })
  }
}
