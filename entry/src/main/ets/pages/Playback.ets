import { common } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import { media } from '@kit.MediaKit';
import { ANDROID_BG, ANDROID_BAR, ANDROID_TEXT, ANDROID_SECONDARY, ANDROID_DIVIDER, ANDROID_ACCENT, ANDROID_TOOLBAR_HEIGHT } from '../components/AndroidTheme';
import streaming from 'libstreaming.so';
import { exportRecording, RecordingExportResult } from '../util/RecordingExport';

function timeLabel(ms: number): string {
  const s = Math.floor(Math.max(0, ms) / 1000);
  return `${Math.floor(s / 60)}:${(s % 60).toString().padStart(2, '0')}`;
}

interface ArchiveItem {
  name: string;
  path: string;
  size: number;
  modified: number;
}

function collect(dir: string, prefix: string, group: string, rows: ArchiveItem[]): void {
  let files: string[];
  try { files = fileIo.listFileSync(dir); } catch (_) { return; } // MD may not exist yet
  for (const name of files) {
    if (!name.startsWith(prefix) || !name.endsWith('.mp4')) continue;
    const path = `${dir}/${name}`;
    try {
      const st = fileIo.statSync(path);
      if (st.isFile()) rows.push({ name: group + name, path: path, size: st.size, modified: st.mtime });
    } catch (_) {} // rotation or deletion should not hide the remaining files
  }
}

@Entry
@Component
struct Playback {
  @State archives: ArchiveItem[] = [];
  @State playingName: string = '';
  @State playerState: string = 'idle';
  @State message: ResourceStr = '';
  private avPlayer?: media.AVPlayer;
  private file?: fileIo.File;
  private surfaceId: string = '';
  private controller: XComponentController = new XComponentController();
  private epoch: number = 0;
  private work: Promise<void> = Promise.resolve();
  private pending?: ArchiveItem;
  @State duration: number = 0;
  @State elapsedMs: number = 0;
  @State exporting: boolean = false;
  @State listVisible: boolean = true;
  @State controlsVisible: boolean = true;
  private hidden: boolean = false;
  private dragging: boolean = false;
  private disposed: boolean = false;

  async saveArchive(item: ArchiveItem): Promise<void> {
    if (this.exporting) return;
    this.exporting = true;
    this.message = '';
    try {
      if (streaming.recordingFileBusy(item.path)) {
        this.message = $r('app.string.arc_busy');
        return;
      }
      const result = await exportRecording(getContext(this) as common.UIAbilityContext, item.path);
      if (this.disposed) return;
      if (result === RecordingExportResult.Saved) this.message = $r('app.string.arc_saved');
      else if (result === RecordingExportResult.EmptyRecording) this.message = $r('app.string.arc_empty');
      else if (result === RecordingExportResult.Failed) this.message = $r('app.string.arc_export_failed');
    } catch (_) { if (!this.disposed) this.message = $r('app.string.arc_export_error'); }
    finally { if (!this.disposed) this.exporting = false; }
  }

  seek(value: number, mode: SliderChangeMode): void {
    this.dragging = mode === SliderChangeMode.Begin || mode === SliderChangeMode.Moving;
    this.elapsedMs = value;
    if (this.dragging || !this.avPlayer || this.duration <= 0) return;
    try { this.avPlayer.seek(Math.min(this.duration - 1, Math.max(0, Math.round(value))), media.SeekMode.SEEK_CLOSEST); }
    catch (_) { this.message = $r('app.string.arc_seek_failed'); }
  }

  onPageShow(): void { this.hidden = false; this.refreshList(); }
  onPageHide(): void { this.hidden = true; this.stop(); }

  onBackPress(): boolean {
    if (this.listVisible && this.playingName) { this.listVisible = false; return true; }
    if (!this.controlsVisible) { this.controlsVisible = true; return true; }
    this.stop();
    return false;
  }

  toolbarBack(): void {
    if (!this.onBackPress()) this.getUIContext().getRouter().back();
  }

  showList(): void {
    this.controlsVisible = true;
    this.listVisible = true;
    this.refreshList();
  }

  tapVideo(): void {
    if (this.playingName && !this.listVisible) this.controlsVisible = !this.controlsVisible;
  }

  chooseFile(item: ArchiveItem): void {
    if (item.name === this.playingName) this.listVisible = false;
    else this.play(item);
  }

  openRemote(): void {
    this.stop();
    // This is a different browser. Never route a local path or reuse camera credentials.
    this.getUIContext().getRouter().pushUrl({ url: 'pages/RemoteArchives' })
      .catch(() => { if (!this.disposed && !this.hidden) this.message = $r('app.string.viewer_archives_error'); });
  }

  aboutToDisappear(): void { this.disposed = true; this.stop(); }

  refreshList(): void {
    const dir = (getContext(this) as common.UIAbilityContext).filesDir;
    const out: ArchiveItem[] = [];
    collect(dir, 'ipc_', '', out);
    collect(dir + '/MD', 'IPS_', 'MD/', out);
    out.sort((a: ArchiveItem, b: ArchiveItem) => b.modified - a.modified || b.name.localeCompare(a.name));
    this.archives = out;
  }

  // Only the serialized work queue can acquire/release an AVPlayer or close its FD.
  // A new generation makes stale callbacks no-ops even while an async release is in flight.
  private async release(): Promise<void> {
    const player = this.avPlayer, file = this.file;
    this.avPlayer = undefined;
    this.file = undefined;
    try { if (player) await player.release(); } catch (_) {}
    finally { if (file) { try { fileIo.closeSync(file); } catch (_) {} } }
  }

  stop(): void {
    ++this.epoch;
    this.pending = undefined;
    this.listVisible = true;
    this.controlsVisible = true;
    this.playingName = '';
    this.playerState = 'idle';
    this.duration = 0;
    this.elapsedMs = 0;
    this.dragging = false;
    this.work = this.work.then(() => this.release());
  }

  fail(gen: number): void {
    if (gen !== this.epoch) return;
    this.stop();
    this.message = $r('app.string.arc_play_failed');
  }

  play(item: ArchiveItem): void {
    if (this.hidden || this.disposed) return;
    this.stop();
    this.listVisible = false;
    this.message = '';
    this.playingName = item.name;
    this.playerState = 'loading';
    if (!this.surfaceId) { this.pending = item; return; }
    const gen = this.epoch;
    this.work = this.work.then(async () => {
      if (gen !== this.epoch) return;
      try {
        this.file = fileIo.openSync(item.path, fileIo.OpenMode.READ_ONLY);
        const player = await media.createAVPlayer();
        this.avPlayer = player;
        if (gen !== this.epoch) return; // stop() already queued release
        player.on('stateChange', (state: string) => {
          if (gen !== this.epoch) return;
          this.playerState = state;
          try {
            if (state === 'initialized') {
              player.surfaceId = this.surfaceId;
              player.prepare().catch(() => this.fail(gen));
            } else if (state === 'prepared') {
              this.duration = Math.max(0, player.duration);
              player.loop = true;
              player.play().catch(() => this.fail(gen));
            }
          } catch (_) { this.fail(gen); }
        });
        player.on('timeUpdate', (ms: number) => { if (gen === this.epoch && !this.dragging) this.elapsedMs = Math.max(0, ms); });
        player.on('durationUpdate', (ms: number) => { if (gen === this.epoch) this.duration = Math.max(0, ms); });
        player.on('error', () => this.fail(gen));
        player.fdSrc = { fd: this.file.fd };
      } catch (_) { this.fail(gen); }
    });
  }

  toggle(): void {
    const player = this.avPlayer, gen = this.epoch;
    if (!player) return;
    try {
      if (this.playerState === 'playing') player.pause().catch(() => this.fail(gen));
      else if (this.playerState === 'paused') player.play().catch(() => this.fail(gen));
    } catch (_) { this.fail(gen); }
  }

  build() {
    Stack() {
      XComponent({ id: 'archivePlayback', type: 'surface', controller: this.controller })
        .renderFit(RenderFit.RESIZE_CONTAIN)
        .onLoad(() => {
          this.surfaceId = this.controller.getXComponentSurfaceId();
          if (this.pending) this.play(this.pending);
        })
        .onDestroy(() => { this.surfaceId = ''; this.stop(); })
        .width('100%').height('100%').backgroundColor('#000000')
      Column() {
        if (this.controlsVisible) {
          Row() {
            Button('‹').width(48).height(48).fontSize(30).fontColor(ANDROID_TEXT).backgroundColor(Color.Transparent)
              .accessibilityText(getContext(this).resourceManager.getStringSync($r('app.string.view_ui_back').id)).onClick(() => this.toolbarBack())
            Column() {
              Text(this.listVisible ? $r('app.string.view_ui_local') : $r('app.string.arc_title')).fontSize(18).fontColor(ANDROID_TEXT)
              if (this.playingName) {
                Text(this.playingName).fontSize(13).fontColor(ANDROID_SECONDARY).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
              }
            }.alignItems(HorizontalAlign.Start).layoutWeight(1)
            if (!this.listVisible) { this.action($r('app.string.view_ui_list'), () => this.showList()) }
            else { this.action($r('app.string.viewer_archives'), () => this.openRemote()) }
          }.width('100%').height(ANDROID_TOOLBAR_HEIGHT).backgroundColor(ANDROID_BAR)
        }
        if (this.listVisible) { this.library() }
        else {
          Column() {
            if (this.playerState === 'loading' || this.playerState === 'initialized') {
              LoadingProgress().width(36).height(36).color(ANDROID_ACCENT)
            }
          }.width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
            .onClick(() => this.tapVideo())
          if (this.controlsVisible) { this.transport() }
        }
      }.width('100%').height('100%')
    }.width('100%').height('100%').backgroundColor('#000000')
  }

  @Builder
  action(label: ResourceStr, click: () => void) {
    Button(label).height(44).fontSize(14).backgroundColor(Color.Transparent).fontColor(ANDROID_TEXT).onClick(click)
  }

  @Builder
  transport() {
    Column() {
      if (this.message) { Text(this.message).fontSize(13).fontColor(ANDROID_TEXT) }
      if (this.duration > 0) {
        Row({ space: 6 }) {
          Text(timeLabel(this.elapsedMs)).fontSize(12).fontColor(ANDROID_TEXT)
          Slider({ value: this.elapsedMs, min: 0, max: this.duration, step: 1 }).layoutWeight(1).selectedColor(ANDROID_ACCENT)
            .enabled(this.playerState === 'playing' || this.playerState === 'paused')
            .onChange((v: number, mode: SliderChangeMode) => this.seek(v, mode))
          Text(timeLabel(this.duration)).fontSize(12).fontColor(ANDROID_TEXT)
        }.width('100%')
      }
      Row({ space: 12 }) {
        Button(this.playerState === 'playing' ? $r('app.string.arc_pause') : $r('app.string.arc_play'))
          .fontColor(ANDROID_BAR).backgroundColor(ANDROID_ACCENT).enabled(this.playerState === 'playing' || this.playerState === 'paused')
          .onClick(() => this.toggle())
        this.action($r('app.string.arc_stop'), () => this.stop())
      }
    }.width('100%').padding(12).backgroundColor('#D9212121')
  }

  @Builder
  library() {
    Column() {
      Row() {
        Text($r('app.string.arc_count', this.archives.length)).fontColor(ANDROID_SECONDARY).layoutWeight(1)
        this.action($r('app.string.arc_refresh'), () => this.refreshList())
      }.padding({ left: 16, right: 8 }).width('100%')
      if (this.message) { Text(this.message).fontSize(13).fontColor(ANDROID_TEXT).padding(8) }
      if (!this.archives.length) {
        Text($r('app.string.view_ui_empty_local')).fontColor(ANDROID_SECONDARY).padding(20)
      }
      List() {
        ForEach(this.archives, (item: ArchiveItem) => {
          ListItem() {
            Row({ space: 6 }) {
              Column({ space: 4 }) {
                Text(item.name).fontSize(15).fontColor(this.playingName === item.name ? ANDROID_ACCENT : ANDROID_TEXT)
                  .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
                Text(`${(item.size / 1048576).toFixed(1)} MB`).fontSize(12).fontColor(ANDROID_SECONDARY)
              }.layoutWeight(1).alignItems(HorizontalAlign.Start)
              Button(this.playingName === item.name ? (this.playerState === 'playing' ? $r('app.string.arc_playing') :
                $r('app.string.view_ui_current')) : $r('app.string.arc_play')).fontSize(12).height(40)
                .fontColor(ANDROID_ACCENT).backgroundColor(Color.Transparent).enabled(item.size > 0)
                .onClick(() => this.chooseFile(item))
              Button($r('app.string.arc_export')).fontSize(12).height(40).fontColor(ANDROID_TEXT).backgroundColor(Color.Transparent)
                .enabled(!this.exporting && item.size > 0).onClick(() => this.saveArchive(item))
            }.width('100%').padding(10)
          }
        }, (item: ArchiveItem) => item.name + ':' + item.size)
      }.width('100%').layoutWeight(1).divider({ strokeWidth: 1, color: ANDROID_DIVIDER })
    }.width('100%').layoutWeight(1).backgroundColor(ANDROID_BG)
  }
}
