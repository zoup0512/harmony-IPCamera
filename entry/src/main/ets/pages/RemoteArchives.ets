import { common } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { fileIo, fileUri, picker } from '@kit.CoreFileKit';
import { archiveOrigin, RemoteArchive, RemoteArchiveClient, REMOTE_FILE_BYTES } from '../util/RemoteArchiveClient';

import { ANDROID_ACCENT, ANDROID_BG, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT } from '../components/AndroidTheme';
import { MgrAction, MgrIcon, MgrMore, MgrToolbar } from '../components/MgrToolbar';

interface RemoteArchivesParams { url?: string }

function discard(path: string): void {
  if (path) { try { fileIo.unlinkSync(path); } catch (_) {} }
}

// NetworkKit stream callbacks cannot await file I/O. Write this chunk before
// accepting another rather than building an unbounded queue of pending writes.
function writeChunk(fd: number, data: ArrayBuffer): void {
  let offset = 0;
  while (offset < data.byteLength) {
    const buffer = offset === 0 ? data : data.slice(offset);
    const n = fileIo.writeSync(fd, buffer);
    if (n <= 0 || n > buffer.byteLength) throw new Error('缓存写入失败');
    offset += n;
  }
}

@Entry
@Component
struct RemoteArchives {
  @State origin: string = '';
  @State user: string = '';
  @State password: string = '';
  @State status: ResourceStr = $r('app.string.addon_remote_initial');
  @State rows: RemoteArchive[] = [];
  @State busy: boolean = false;
  @State video: string = '';
  @State playing: string = '';
  @State pickerOpen: boolean = false;
  @State category: number = -1;
  private cached: string = '';
  private generation: number = 0;
  private client?: RemoteArchiveClient;

  aboutToAppear(): void {
    const params = this.getUIContext().getRouter().getParams() as RemoteArchivesParams;
    if (params && typeof params.url === 'string') {
      try { this.origin = archiveOrigin(params.url); }
      catch (_) { this.status = $r('app.string.addon_remote_origin_unknown'); }
    }
  }

  stopVideo(): void {
    this.video = '';
    this.playing = '';
    discard(this.cached);
    this.cached = '';
  }

  reset(): void {
    ++this.generation;
    this.pickerOpen = false;
    this.category = -1;
    if (this.client) this.client.close();
    this.client = undefined;
    this.rows = [];
    this.busy = false;
    this.stopVideo();
  }

  changeOrigin(value: string): void {
    if (value === this.origin) return;
    this.reset();
    this.origin = value;
    this.user = '';
    this.password = '';
    this.status = $r('app.string.addon_remote_changed');
  }

  aboutToDisappear(): void { this.reset(); this.password = ''; this.user = ''; }
  onPageHide(): void { this.reset(); this.status = $r('app.string.addon_remote_reconnect'); }

  private listFailure(reason: string): Resource {
    switch (reason) {
      case '请输入不含账号、路径或查询串的 HTTP(S) 服务器地址': return $r('app.string.addon_remote_bad_origin');
      case '服务器地址无效': return $r('app.string.addon_remote_invalid_origin');
      case '账号或密码格式无效': return $r('app.string.addon_remote_bad_auth');
      case '服务器拒绝访问，请检查账号和密码': return $r('app.string.addon_remote_denied');
      case '录像条目过多': return $r('app.string.addon_remote_limit');
      case '响应超过允许的长度': return $r('app.string.addon_remote_length');
      case '响应长度超限或文件已变化': return $r('app.string.addon_remote_changed_response');
      case '传输超时': return $r('app.string.addon_remote_timeout');
      case '连接失败或重定向已拒绝': return $r('app.string.addon_remote_connection');
      case '无法缓存响应，请检查可用空间': return $r('app.string.addon_remote_cache');
    }
    const http = /^服务器响应 ([1-5][0-9]{2})$/.exec(reason);
    return http ? $r('app.string.addon_remote_http', http[1]) :
      $r('app.string.addon_remote_list_failed');
  }

  async refresh(): Promise<void> {
    this.reset();
    const gen = this.generation;
    this.busy = true;
    this.status = $r('app.string.addon_remote_loading');
    try {
      this.origin = archiveOrigin(this.origin);
      const session = new RemoteArchiveClient(this.origin, this.user, this.password);
      this.client = session;
      const rows = await session.list();
      if (gen !== this.generation) return;
      this.rows = rows;
      this.pickerOpen = rows.length > 0;
      this.status = rows.length ? $r('app.string.addon_remote_count', rows.length) : $r('app.string.addon_remote_empty');
    } catch (e) {
      if (gen === this.generation) this.status = this.listFailure((e as Error).message);
    } finally { if (gen === this.generation) this.busy = false; }
  }

  async select(item: RemoteArchive, save: boolean): Promise<void> {
    const session = this.client;
    if (!session || this.busy) return;
    const gen = this.generation;
    const ctx = getContext(this) as common.UIAbilityContext;
    this.busy = true;
    this.stopVideo();
    this.status = $r('app.string.addon_remote_receiving', item.name);
    // Never use a remote filename in a local path. Only this private temporary
    // file can receive unverified bytes (including 401/redirect response bodies).
    const mkv = item.name.toLowerCase().endsWith('.mkv');
    let temp = '';
    try {
      temp = `${ctx.cacheDir}/remote-archive-${util.generateRandomUUID()}.${mkv ? 'mkv' : 'mp4'}`;
      const file = fileIo.openSync(temp, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      try {
        const b = new Uint8Array(12);
        let n = 0;
        await session.file(item.key, (data: ArrayBuffer) => {
          const take = Math.min(b.length - n, data.byteLength);
          if (take > 0) { b.set(new Uint8Array(data, 0, take), n); n += take; }
          writeChunk(file.fd, data);
        });
        // Reject error pages/playlists masquerading as a successful archive response.
        const valid = mkv ? n >= 4 && b[0] === 0x1a && b[1] === 0x45 && b[2] === 0xdf && b[3] === 0xa3 :
          n === 12 && b[4] === 0x66 && b[5] === 0x74 && b[6] === 0x79 && b[7] === 0x70;
        if (!valid) throw new Error('响应不是支持的 MP4/MKV 录像');
      } finally { fileIo.closeSync(file); }
      if (gen !== this.generation) return;
      if (!save) {
        this.video = fileUri.getUriFromPath(temp);
        this.cached = temp;
        temp = '';
        this.playing = item.name;
        this.status = $r('app.string.addon_remote_cached');
        return;
      }
      this.status = $r('app.string.addon_remote_choose');
      const options = new picker.DocumentSaveOptions();
      options.newFileNames = [item.name];
      const uris = await new picker.DocumentViewPicker(ctx).save(options);
      if (gen !== this.generation) return;
      if (!uris.length) { this.status = $r('app.string.addon_remote_export_cancelled'); return; }
      if (uris.length !== 1 || !uris[0]) throw new Error('未选择有效的目标文件');
      const target = fileIo.openSync(uris[0], fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
      try { await fileIo.copyFile(temp, target.fd); await fileIo.fsync(target.fd); }
      finally { fileIo.closeSync(target); }
      if (gen === this.generation) this.status = $r('app.string.addon_remote_exported');
    } catch (e) {
      // Platform I/O and picker errors may include paths. Do not expose URLs or secrets.
      if (gen === this.generation) this.status = $r('app.string.addon_remote_failed');
    } finally {
      discard(temp);
      if (gen === this.generation) this.busy = false;
    }
  }

  private confirmServer(): void {
    if (this.busy) return;
    let target: string;
    try { target = archiveOrigin(this.origin); }
    catch (e) { this.status = this.listFailure((e as Error).message); return; }
    const gen = this.generation;
    this.getUIContext().showAlertDialog({ title: $r('app.string.addon_remote_load'),
      message: $r('app.string.mgr_ui_confirm_origin', target),
      primaryButton: { value: $r('app.string.addon_remote_cancel'), action: () => {} },
      secondaryButton: { value: $r('app.string.addon_remote_load'), action: () => {
        if (gen === this.generation && !this.busy) this.refresh();
      } }
    });
  }

  private groupRows(group: number): RemoteArchive[] {
    return this.rows.filter((row: RemoteArchive) => row.motion === (group === 1));
  }

  private take(item: RemoteArchive, save: boolean): void {
    if (this.busy || !this.pickerOpen || !this.rows.includes(item)) return;
    this.pickerOpen = false;
    this.select(item, save);
  }

  build() {
    WithTheme({ colorMode: ThemeColorMode.DARK }) {
      Column() {
        MgrToolbar({ title: $r('app.string.addon_remote_title') }) {
          if (this.rows.length && !this.busy) {
            MgrIcon({ icon: $r('sys.symbol.list_bullet'), label: $r('app.string.mgr_ui_choose_recording'),
              action: () => { this.category = -1; this.pickerOpen = true; } })
          }
        }
        Scroll() {
          Column({ space: 8 }) {
            Text($r('app.string.mgr_ui_server_ext')).fontSize(14).fontColor(ANDROID_ACCENT)
            TextInput({ text: this.origin, placeholder: $r('app.string.addon_remote_address') })
              .type(/[@?#]/.test(this.origin) ? InputType.Password : InputType.Normal)
              .showPasswordIcon(false).enableAutoFill(false).copyOption(CopyOptions.None)
              .onChange((value: string) => this.changeOrigin(value))
            Text($r('app.string.addon_remote_trust')).fontSize(12).fontColor(ANDROID_SECONDARY)
            Row({ space: 6 }) {
              TextInput({ text: this.user, placeholder: $r('app.string.addon_remote_user') }).layoutWeight(1)
                .type(InputType.Password).showPasswordIcon(false).enableAutoFill(false).copyOption(CopyOptions.None)
                .onChange((v: string) => { this.reset(); this.user = v; this.status = $r('app.string.addon_remote_reload'); })
              TextInput({ text: this.password, placeholder: $r('app.string.addon_remote_password') }).layoutWeight(1)
                .type(InputType.Password).showPasswordIcon(false).enableAutoFill(false).copyOption(CopyOptions.None)
                .onChange((v: string) => { this.reset(); this.password = v; this.status = $r('app.string.addon_remote_reload'); })
            }
            Row() {
              MgrAction({ label: $r('app.string.addon_remote_load'), enabledAction: !this.busy, action: () => this.confirmServer() })
              if (this.busy) {
                MgrAction({ label: $r('app.string.addon_remote_cancel'), action: () => { this.reset(); this.status = $r('app.string.addon_remote_cancelled'); } })
              }
            }
            Divider().color(ANDROID_DIVIDER)
            Text(this.status).fontSize(14).fontColor(ANDROID_SECONDARY)
            if (this.rows.length && !this.busy) {
              MgrAction({ label: $r('app.string.mgr_ui_choose_recording'), action: () => { this.category = -1; this.pickerOpen = true; } })
            }
            if (this.video) {
              Video({ src: this.video }).width('100%').height(180).controls(true).autoPlay(true)
                .onError(() => { this.status = $r('app.string.addon_remote_play_failed'); })
              Text(this.playing).fontColor(ANDROID_TEXT).fontSize(14)
              MgrAction({ label: $r('app.string.addon_remote_close'), action: () => this.stopVideo() })
            }
          }.width('100%').alignItems(HorizontalAlign.Start).padding(16)
        }.width('100%').layoutWeight(1).align(Alignment.TopStart)
      }.width('100%').height('100%').backgroundColor(ANDROID_BG)
      .bindSheet($$this.pickerOpen, this.archivePicker(), { preferType: SheetType.CENTER, height: '60%',
        showClose: false, dragBar: false, backgroundColor: ANDROID_BG })
    }
  }

  @Builder archivePicker() {
    Column() {
      Text(this.category === -1 ? $r('app.string.mgr_ui_choose_recording') :
        this.category === 0 ? $r('app.string.addon_remote_manual') : $r('app.string.addon_remote_motion'))
        .fontSize(20).fontWeight(FontWeight.Medium).fontColor(ANDROID_TEXT).width('100%').padding(16)
      List() {
        if (this.category === -1) {
          ForEach([0, 1], (group: number) => {
            ListItem() {
              Row({ space: 8 }) {
                Text(group === 0 ? $r('app.string.addon_remote_manual') : $r('app.string.addon_remote_motion'))
                  .fontSize(16).fontColor(ANDROID_TEXT)
                Text(`(${this.groupRows(group).length})`).fontSize(16).fontColor(ANDROID_SECONDARY)
              }.width('100%').height(52).padding({ left: 16, right: 16 }).enabled(this.groupRows(group).length > 0)
                .onClick(() => { this.category = group; })
            }
          })
        } else {
          ForEach(this.groupRows(this.category), (item: RemoteArchive) => {
            ListItem() {
              Row({ space: 8 }) {
                Column({ space: 3 }) {
                  Text(item.name).fontColor(ANDROID_TEXT).fontSize(16)
                  if (item.size >= 0) { Text(`${(item.size / 1048576).toFixed(1)} MiB`).fontColor(ANDROID_SECONDARY).fontSize(12) }
                }.alignItems(HorizontalAlign.Start).layoutWeight(1)
                MgrMore() { this.fileMenu(item) }
              }.width('100%').constraintSize({ minHeight: 52 }).padding({ left: 16, right: 4 })
              .enabled(!this.busy && item.size !== 0 && item.size <= REMOTE_FILE_BYTES)
              .onClick(() => this.take(item, false)).accessibilityDescription($r('app.string.addon_remote_play'))
            }
          }, (item: RemoteArchive) => item.key)
        }
      }.width('100%').layoutWeight(1).divider({ strokeWidth: 1, color: ANDROID_DIVIDER })
      Row() {
        if (this.category !== -1) {
          MgrAction({ label: $r('app.string.mgr_ui_back'), action: () => { this.category = -1; } })
        }
        MgrAction({ label: $r('app.string.addon_remote_cancel'), action: () => { this.pickerOpen = false; } })
      }.width('100%').justifyContent(FlexAlign.End).padding(4)
    }.width('100%').height('100%').backgroundColor(ANDROID_BG)
  }

  @Builder fileMenu(item: RemoteArchive) {
    Menu() {
      MenuItem({ content: $r('app.string.addon_remote_play') }).onClick(() => this.take(item, false))
      MenuItem({ content: $r('app.string.addon_remote_export'), labelInfo: 'HarmonyOS' }).onClick(() => this.take(item, true))
    }
  }
}
