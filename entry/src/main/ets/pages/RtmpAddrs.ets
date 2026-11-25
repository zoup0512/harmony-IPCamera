import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';
import { RtmpConfig, RtmpProfile, readProfiles, makeProfile, endpointLabel, editAddress, editKey,
  saveProfile, selectProfile, deleteProfile } from '../util/RtmpProfiles';

import { ANDROID_ACCENT, ANDROID_BG, ANDROID_BAR, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT, ANDROID_TOOLBAR_HEIGHT } from '../components/AndroidTheme';
import { MgrAction, MgrIcon, MgrMore, MgrToolbar } from '../components/MgrToolbar';

interface AddressRow {
  id: string;
  index: number;
  name: ResourceStr;
  endpoint: ResourceStr;
  selected: boolean;
}

@Entry
@Component
struct RtmpAddrs {
  private config: RtmpConfig = { list: [], current: '' };
  private revision: number = 0;
  @State editorOpen: boolean = false;
  @State renameOnly: boolean = false;
  @State searching: boolean = false;
  @State query: string = '';
  @State rows: AddressRow[] = [];
  @State ready: boolean = false;
  @State currentLabel: ResourceStr = '';
  @State editing: number = -1;
  @State name: string = '';
  @State address: string = '';
  @State keyInput: string = '';

  aboutToAppear(): void {
    try {
      const pref = this.prefs();
      const raw = pref.getSync('rtmpAddrList', '[]');
      const current = pref.getSync('rtmpUrl', '');
      if (typeof raw !== 'string' || typeof current !== 'string') throw new Error();
      this.renderConfig({ list: readProfiles(raw), current: current });
      this.ready = true;
    } catch (_) {
      this.ready = false; // Don't overwrite an unreadable list with an empty one.
    }
  }

  private prefs(): preferences.Preferences {
    return preferences.getPreferencesSync(getContext(this) as common.UIAbilityContext, { name: 'ipcamSettings' });
  }

  private renderConfig(next: RtmpConfig): void {
    this.config = next;
    this.revision++;
    const endpoint = endpointLabel(next.current, '');
    this.currentLabel = !next.current ? $r('app.string.pub_current_none') :
      endpoint ? $r('app.string.pub_current', endpoint) : $r('app.string.pub_current_hidden');
    this.rows = next.list.map((p: RtmpProfile, i: number): AddressRow => {
      return { id: `${this.revision}-${i}`, index: i, name: p.name || $r('app.string.pub_unnamed'),
        endpoint: endpointLabel(p.url, '') || $r('app.string.pub_hidden'), selected: p.url !== '' && p.url === next.current };
    });
  }

  private persist(next: RtmpConfig): boolean {
    if (!this.ready) return false;
    const listChanged = next.list !== this.config.list;
    const currentChanged = next.current !== this.config.current;
    let hadList = false;
    let hadCurrent = false;
    let wroteList = false;
    let wroteCurrent = false;
    try {
      const pref = this.prefs();
      if (listChanged) {
        hadList = pref.hasSync('rtmpAddrList');
        pref.putSync('rtmpAddrList', JSON.stringify(next.list));
        wroteList = true;
      }
      if (currentChanged) {
        hadCurrent = pref.hasSync('rtmpUrl');
        pref.putSync('rtmpUrl', next.current);
        wroteCurrent = true;
      }
      pref.flushSync();
    } catch (_) {
      // Restore the shared cache too. For Index a missing rtmpUrl is not an empty rtmpUrl.
      try {
        const pref = this.prefs();
        if (wroteList) {
          if (hadList) pref.putSync('rtmpAddrList', JSON.stringify(this.config.list));
          else pref.deleteSync('rtmpAddrList');
        }
        if (wroteCurrent) {
          if (hadCurrent) pref.putSync('rtmpUrl', this.config.current);
          else pref.deleteSync('rtmpUrl');
        }
        if (wroteList || wroteCurrent) pref.flushSync();
      } catch (_) {
        this.ready = false;
      }
      promptAction.showToast({ message: $r('app.string.pub_save_failed') });
      return false;
    }
    this.renderConfig(next);
    return true;
  }

  private clearEditor(): void {
    this.editorOpen = false;
    this.renameOnly = false;
    this.editing = -1;
    this.name = '';
    this.address = '';
    this.keyInput = '';
  }

  private visibleRows(): AddressRow[] {
    const q = this.query.trim().toLowerCase();
    if (!q) return this.rows;
    // Filter displayed information only: never reveal a key by matching a hidden URL.
    return this.rows.filter((row: AddressRow) =>
      (typeof row.name === 'string' && row.name.toLowerCase().includes(q)) ||
      (typeof row.endpoint === 'string' && row.endpoint.toLowerCase().includes(q)));
  }

  onBackPress(): boolean {
    if (this.editorOpen) { this.clearEditor(); return true; }
    if (!this.searching) return false;
    this.searching = false;
    this.query = '';
    return true;
  }

  private edit(index: number, rename: boolean = false): void {
    const p = this.config.list[index];
    this.editing = index;
    this.name = p.name || '';
    this.address = editAddress(p);
    this.keyInput = editKey(p);
    this.renameOnly = rename;
    this.editorOpen = true;
  }

  private validationMessage(code: string): Resource {
    switch (code) {
      case 'name_required': return $r('app.string.pub_name_required');
      case 'full_url': return $r('app.string.pub_full_url');
      case 'slash': return $r('app.string.pub_slash');
      case 'key_path': return $r('app.string.pub_key_path');
      case 'format': return $r('app.string.pub_format');
      case 'duplicate': return $r('app.string.pub_duplicate');
      default: return $r('app.string.pub_invalid_profile');
    }
  }

  private save(): void {
    let next: RtmpConfig;
    try {
      let p: RtmpProfile;
      if (this.renameOnly) {
        const old = this.config.list[this.editing];
        if (!this.name.trim()) throw new Error('name_required');
        p = { name: this.name.trim(), url: old.url, keyStart: old.keyStart };
      } else {
        p = makeProfile(this.name, this.address, this.keyInput);
      }
      next = saveProfile(this.config, this.editing, p);
    } catch (err) {
      // Only helper validation errors, never a platform/persistence error containing data.
      promptAction.showToast({ message: this.validationMessage((err as Error).message) });
      return;
    }
    if (this.persist(next)) this.clearEditor();
  }

  private select(index: number): void {
    try {
      if (this.persist(selectProfile(this.config, index))) this.getUIContext().getRouter().back();
    } catch (_) {
      promptAction.showToast({ message: $r('app.string.pub_invalid_selection') });
    }
  }

  private confirmDelete(index: number): void {
    const revision = this.revision;
    this.getUIContext().showAlertDialog({
      title: $r('app.string.pub_delete_title'), subtitle: this.rows[index].name,
      message: this.rows[index].selected ? $r('app.string.pub_delete_selected') : $r('app.string.pub_delete_warning'),
      primaryButton: { value: $r('app.string.pub_cancel'), action: () => {} },
      secondaryButton: { value: $r('app.string.pub_delete'), action: () => {
        if (revision === this.revision && this.persist(deleteProfile(this.config, index))) this.clearEditor();
      } }
    });
  }

  build() {
    WithTheme({ colorMode: ThemeColorMode.DARK }) {
      Column() {
        if (this.searching) {
          Row() {
            MgrIcon({ label: $r('app.string.mgr_ui_back'), action: () => { this.query = ''; this.searching = false; } })
            Search({ value: this.query, placeholder: $r('app.string.mgr_ui_search') }).layoutWeight(1)
              .defaultFocus(true).onChange((value: string) => { this.query = value; })
          }.width('100%').height(ANDROID_TOOLBAR_HEIGHT).padding(4).backgroundColor(ANDROID_BAR)
        } else {
          MgrToolbar({ title: $r('app.string.pub_title') }) {
            MgrIcon({ icon: $r('sys.symbol.magnifyingglass'), label: $r('app.string.mgr_ui_search'),
              enabledAction: this.ready, action: () => { this.searching = true; } })
          }
        }
        Stack({ alignContent: Alignment.Center }) {
          if (!this.ready || this.visibleRows().length === 0) {
            Text(!this.ready ? $r('app.string.pub_unavailable') :
              (this.rows.length ? $r('app.string.mgr_ui_no_matches') : $r('app.string.pub_empty')))
              .fontSize(18).fontColor(ANDROID_SECONDARY).textAlign(TextAlign.Center).margin(20)
          } else {
            List() {
              ForEach(this.visibleRows(), (row: AddressRow) => {
                ListItem() {
                  Row({ space: 12 }) {
                    Radio({ group: 'profiles', value: row.id }).checked(row.selected).width(20).height(20)
                      .radioStyle({ checkedBackgroundColor: ANDROID_ACCENT, uncheckedBorderColor: ANDROID_SECONDARY })
                      .hitTestBehavior(HitTestMode.None).accessibilityLevel('no')
                    Column({ space: 2 }) {
                      Text(row.name).fontSize(16).fontColor(ANDROID_TEXT)
                      Text(row.endpoint).fontSize(14).fontColor(ANDROID_SECONDARY)
                    }.layoutWeight(1).alignItems(HorizontalAlign.Start)
                    MgrMore() { this.profileMenu(row.index) }
                  }.constraintSize({ minHeight: 64 }).width('100%').padding({ left: 16, right: 4, top: 4, bottom: 4 })
                  .onClick(() => this.select(row.index)).accessibilityDescription(row.selected ? $r('app.string.pub_selected') : $r('app.string.pub_use'))
                  .bindContextMenu(this.profileMenu(row.index), ResponseType.LongPress)
                }
              }, (row: AddressRow) => row.id)
            }.width('100%').height('100%').contentEndOffset(82).divider({ strokeWidth: 1, color: ANDROID_DIVIDER })
          }
          if (this.ready) {
            Button({ type: ButtonType.Circle }) {
              SymbolGlyph($r('sys.symbol.plus')).fontSize(24).fontColor([ANDROID_TEXT])
            }.width(56).height(56).backgroundColor(ANDROID_ACCENT).align(Alignment.BottomEnd)
            .margin({ right: 10, bottom: 18 }).accessibilityText($r('app.string.pub_add'))
            .onClick(() => { this.clearEditor(); this.editorOpen = true; })
          }
        }.layoutWeight(1).width('100%')
        Text(this.currentLabel).width('100%').fontSize(12).fontColor(ANDROID_SECONDARY)
          .padding({ left: 16, right: 16, top: 4, bottom: 4 })
      }.width('100%').height('100%').backgroundColor(ANDROID_BG)
      .bindSheet($$this.editorOpen, this.editor(), { preferType: SheetType.CENTER, height: SheetSize.FIT_CONTENT,
        showClose: false, dragBar: false, backgroundColor: ANDROID_BG, onDisappear: () => this.clearEditor() })
    }
  }

  @Builder profileMenu(index: number) {
    Menu() {
      MenuItem({ content: $r('app.string.mgr_ui_rename') }).onClick(() => this.edit(index, true))
      MenuItem({ content: $r('app.string.pub_delete') }).onClick(() => this.confirmDelete(index))
      MenuItem({ content: $r('app.string.pub_edit'), labelInfo: 'HarmonyOS' }).onClick(() => this.edit(index))
    }
  }

  @Builder editor() {
    Column({ space: 8 }) {
      Text(this.renameOnly ? $r('app.string.mgr_ui_rename') : this.editing === -1 ? $r('app.string.pub_new_profile') : $r('app.string.pub_edit_profile'))
        .fontSize(20).fontWeight(FontWeight.Medium).fontColor(ANDROID_TEXT)
      TextInput({ text: this.name, placeholder: $r('app.string.pub_name') }).onChange((v: string) => { this.name = v; })
      if (!this.renameOnly) {
        TextInput({ text: this.address, placeholder: $r('app.string.pub_address') })
          .type(InputType.Password).showPasswordIcon(false).enableAutoFill(false).copyOption(CopyOptions.None)
          .onChange((v: string) => { this.address = v; })
        TextInput({ text: this.keyInput, placeholder: $r('app.string.pub_key') })
          .type(InputType.Password).showPasswordIcon(false).enableAutoFill(false).copyOption(CopyOptions.None)
          .onChange((v: string) => { this.keyInput = v; })
        Text($r('app.string.pub_privacy')).fontSize(12).fontColor(ANDROID_SECONDARY)
      }
      Row() {
        MgrAction({ label: $r('app.string.pub_cancel'), action: () => this.clearEditor() })
        MgrAction({ label: this.editing === -1 ? $r('app.string.pub_add') : $r('app.string.pub_save_changes'), action: () => this.save() })
      }.width('100%').justifyContent(FlexAlign.End)
    }.width('100%').alignItems(HorizontalAlign.Start).padding(16).backgroundColor(ANDROID_BG)
  }
}
