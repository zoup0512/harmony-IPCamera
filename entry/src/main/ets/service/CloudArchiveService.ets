import { asset } from '@kit.AssetStoreKit';
import { fileIo } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';
import { BusinessError } from '@kit.BasicServicesKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import streaming, { CloudRecordingPin } from 'libstreaming.so';
import { archive, Challenge, CHUNK, Clock, ContentReceipt, deviceLogin, finalized, Login, recordingName, Reply, Source, Transport, usable, verifyContent } from '../util/CloudArchiveProtocol';

// ASSET plaintext per item is bounded. Keep even long bearer tokens entirely inside ASSET,
// indexed by alias; publish a part count last. No preferences, backups, refresh tokens or logs.
const GROUP = 'cloud.archive.v1';
function bytes(text: string): Uint8Array { return new util.TextEncoder().encodeInto(text); }
function group(): asset.AssetMap {
  const query: asset.AssetMap = new Map();
  query.set(asset.Tag.DATA_LABEL_CRITICAL_1, bytes(GROUP));
  return query;
}
function forget(): void {
  // The device API accepts an alias (not a label) for removal. Enumerate even
  // uncommitted chunks after an interrupted write, without requesting their values.
  const q = group();
  q.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ATTRIBUTES);
  q.set(asset.Tag.RETURN_LIMIT, 35);
  let rows: asset.AssetMap[];
  try { rows = asset.querySync(q); }
  catch (e) { if ((e as BusinessError).code === asset.ErrorCode.NOT_FOUND) return; throw new Error('vault'); }
  for (const row of rows) {
    const alias = row.get(asset.Tag.ALIAS) as Uint8Array;
    const key: asset.AssetMap = new Map(); key.set(asset.Tag.ALIAS, alias);
    asset.removeSync(key);
  }
}
function put(key: string, value: string): void {
  const item = group();
  item.set(asset.Tag.ALIAS, bytes(GROUP + '.' + key));
  item.set(asset.Tag.SECRET, bytes(value));
  item.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_UNLOCKED);
  item.set(asset.Tag.SYNC_TYPE, asset.SyncType.NEVER);
  item.set(asset.Tag.IS_PERSISTENT, false);
  asset.addSync(item);
}
function get(key: string): string {
  const q = group();
  q.set(asset.Tag.ALIAS, bytes(GROUP + '.' + key));
  q.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
  try {
    const rows = asset.querySync(q);
    if (!rows.length) return '';
    const secret = rows[0].get(asset.Tag.SECRET) as Uint8Array;
    return util.TextDecoder.create('utf-8').decodeToString(secret);
  } catch (e) {
    if ((e as BusinessError).code === asset.ErrorCode.NOT_FOUND) return '';
    throw new Error('vault');
  }
}
function save(login: Login): void {
  const text = JSON.stringify(login); // all fields constrained to ASCII by protocol
  const count = Math.ceil(text.length / 1000);
  if (count < 1 || count > 34) throw new Error('vault');
  forget();
  try {
    for (let i = 0; i < count; i++) put(`${i}`, text.substring(i * 1000, (i + 1) * 1000));
    put('count', `${count}`);
  } catch (_) { forget(); throw new Error('vault'); }
}
function load(): Login | undefined {
  const index = get('count');
  if (!index) return undefined;
  const count = Number(index);
  if (!Number.isInteger(count) || count < 1 || count > 34) throw new Error('vault');
  let text = '';
  for (let i = 0; i < count; i++) {
    const part = get(`${i}`);
    if (!part) throw new Error('vault');
    text += part;
  }
  const login = JSON.parse(text) as Login;
  if (!usable(login, Date.now())) { forget(); return undefined; }
  return login;
}

class SystemClock implements Clock {
  now(): number { return Date.now(); }
  sleep(ms: number): Promise<void> { return new Promise<void>((resolve) => { setTimeout(resolve, ms); }); }
}
class SecureHttp implements Transport {
  current?: http.HttpRequest;
  cancel(): void { this.current?.destroy(); this.current = undefined; }
  async send(method: string, url: string, headers: Record<string, string>, body?: string | ArrayBuffer): Promise<Reply> {
    const req = http.createHttp();
    this.current = req;
    try {
      const opts: http.HttpRequestOptions = { method: method === 'GET' ? http.RequestMethod.GET :
        method === 'POST' ? http.RequestMethod.POST : http.RequestMethod.PUT, header: headers, extraData: body,
        expectDataType: http.HttpDataType.STRING, usingCache: false, maxRedirects: 0, maxLimit: 128 * 1024,
        connectTimeout: 15000, readTimeout: 60000 };
      const result = await req.request(url, opts);
      return { status: result.responseCode, text: result.result as string };
    } finally { if (this.current === req) { this.current = undefined; req.destroy(); } }
  }
}
export interface ArchiveRow { name: string; size: number; mtime: number; identity: string }
function identity(st: fileIo.Stat): string { return `${st.ino}/${st.size}/${st.mtimeNs ?? st.mtime}/${st.ctimeNs ?? st.ctime}`; }
function pathFor(dir: string, name: string): string {
  if (!recordingName(name)) throw new Error('source');
  if (name.startsWith('IPS_')) {
    if (!fileIo.lstatSync(dir + '/MD').isDirectory()) throw new Error('source');
    return `${dir}/MD/${name}`;
  }
  return `${dir}/${name}`;
}
export function recordings(dir: string): ArchiveRow[] {
  const rows: ArchiveRow[] = [];
  for (const folder of ['', '/MD']) {
    let names: string[];
    try {
      if (!fileIo.lstatSync(dir + folder).isDirectory()) continue;
      names = fileIo.listFileSync(dir + folder);
    } catch (_) { continue; }
    for (const name of names) {
      if (!recordingName(name) || (folder === '/MD') !== name.startsWith('IPS_')) continue;
      try {
        const path = pathFor(dir, name), st = fileIo.lstatSync(path);
        if (st.isFile() && st.size > 0 && !streaming.recordingFileBusy(path)) {
          rows.push({ name: name, size: st.size, mtime: st.mtime, identity: identity(st) });
        }
      } catch (_) { /* rotated away or not a readable regular file */ }
    }
  }
  return rows.sort((a: ArchiveRow, b: ArchiveRow) => b.mtime - a.mtime || b.name.localeCompare(a.name));
}
class Recording implements Source {
  name: string;
  size: number;
  private dir: string;
  private file: fileIo.File;
  private snapshot: string;
  constructor(dir: string, row: ArchiveRow) {
    this.dir = dir; this.name = row.name; this.size = row.size; this.snapshot = row.identity;
    const path = pathFor(dir, row.name);
    const st = fileIo.lstatSync(path);
    if (!st.isFile() || identity(st) !== row.identity || streaming.recordingFileBusy(path)) throw new Error('source');
    this.file = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY | fileIo.OpenMode.NOFOLLOW);
  }
  pin(): CloudRecordingPin | undefined {
    return streaming.pinCloudRecording(this.dir, this.name, this.file.fd);
  }
  check(): void {
    const path = pathFor(this.dir, this.name), st = fileIo.lstatSync(path);
    if (!st.isFile() || streaming.recordingFileBusy(path) || identity(st) !== this.snapshot ||
      identity(fileIo.statSync(this.file.fd)) !== this.snapshot) throw new Error('source');
  }
  async read(offset: number, length: number): Promise<ArrayBuffer> {
    fileIo.lseek(this.file.fd, offset, fileIo.WhenceType.SEEK_SET);
    const buf = new ArrayBuffer(length);
    if (await fileIo.read(this.file.fd, buf) !== length) throw new Error('source');
    return buf;
  }
  close(): void { fileIo.closeSync(this.file); }
}
interface Candidate { pin: CloudRecordingPin; remote: ContentReceipt; login: Login; identity: string }

async function localSha(src: Source, guard: () => void): Promise<string> {
  const md = cryptoFramework.createMd('SHA256');
  for (let pos = 0; pos < src.size; pos += CHUNK) {
    guard();
    const buf = await src.read(pos, Math.min(CHUNK, src.size - pos));
    await md.update({ data: new Uint8Array(buf) });
  }
  const hash = (await md.digest()).data;
  guard();
  let hex = '';
  for (const byte of hash) hex += byte.toString(16).padStart(2, '0');
  return hex;
}
export class CloudArchiveService {
  private net: SecureHttp = new SecureHttp();
  private clock: SystemClock = new SystemClock();
  private generation: number = 0;
  private candidate?: Candidate;
  cancel(): void {
    this.generation++; this.net.cancel();
    this.candidate?.pin.close(); this.candidate = undefined;
  }
  private sameGrant(login: Login): boolean {
    const current = load();
    return !!current && current.token === login.token && current.client === login.client && current.expires === login.expires && usable(login, this.clock.now());
  }
  cleanupExpires(): number { return this.candidate?.login.expires ?? 0; }
  canClean(row: ArchiveRow): boolean {
    const c = this.candidate;
    try { return !!c && c.identity === row.identity && row.name === c.remote.name && row.size === c.remote.size && this.sameGrant(c.login) && c.pin.current(); }
    catch (_) { return false; }
  }
  // One-shot; called only from the separate single-file confirmation dialog.
  async cleanConfirmed(row: ArchiveRow): Promise<void> {
    const c = this.candidate, gen = this.generation;
    if (!c) throw new Error('proof');
    const eligible = this.canClean(row);
    this.candidate = undefined;
    try {
      if (!eligible) throw new Error('proof');
      await verifyContent(this.net, c.login, this.clock, c.remote, () => this.guard(gen));
      this.guard(gen);
      if (!this.sameGrant(c.login) || !c.pin.remove()) throw new Error('proof');
    } finally { c.pin.close(); }
  }
  private guard(gen: number): void { if (gen !== this.generation) throw new Error('cancelled'); }
  account(): string { return load()?.client ?? ''; }
  disconnect(): void { this.cancel(); forget(); }
  async connect(client: string, tenant: string, show: (challenge: Challenge) => void): Promise<void> {
    this.cancel();
    const gen = this.generation;
    const login = await deviceLogin(this.net, this.clock, client, () => this.guard(gen), show, tenant);
    this.guard(gen);
    save(login);
  }
  async upload(dir: string, row: ArchiveRow, progress: (bytes: number) => void): Promise<boolean> {
    this.cancel();
    const gen = this.generation, login = load();
    if (!login) throw new Error('expired');
    const src = new Recording(dir, row);
    let pin: CloudRecordingPin | undefined = undefined;
    try {
      src.check();
      // Pin before reading any upload bytes. Missing capability never upgrades an ordinary upload.
      try { pin = src.pin(); } catch (_) { /* metadata-only */ }
      if (!await finalized(src)) throw new Error('source');
      const remote = await archive(this.net, login, this.clock, src, () => this.guard(gen), progress);
      if (!remote || !pin) return false;
      try {
        const sha = await localSha(src, () => { this.guard(gen); src.check(); });
        if (sha !== remote.sha || !pin.current() || !this.sameGrant(login)) return false;
        this.candidate = { pin: pin, remote: remote, login: login, identity: row.identity };
        pin = undefined;
        return true;
      } catch (_) { this.guard(gen); return false; }
    } finally { pin?.close(); src.close(); }
  }
}
