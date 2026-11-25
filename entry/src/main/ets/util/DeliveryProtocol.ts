// Transport independent: tests supply only a loopback dialer. No file paths or platform errors go on the wire.
export class MailSettings {
  boundTo: string = '';
  enabled: boolean = false;
  host: string = '';
  port: string = '465';
  user: string = '';
  pass: string = '';
  from: string = '';
  to: string = '';
}
export class FtpSettings {
  boundTo: string = '';
  enabled: boolean = false;
  host: string = '';
  port: string = '21';
  user: string = '';
  pass: string = '';
  dir: string = '';
  // Consent is scoped to a canonical host and port, never a global “allow cleartext” bit.
  consent: string = '';
}
export class DeliveryError extends Error {}
export function fail(reason: string): never { throw new DeliveryError(reason); }
export function safeFailure(err: Error): string {
  return err instanceof DeliveryError ? err.message : '操作失败，请检查连接、设备解锁状态及文件权限';
}
export const MAIL_LIMIT = 12 * 1024 * 1024;
export const FTP_LIMIT = 512 * 1024 * 1024;
export const STAMP = '[0-9]{4}-[0-9]{2}-[0-9]{2}\\.[0-9]{2}\\.[0-9]{2}\\.[0-9]{2}\\.[0-9]{4}';
const bmp = new RegExp('^IPS_' + STAMP + '\\.bmp$');
const mp4 = new RegExp('^(?:IPS_|ipc_)' + STAMP + '(?:_[1-9][0-9]{0,8})?\\.mp4$');
export function artifactName(root: string, path: string): string {
  const name = path.substring(path.lastIndexOf('/') + 1);
  if (/[\r\n]/.test(path) || !(bmp.test(name) || mp4.test(name))) return '';
  return path === root + (name.startsWith('IPS_') ? '/MD/' : '/') + name ? name : '';
}
function validHost(host: string): boolean {
  return host.length > 0 && host.length <= 253 && !/[^a-zA-Z0-9.-]/.test(host) && host.split('.').every((label: string) =>
    /^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$/.test(label));
}
export function endpoint(host: string, port: string): string {
  if (!validHost(host) || !/^[1-9][0-9]{0,4}$/.test(port) || Number(port) > 65535) return '';
  return host.toLowerCase() + ':' + port;
}
type Account = MailSettings | FtpSettings;
export function authTarget(mail: boolean, c: Account): string {
  const address = endpoint(c.host, c.port);
  return address ? (mail ? 'smtps://' : 'ftp://') + address : '';
}
function clearAccount(mail: boolean, c: Account): void {
  c.enabled = false; c.user = ''; c.pass = ''; c.boundTo = '';
  if (!mail) (c as FtpSettings).consent = '';
}
// Call on every editor change, not only on blur or save. Returning A -> B -> A cannot revive A's secrets.
export function editDestination(mail: boolean, c: Account, host: string, port: string): boolean {
  const old = authTarget(mail, c);
  const h = host.trim(), p = port.trim();
  if (c.host === h && c.port === p) return false;
  c.host = h; c.port = p;
  if (old && old === authTarget(mail, c)) return false;
  clearAccount(mail, c);
  return true;
}
export function editCredential(mail: boolean, c: Account, user: boolean, value: string): void {
  const target = authTarget(mail, c);
  if (!target) { clearAccount(mail, c); return; }
  // Never stamp a new binding over an existing unbound or differently-bound secret.
  if (c.boundTo !== target) clearAccount(mail, c);
  if (user) c.user = value; else c.pass = value;
  c.boundTo = target;
  if (!c.user || !c.pass) { c.enabled = false; if (!mail) (c as FtpSettings).consent = ''; }
}
export function credentialsBound(mail: boolean, c: Account): boolean {
  const target = authTarget(mail, c);
  return !!target && c.boundTo === target && !!c.user && !!c.pass;
}
// Unbound legacy records, records from a different protocol, or mismatching records are inert on load.
export function restoreBinding(mail: boolean, c: Account): void {
  const target = authTarget(mail, c);
  if (!target || !c.boundTo || c.boundTo !== target) clearAccount(mail, c);
  else if (!credentialsBound(mail, c)) { c.enabled = false; if (!mail) (c as FtpSettings).consent = ''; }
}
export function checkStoredBinding(mail: boolean, c: Account): void {
  if ((c.user || c.pass || c.boundTo) && (!c.boundTo || c.boundTo !== authTarget(mail, c)))
    fail(mail ? '请重新填写当前 SMTP 服务器的用户名和密码' : '请检查 FTP 用户名和密码');
}
function validField(value: string): boolean { return typeof value === 'string' && value.length <= 240 && !/[\x00-\x1f\x7f]/.test(value); }
function mailbox(s: string): boolean {
  const fields = s.split('@');
  return validField(s) && fields.length === 2 && fields[0].length <= 64 && /^[A-Za-z0-9_+%-]+(?:\.[A-Za-z0-9_+%-]+)*$/.test(fields[0]) && validHost(fields[1]);
}
export function checkMail(c: MailSettings): void {
  if (c.enabled !== true) fail('SMTP 未开启');
  if (!endpoint(c.host, c.port) || !mailbox(c.from) || !mailbox(c.to)) fail('请填写有效的 SMTP 主机、端口、发件及收件地址');
  if (!credentialsBound(true, c) || !validField(c.user) || !validField(c.pass)) fail('请重新填写当前 SMTP 服务器的用户名和密码');
}
export function checkFtp(c: FtpSettings): void {
  if (c.enabled !== true) fail('FTP 未开启');
  const target = endpoint(c.host, c.port);
  if (!target || c.consent !== target) fail('请先确认对该服务器使用明文 FTP');
  if (!credentialsBound(false, c) || !validField(c.user) || !validField(c.pass)) fail('请检查 FTP 用户名和密码');
  if (!validField(c.dir) || (c.dir && !/^\/?[A-Za-z0-9_-]+(?:\/[A-Za-z0-9_-]+)*\/?$/.test(c.dir) && c.dir !== '/')) fail('目录仅支持字母、数字、横线、下划线及斜线');
}
export interface Part {
  name: string;
  size: number;
  read(length: number): Promise<Uint8Array>;
}
export interface Wire {
  line(): Promise<string>;
  write(data: string | ArrayBuffer): Promise<void>;
  close(): Promise<void>;
  peer(): Promise<string>;
}
export interface Dialer {
  dial(host: string, port: number, tls: boolean): Promise<Wire>;
  base64(data: Uint8Array): string;
  utf8(data: string): Uint8Array;
}
class Reply {
  code: number = 0;
  text: string = '';
}
async function reply(w: Wire): Promise<Reply> {
  const r = new Reply();
  for (let i = 0; i < 64; i++) {
    const line = await w.line();
    if (line.length > 2048 || r.text.length + line.length > 16384) fail('服务器响应超限');
    const match = /^([0-9]{3})([ -])(.*)$/.exec(line);
    if (!i) {
      if (!match) fail('服务器响应不是预期协议');
      r.code = Number(match[1]);
    }
    r.text += '\n' + line;
    if (match && Number(match[1]) === r.code && match[2] === ' ') return r;
  }
  return fail('服务器多行响应超限');
}
async function expect(w: Wire, codes: number[], stage: string): Promise<Reply> {
  const r = await reply(w);
  // Never show response text: a remote server can echo addresses or AUTH/PASS arguments.
  if (!codes.includes(r.code)) fail(stage + '失败（服务器状态 ' + r.code + '）');
  return r;
}
async function cmd(w: Wire, s: string): Promise<void> { await w.write(s + '\r\n'); }
async function step(w: Wire, s: string, codes: number[], stage: string): Promise<Reply> {
  await cmd(w, s);
  return expect(w, codes, stage);
}
function limit(p: Part, mail: boolean): void {
  if (!(mail ? bmp : mp4).test(p.name) || /[\r\n]/.test(p.name)) fail('只允许本应用完成的告警抓拍或录像');
  if (!Number.isSafeInteger(p.size) || p.size < 1 || p.size > (mail ? MAIL_LIMIT : FTP_LIMIT)) {
    fail(mail ? '抓拍为空或超过 12 MiB 附件上限' : '录像为空或超过 512 MiB 上传上限');
  }
}
async function block(p: Part, n: number): Promise<Uint8Array> {
  const data = await p.read(n);
  if (data.length !== n) fail('文件读取不完整，已中止');
  return data;
}
const boundary = '=_IPCamera_Motion_1'; // '=' cannot start a base64 line.
export async function mail(d: Dialer, c: MailSettings, p?: Part): Promise<void> {
  checkMail(c);
  if (p) limit(p, true);
  const w = await d.dial(c.host, Number(c.port), true);
  try {
    await expect(w, [220], 'SMTP 欢迎');
    const hello = await step(w, 'EHLO ipcamera.invalid', [250], 'SMTP EHLO');
    if (c.user) {
      const caps = hello.text.toUpperCase().split('\n').filter((s: string) => /^250[ -]AUTH[ =]/.test(s)).join(' ');
      if (/\bPLAIN\b/.test(caps)) {
        const auth = d.base64(d.utf8('\x00' + c.user + '\x00' + c.pass));
        await step(w, 'AUTH PLAIN ' + auth, [235], 'SMTP 认证');
      } else if (/\bLOGIN\b/.test(caps)) {
        await step(w, 'AUTH LOGIN', [334], 'SMTP 认证');
        await step(w, d.base64(d.utf8(c.user)), [334], 'SMTP 认证');
        await step(w, d.base64(d.utf8(c.pass)), [235], 'SMTP 认证');
      } else fail('服务器未提供 PLAIN 或 LOGIN 认证');
    }
    await step(w, 'MAIL FROM:<' + c.from + '>', [250], '发件人');
    await step(w, 'RCPT TO:<' + c.to + '>', [250, 251], '收件人');
    await step(w, 'DATA', [354], 'SMTP DATA');
    await w.write('Date: ' + new Date().toUTCString() + '\r\nFrom: <' + c.from + '>\r\nTo: <' + c.to +
      '>\r\nSubject: IPCamera ' + (p ? 'motion alert' : 'delivery test') + '\r\nMIME-Version: 1.0\r\n');
    if (p) {
      await w.write('Content-Type: multipart/mixed; boundary="' + boundary + '"\r\n\r\n--' + boundary +
        '\r\nContent-Type: text/plain; charset=us-ascii\r\n\r\nMotion detected.\r\n--' + boundary +
        '\r\nContent-Type: image/bmp\r\nContent-Disposition: attachment; filename="' + p.name +
        '"\r\nContent-Transfer-Encoding: base64\r\n\r\n');
      // 57 input bytes encode to 76 MIME columns. Only the final block may be padded.
      for (let pos = 0; pos < p.size;) {
        const n = Math.min(57 * 512, p.size - pos);
        const b64 = d.base64(await block(p, n));
        const lines: string[] = [];
        for (let j = 0; j < b64.length; j += 76) lines.push(b64.substring(j, j + 76));
        await w.write(lines.join('\r\n') + '\r\n');
        pos += n;
      }
      await cmd(w, '--' + boundary + '--');
    } else await w.write('Content-Type: text/plain; charset=us-ascii\r\n\r\nIPCamera SMTP delivery test. No attachment.\r\n');
    await step(w, '.', [250], '邮件提交');
  } finally { await w.close(); }
}
function passivePort(r: Reply): number {
  let port = 0;
  if (r.code === 229) {
    const m = /\(\|\|\|([0-9]{1,5})\|\)/.exec(r.text);
    if (m) port = Number(m[1]);
  } else {
    const m = /\(([0-9]{1,3}(?:,[0-9]{1,3}){5})\)/.exec(r.text);
    if (m) {
      const b = m[1].split(',').map((s: string) => Number(s));
      if (b.every((v: number) => v <= 255)) port = b[4] * 256 + b[5];
    }
  }
  if (port < 1024 || port > 65535) fail('不安全或无效的 FTP 被动端口');
  return port;
}
export async function ftp(d: Dialer, c: FtpSettings, p?: Part): Promise<void> {
  checkFtp(c);
  if (p) limit(p, false);
  const w = await d.dial(c.host, Number(c.port), false);
  try {
    await expect(w, [220], 'FTP 欢迎');
    const user = await step(w, 'USER ' + c.user, [230, 331], 'FTP 用户');
    if (user.code === 331) await step(w, 'PASS ' + c.pass, [230], 'FTP 认证');
    if (c.dir) await step(w, 'CWD ' + c.dir, [250], 'FTP 目录');
    await step(w, 'TYPE I', [200], 'FTP 二进制模式');
    if (!p) { await step(w, 'PWD', [257], 'FTP 目录检查'); return; }
    let r = await step(w, 'EPSV', [229, 500, 501, 502, 504, 522], 'FTP 被动模式');
    if (r.code !== 229) r = await step(w, 'PASV', [227], 'FTP 被动模式');
    // Never use the PASV advertised IP and never resolve the configured hostname a second time.
    const data = await d.dial(await w.peer(), passivePort(r), false);
    try {
      await step(w, 'STOR ' + p.name + '.part', [125, 150], 'FTP 开始上传');
      for (let pos = 0; pos < p.size;) {
        const n = Math.min(64 * 1024, p.size - pos);
        const b = await block(p, n);
        await data.write(b.buffer.slice(b.byteOffset, b.byteOffset + n) as ArrayBuffer);
        pos += n;
      }
    } finally { await data.close(); }
    await expect(w, [226, 250], 'FTP 结束上传');
    await step(w, 'RNFR ' + p.name + '.part', [350], 'FTP 重命名临时文件');
    await step(w, 'RNTO ' + p.name, [250], 'FTP 发布录像');
  } finally { await w.close(); }
}
