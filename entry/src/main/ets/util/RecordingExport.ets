import { common } from '@kit.AbilityKit';
import { fileIo, picker } from '@kit.CoreFileKit';

export enum RecordingExportResult {
  Saved = 'saved',
  Cancelled = 'cancelled',
  EmptyRecording = 'emptyRecording',
  Failed = 'failed'
}

const CHUNK_BYTES: number = 256 * 1024;

async function copyRecording(source: number, destination: number, size: number): Promise<void> {
  const buffer = new ArrayBuffer(CHUNK_BYTES);
  let remaining = size;
  while (remaining > 0) {
    const length = Math.min(remaining, CHUNK_BYTES);
    const count = await fileIo.read(source, buffer, { length: length });
    if (count <= 0 || count > length) {
      throw new Error('Incomplete recording read');
    }
    let written = 0;
    while (written < count) {
      // FileIO's offset is a file position, not an offset into the buffer.
      const data = written === 0 ? buffer : buffer.slice(written, count);
      const n = await fileIo.write(destination, data, { length: count - written });
      if (n <= 0 || n > count - written) {
        throw new Error('Incomplete recording write');
      }
      written += n;
    }
    remaining -= count;
  }
}

// Pass the selected archive's path, not the current recorder's output path. Only
// completed recordings should be offered by the caller. No directory grant is kept.
export async function exportRecording(context: common.UIAbilityContext, path: string): Promise<RecordingExportResult> {
  const name = path.substring(path.lastIndexOf('/') + 1);
  const manual = name.startsWith('ipc_') && path === `${context.filesDir}/${name}`;
  const motion = name.startsWith('IPS_') && path === `${context.filesDir}/MD/${name}`;
  if (!(manual || motion) || !name.endsWith('.mp4')) {
    return RecordingExportResult.Failed;
  }
  try {
    const source = await fileIo.open(path, fileIo.OpenMode.READ_ONLY);
    try {
      const before = await fileIo.stat(source.fd);
      if (!before.isFile()) {
        return RecordingExportResult.Failed;
      }
      // The recorder creates the file before it receives any samples.
      if (before.size === 0) {
        return RecordingExportResult.EmptyRecording;
      }
      const options = new picker.DocumentSaveOptions();
      options.newFileNames = [name];
      const uris = await new picker.DocumentViewPicker(context).save(options);
      if (uris.length === 0) {
        return RecordingExportResult.Cancelled;
      }
      if (uris.length !== 1 || !uris[0]) {
        return RecordingExportResult.Failed;
      }
      const destination = await fileIo.open(uris[0], fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
      try {
        await copyRecording(source.fd, destination.fd, before.size);
        const after = await fileIo.stat(source.fd);
        if (after.size !== before.size || after.mtime !== before.mtime || after.mtimeNs !== before.mtimeNs) {
          return RecordingExportResult.Failed;
        }
        await fileIo.fsync(destination.fd);
      } finally {
        await fileIo.close(destination);
      }
    } finally {
      await fileIo.close(source);
    }
    return RecordingExportResult.Saved;
  } catch (_) {
    // Picker errors and I/O errors (including flush/close) are not cancellations.
    return RecordingExportResult.Failed;
  }
}
