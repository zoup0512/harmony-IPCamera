import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import { media } from '@kit.MediaKit';
import { promptAction } from '@kit.ArkUI';

const TAG: string = 'PlaybackPage';

interface ArchiveItem {
  name: string;
  path: string;
  size: number;
}

function fmtSize(bytes: number): string {
  if (bytes > 1024 * 1024 * 1024) {
    return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
  }
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

@Entry
@Component
struct Playback {
  @State archives: ArchiveItem[] = [];
  @State playingName: string = '';
  @State playerState: string = 'idle';
  private avPlayer?: media.AVPlayer;
  private fileFd: number = -1;
  private surfaceId: string = '';
  private xcomponentController: XComponentController = new XComponentController();

  aboutToAppear(): void {
    this.refreshList();
  }

  aboutToDisappear(): void {
    this.releasePlayer();
  }

  refreshList(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    const out: ArchiveItem[] = [];
    try {
      const names = fileIo.listFileSync(ctx.filesDir);
      for (const name of names) {
        if (name.startsWith('ipc_') && name.endsWith('.mp4')) {
          const full = `${ctx.filesDir}/${name}`;
          const st = fileIo.statSync(full);
          out.push({ name: name, path: full, size: st.size });
        }
      }
    } catch (err) {
      hilog.warn(0xC010, TAG, 'list files failed');
    }
    out.sort((a: ArchiveItem, b: ArchiveItem) => a.name < b.name ? 1 : -1);
    this.archives = out;
  }

  releasePlayer(): void {
    if (this.avPlayer) {
      this.avPlayer.release();
      this.avPlayer = undefined;
    }
    if (this.fileFd >= 0) {
      try {
        fileIo.closeSync(this.fileFd);
      } catch (err) {
        hilog.warn(0xC010, TAG, 'close fd failed');
      }
      this.fileFd = -1;
    }
    this.playerState = 'idle';
  }

  async play(item: ArchiveItem): Promise<void> {
    this.releasePlayer();
    try {
      const file = fileIo.openSync(item.path, fileIo.OpenMode.READ_ONLY);
      this.fileFd = file.fd;
      const player = await media.createAVPlayer();
      this.avPlayer = player;
      player.on('stateChange', (state: string) => {
        this.playerState = state;
        if (state === 'initialized') {
          if (this.surfaceId !== '') {
            player.surfaceId = this.surfaceId;
          }
          player.prepare();
        } else if (state === 'prepared') {
          player.play();
        } else if (state === 'completed') {
          player.seek(0);
          player.play();
        }
      });
      player.on('error', () => {
        promptAction.showToast({ message: '播放失败' });
        this.releasePlayer();
      });
      player.url = `fd://${this.fileFd}`;
      this.playingName = item.name;
    } catch (err) {
      promptAction.showToast({ message: '打开录像失败' });
      this.releasePlayer();
    }
  }

  toggle(): void {
    if (this.avPlayer === undefined) return;
    if (this.playerState === 'playing') {
      this.avPlayer.pause();
    } else if (this.playerState === 'paused' || this.playerState === 'prepared' ||
               this.playerState === 'completed') {
      this.avPlayer.play();
    }
  }

  build() {
    Scroll() {
      Column({ space: 10 }) {
        Text('录像回放')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .margin({ top: 14 })

        XComponent({ id: 'playback', type: 'surface', libraryname: 'streaming',
          controller: this.xcomponentController })
          .onLoad(() => {
            this.surfaceId = this.xcomponentController.getXComponentSurfaceId();
          })
          .width('92%')
          .height(220)
          .borderRadius(8)
          .backgroundColor('#000000')

        if (this.playingName !== '') {
          Row({ space: 10 }) {
            Button(this.playerState === 'playing' ? '暂停' : '播放')
              .onClick(() => this.toggle())
            Button('停止').onClick(() => {
              this.releasePlayer();
              this.playingName = '';
            })
            Text(this.playingName).fontSize(11).fontColor('#757575').layoutWeight(1)
            Text(this.playerState).fontSize(11).fontColor('#9E9E9E')
          }
          .width('92%')
        }

        Row({ space: 8 }) {
          Button('刷新列表').onClick(() => this.refreshList())
          Text(`${this.archives.length} 个录像`).fontSize(12).fontColor('#757575')
        }

        ForEach(this.archives, (item: ArchiveItem) => {
          Row({ space: 10 }) {
            Column({ space: 2 }) {
              Text(item.name).fontSize(14)
              Text(fmtSize(item.size)).fontSize(11).fontColor('#757575')
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)
            Button(this.playingName === item.name && this.playerState === 'playing'
              ? '播放中' : '播放')
              .fontSize(12)
              .height(32)
              .enabled(this.playingName !== item.name)
              .onClick(() => this.play(item))
          }
          .padding(10)
          .borderRadius(8)
          .backgroundColor('#F5F5F5')
          .width('92%')
        }, (item: ArchiveItem) => item.name)
      }
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
