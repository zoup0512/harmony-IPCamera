import { common } from '@kit.AbilityKit';
import { fileIo, fileUri, picker } from '@kit.CoreFileKit';

// Per-instance opt-in, never restored from preferences. UI must explain that
// copies in Download/<bundle> are user-visible, unlike the private originals.
// Use authorize() only on an explicit user gesture; never prompt from a writer.
export class RecordingDownloads {
  private directory: string = '';
  private generation: number = 0;

  disable(): void {
    this.generation++;
    this.directory = '';
  }

  async authorize(context: common.UIAbilityContext): Promise<boolean> {
    this.disable();
    const ticket = this.generation;
    const options = new picker.DocumentSaveOptions();
    options.pickerMode = picker.DocumentPickerMode.DOWNLOAD;
    const uris = await new picker.DocumentViewPicker(context).save(options);
    if (ticket !== this.generation || uris.length === 0) return false;
    // A DOWNLOAD directory is a local Documents-provider URI, not a network
    // URL, a named file from the standard Save picker, or an arbitrary tree.
    if (uris.length !== 1 || !/^file:\/\/docs\/[A-Za-z0-9_./-]+$/.test(uris[0]) ||
        /\/(?:\.{1,2})(?:\/|$)/.test(uris[0])) {
      throw new Error('Unexpected Download URI');
    }
    const uri = new fileUri.FileUri(uris[0]);
    const path = uri.path.replace(/\/$/, '');
    if (uri.isRemoteUri() || !path.startsWith('/') || path.split('/').includes('..') ||
        !path.endsWith(`/Download/${context.applicationInfo.name}`)) throw new Error('Unexpected Download directory');
    this.directory = path;
    return true;
  }

  // Call ONLY from the native finalized-file event; file size or a directory
  // scan cannot tell a failed/unfinalized MP4 from a completed recording.
  // Returns false if disabled/cancelled; storage/permission/collision errors
  // reject. Queue calls in the consumer if bounded disk concurrency is needed.
  async copyCompleted(context: common.UIAbilityContext, path: string): Promise<boolean> {
    const ticket = this.generation;
    const directory = this.directory;
    if (!directory) return false;
    const name = path.substring(path.lastIndexOf('/') + 1);
    if (!(/^(ipc_|IPS_)[A-Za-z0-9._-]+\.mp4$/.test(name)) ||
        path !== `${context.filesDir}/${name.startsWith('IPS_') ? 'MD/' : ''}${name}`) {
      throw new Error('Unexpected recording path');
    }
    // Stage in a unique PRIVATE directory: never truncate or recursively clean
    // anything in public storage. Move only this copy, with no-overwrite mode.
    const stage = await fileIo.mkdtemp(`${context.cacheDir}/record-copy-`);
    const tmp = `${stage}/copy.part`;
    try {
      const source = await fileIo.open(path, fileIo.OpenMode.READ_ONLY | fileIo.OpenMode.NOFOLLOW);
      try {
        const before = await fileIo.stat(source.fd);
        if (!before.isFile() || before.size <= 0) throw new Error('Empty or invalid recording');
        const output = await fileIo.open(tmp, fileIo.OpenMode.CREATE | fileIo.OpenMode.READ_WRITE);
        try {
          await fileIo.copyFile(source.fd, output.fd);
          await fileIo.fsync(output.fd);
          const after = await fileIo.stat(source.fd);
          if (after.size !== before.size || after.mtime !== before.mtime || after.mtimeNs !== before.mtimeNs ||
              (await fileIo.stat(output.fd)).size !== before.size) throw new Error('Incomplete recording copy');
        } finally {
          await fileIo.close(output);
        }
      } finally {
        await fileIo.close(source);
      }
      if (ticket !== this.generation) return false;
      // Do not fall back to overwriting if the provider rejects a cross-volume
      // move. The usual single-file Save picker remains available to the UI.
      await fileIo.moveFile(tmp, `${directory}/${name}`, 1);
      return true;
    } finally {
      await fileIo.rmdir(stage);
    }
  }
}
