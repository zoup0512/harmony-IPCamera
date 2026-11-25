import { common } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import streaming from 'libstreaming.so';
import { artifactName, STAMP, checkMail, checkFtp, mail, ftp, safeFailure, Part, fail, MAIL_LIMIT, FTP_LIMIT } from './DeliveryProtocol';
import { loadMail, loadFtp, revision } from './DeliveryStore';
import { PlatformDialer } from './DeliverySocket';

const pendingSnapshots = new Map<string, number>();
const active = new Set<string>();
const recent: string[] = [];
let lastResult = '';
export function lastDeliveryResult(): string { return lastResult; }
function report(s: string): string { lastResult = s; return s; }

// Called once per motion episode. Not a picker or a scan. Only an issued, unique snapshot may be mailed.
export async function deliveryMotionSnapshotPath(ctx: common.UIAbilityContext, stamp: string): Promise<string> {
  const current = revision(true);
  const c = await loadMail();
  if (!c.enabled || current !== revision(true)) return '';
  checkMail(c);
  if (!(new RegExp('^' + STAMP + '$')).test(stamp) || /[\r\n]/.test(stamp)) return fail('抓拍时间戳无效');
  const dir = ctx.filesDir + '/MD';
  if (!fileIo.accessSync(dir)) fileIo.mkdirSync(dir);
  if (!fileIo.lstatSync(dir).isDirectory()) return fail('告警目录不是本地目录');
  const path = dir + '/IPS_' + stamp + '.bmp';
  if (pendingSnapshots.has(path)) return '';
  // The private producer owns this namespace. Reject *any* existing entry, including dangling links.
  try { fileIo.lstatSync(path); return ''; } catch (err) {
    if ((err as BusinessError).code !== 13900002) fail('无法确认抓拍目标为空');
  }
  if (pendingSnapshots.size >= 32) pendingSnapshots.delete(Array.from(pendingSnapshots.keys())[0]);
  pendingSnapshots.set(path, current);
  return path;
}

class FilePart implements Part {
  name: string;
  size: number;
  private file: fileIo.File;
  private mtime: number;
  private changeTime?: bigint;
  private channel: boolean;
  private version: number;
  constructor(name: string, file: fileIo.File, stat: fileIo.Stat, channel: boolean, version: number) {
    this.name = name; this.file = file; this.size = stat.size; this.mtime = stat.mtime;
    this.changeTime = stat.ctimeNs; this.channel = channel; this.version = version;
  }
  async read(n: number): Promise<Uint8Array> {
    if (this.version !== revision(this.channel)) fail('配置已更改，已中止传输');
    const b = new ArrayBuffer(n);
    const count = await fileIo.read(this.file.fd, b);
    const stat = fileIo.statSync(this.file.fd);
    if (stat.size !== this.size || stat.mtime !== this.mtime || stat.ctimeNs !== this.changeTime) fail('文件已更改，已中止传输');
    return new Uint8Array(b, 0, count);
  }
}
function checkFile(file: fileIo.File, isMail: boolean, size: number): void {
  if (size <= 0 || size > (isMail ? MAIL_LIMIT : FTP_LIMIT))
    fail(isMail ? '抓拍为空或超过 12 MiB 上限' : '录像为空或超过 512 MiB 上限');
  const head = new ArrayBuffer(12);
  if (fileIo.readSync(file.fd, head, { offset: 0, length: 12 }) !== 12) fail('媒体文件不完整');
  fileIo.lseek(file.fd, 0, fileIo.WhenceType.SEEK_SET);
  const v = new DataView(head);
  if (isMail ? (v.getUint16(0) !== 0x424d || v.getUint32(2, true) !== size) : v.getUint32(4) !== 0x66747970) fail('本地媒体格式无效');
}
// ONLY call from the trusted native success/close callback (including completed rotated segments).
// Never call on Start/Stop request, during playback, from HTTP, route params or a filesystem listing.
export async function onDeliveryCompleted(ctx: common.UIAbilityContext, path: string): Promise<string> {
  const name = artifactName(ctx.filesDir, path);
  if (!name) return '';
  const isMail = name.endsWith('.bmp');
  const version = revision(isMail);
  if (isMail) {
    const pending = pendingSnapshots.get(path);
    pendingSnapshots.delete(path);
    if (pending !== version) return '';
  }
  if (active.has(path) || recent.includes(path)) return '';
  if (active.size >= 4) return report('传输任务已满，已跳过本次事件；文件仍保留在本地');
  active.add(path);
  let file: fileIo.File | undefined = undefined;
  try {
    const smtp = isMail ? await loadMail() : undefined;
    const upload = isMail ? undefined : await loadFtp();
    if ((!smtp || !smtp.enabled) && (!upload || !upload.enabled)) return '';
    if (version !== revision(isMail)) return '';
    if (smtp) checkMail(smtp);
    if (upload) checkFtp(upload);
    if (name.startsWith('IPS_') && !fileIo.lstatSync(ctx.filesDir + '/MD').isDirectory()) fail('拒绝符号链接目录');
    if (!isMail && streaming.recordingFileBusy(path)) fail('录像仍在写入，未上传');
    file = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY | fileIo.OpenMode.NOFOLLOW | fileIo.OpenMode.NONBLOCK);
    const stat = fileIo.statSync(file.fd);
    if (!stat.isFile() || file.path !== path || fileIo.lstatSync(path).ino !== stat.ino) fail('拒绝非本地普通文件');
    checkFile(file, isMail, stat.size);
    // Suppress duplicate native completions, even after an ambiguous network failure. No retries or deletion.
    if (recent.length >= 256) recent.shift();
    recent.push(path);
    const part = new FilePart(name, file, stat, isMail, version);
    const dialer = new PlatformDialer(() => version === revision(isMail));
    if (smtp) await mail(dialer, smtp, part);
    if (upload) await ftp(dialer, upload, part);
    return report(isMail ? '移动告警邮件已由服务器接受' : '录像已上传；本地文件保留');
  } catch (err) {
    return report((isMail ? '告警邮件：' : '录像上传：') + safeFailure(err as Error));
  } finally {
    try { if (file) fileIo.closeSync(file); } finally { active.delete(path); }
  }
}
