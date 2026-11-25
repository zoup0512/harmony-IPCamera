import { url, util } from '@kit.ArkTS';
import { http } from '@kit.NetworkKit';

const LIST_BYTES = 512 * 1024;
const LIST_ROWS = 512;
export const REMOTE_FILE_BYTES = 512 * 1024 * 1024;

export interface RemoteArchive {
  key: string;
  name: string;
  motion: boolean;
  size: number; // Android's HTML listing does not give sizes (-1).
}

// Only a literal HTTP(S) origin, never a stream URL, userinfo or a guessed RTSP port.
export function archiveOrigin(input: string): string {
  const raw = input.trim();
  if (raw.length > 512 || !/^https?:\/\/(?:[a-z0-9.-]+|\[[a-f0-9:.]+\])(?::[0-9]{1,5})?\/?$/i.test(raw)) {
    throw new Error('请输入不含账号、路径或查询串的 HTTP(S) 服务器地址');
  }
  const parsed = url.URL.parseURL(raw);
  if (!parsed.hostname || parsed.port === '0') throw new Error('服务器地址无效');
  return parsed.origin;
}

function addRow(rows: Map<string, RemoteArchive>, raw: string, motion: boolean, html: boolean, size: number): void {
  if (raw.length > 768) return;
  let name = raw;
  try { if (html) name = decodeURIComponent(raw); } catch (_) { return; }
  // Decode exactly once, then validate ONE basename before rebuilding a new path.
  // A residual % could be interpreted by a second decoder. Never use an href as a URL.
  if (name.length > 240 || /[\/\\%?#:\x00-\x1f\x7f-\x9f\u202a-\u202e\u2066-\u2069]/.test(name) || name.includes('..') ||
    !/\.(?:mp4|mkv)$/i.test(name) || !(motion || html ? /^ips_/i : /^ipc_/i).test(name)) return;
  const key = (motion ? 'MD/' : '') + name;
  if (rows.has(key)) return;
  if (rows.size >= LIST_ROWS) throw new Error('录像条目过多');
  rows.set(key, { key: key, name: name, motion: motion, size: size });
}

function parseListing(text: string): Map<string, RemoteArchive> {
  const rows = new Map<string, RemoteArchive>();
  if (text.trimStart().startsWith('<')) {
    // Android emits <li><a href="/media/[md/]IPS_...">. Ignore labels, external
    // URLs, <base>, photos and script links. Do not render the listing as HTML.
    const links = /<a\s+href\s*=\s*["']\/media\/(md\/)?([^"'<>\s/]{1,768})["'](?=[\s>])/gi;
    let link = links.exec(text);
    while (link !== null) {
      addRow(rows, link[2], !!link[1], true, -1);
      link = links.exec(text);
    }
  } else {
    for (const line of text.split('\n')) {
      const row = /^(MD\/)?([^/\r\n]{1,240}) ([0-9]{1,16})\r?$/i.exec(line);
      if (!row) continue;
      const size = Number(row[3]);
      if (Number.isSafeInteger(size)) addRow(rows, row[2], !!row[1], false, size);
    }
  }
  return rows;
}

export class RemoteArchiveClient {
  private readonly origin: string;
  private readonly auth: string;
  private rows: Map<string, RemoteArchive> = new Map();
  private abort?: () => void;
  private closed: boolean = false;

  constructor(origin: string, user: string, password: string) {
    this.origin = archiveOrigin(origin);
    if (user.includes(':') || user.length + password.length > 1024 || /[\x00-\x1f\x7f]/.test(user + password)) {
      throw new Error('账号或密码格式无效');
    }
    this.auth = user || password ? 'Basic ' + new util.Base64Helper().encodeToStringSync(
      util.TextEncoder.create().encodeInto(user + ':' + password)) : '';
  }

  close(): void {
    this.closed = true;
    this.rows.clear();
    if (this.abort) this.abort();
  }

  async list(): Promise<RemoteArchive[]> {
    this.rows.clear();
    const buf = new Uint8Array(LIST_BYTES);
    let offset = 0;
    await this.read('/getarchives', LIST_BYTES, -1, 30000, (data: ArrayBuffer) => {
      buf.set(new Uint8Array(data), offset);
      offset += data.byteLength;
    });
    if (this.closed) throw new Error('已取消');
    this.rows = parseListing(util.TextDecoder.create('utf-8').decodeToString(buf.subarray(0, offset)));
    // UI receives copies, not mutable references to the internal allowlist.
    const result: RemoteArchive[] = [];
    this.rows.forEach((r: RemoteArchive) => result.push({ key: r.key, name: r.name, motion: r.motion, size: r.size }));
    return result;
  }

  async file(key: string, sink: (data: ArrayBuffer) => void): Promise<void> {
    const r = this.rows.get(key);
    if (!r) throw new Error('请从当前服务器的列表中选择录像');
    if (r.size === 0 || r.size > REMOTE_FILE_BYTES) throw new Error('文件为空或超过 512 MiB');
    const path = '/get/' + (r.motion ? 'MD/' : '') + encodeURIComponent(r.name);
    const count = await this.read(path, REMOTE_FILE_BYTES, r.size, 600000, sink);
    if (count === 0) throw new Error('录像为空');
  }

  // NetworkKit reports HTTP status separately from stream events. Stage bytes only
  // in private storage until both status and EOF have arrived, in either order.
  private read(path: string, limit: number, expected: number, timeout: number,
    sink: (data: ArrayBuffer) => void): Promise<number> {
    if (this.closed || this.abort) return Promise.reject(new Error('请求不可用'));
    const req = http.createHttp();
    const headers: Record<string, string> = { 'Accept-Encoding': 'identity' };
    if (this.auth) headers['Authorization'] = this.auth;
    return new Promise<number>((resolve, reject) => {
      let settled = false;
      let ended = false;
      let ok = false;
      let count = 0;
      let declared = -1;
      let timer = -1;
      const finish = (error: string): void => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        this.abort = undefined;
        req.destroy();
        if (error) reject(new Error(error)); else resolve(count);
      };
      const check = (): void => {
        if (ok && ended) finish((expected >= 0 && count !== expected) || (declared >= 0 && count !== declared) ?
          '录像已变化或传输不完整' : '');
      };
      this.abort = () => finish('已取消');
      timer = setTimeout(() => finish('传输超时'), timeout);
      try {
        req.on('headersReceive', (obj: Object) => {
          if (settled) return;
          const fields = obj as Record<string, string>;
          for (const k of Object.keys(fields)) {
            if (k.toLowerCase() !== 'content-length') continue;
            const value = String(fields[k]);
            declared = Number(value);
            if (!/^\d+$/.test(value) || !Number.isSafeInteger(declared) || declared > limit ||
              (expected >= 0 && expected !== declared)) finish('响应长度超限或文件已变化');
          }
        });
        req.on('dataReceive', (data: ArrayBuffer) => {
          if (settled) return;
          count += data.byteLength;
          if (ended || count > limit || (expected >= 0 && count > expected) || (declared >= 0 && count > declared)) {
            finish('响应超过允许的长度'); return;
          }
          try { sink(data); } catch (_) { finish('无法缓存响应，请检查可用空间'); }
        });
        req.on('dataEnd', () => { ended = true; check(); });
        req.requestInStream(this.origin + path, { method: http.RequestMethod.GET, header: headers,
          maxRedirects: 0, usingProxy: false, usingCache: false, connectTimeout: 6000, readTimeout: 15000 })
          .then((code: number) => {
            if (code !== 200) { finish(code === 401 || code === 403 ? '服务器拒绝访问，请检查账号和密码' : `服务器响应 ${code}`); return; }
            ok = true; check();
          }).catch(() => finish('连接失败或重定向已拒绝'));
      } catch (_) { finish('无法启动请求'); }
    });
  }
}
