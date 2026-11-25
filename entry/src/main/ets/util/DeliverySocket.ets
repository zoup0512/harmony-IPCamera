import { socket } from '@kit.NetworkKit';
import { cert } from '@kit.DeviceCertificateKit';
import { util } from '@kit.ArkTS';
import { Dialer, Wire, fail, DeliveryError } from './DeliveryProtocol';

// Inspect only the SDK-extracted subjectAltName extension. Chain/time/signature validation
// is mandatory in TLSSocket itself. Never use the CN, a URI/email SAN, or substring matches.
class DerField {
  tag: number = 0;
  start: number = 0;
  end: number = 0;
}
function field(b: Uint8Array, i: number, end: number): DerField {
  if (i + 2 > end) fail('无效的证书 SAN');
  const f = new DerField();
  f.tag = b[i++];
  let n = b[i++];
  if (n >= 128) {
    const k = n - 128;
    if (!k || k > 3 || i + k > end || b[i] === 0) fail('无效的证书 SAN');
    n = 0;
    for (let j = 0; j < k; j++) n = 256 * n + b[i++];
    if (n < 128) fail('无效的证书 SAN');
  }
  if (i + n > end) fail('无效的证书 SAN');
  f.start = i;
  f.end = i + n;
  return f;
}
export function matchesSan(host: string, bytes: Uint8Array, confirm: (name: Uint8Array) => boolean): boolean {
  const h = host.toLowerCase();
  const f = field(bytes, 0, bytes.length);
  // matchesLeaf already removed extnValue. Only GeneralNames (one SEQUENCE) belongs here.
  if (f.tag !== 48 || f.end !== bytes.length) return false;
  const numeric = /^[0-9.]+$/.test(h);
  for (let i = f.start; i < f.end;) {
    const n = field(bytes, i, f.end);
    if (numeric && n.tag === 135 && n.end - n.start === 4 && bytes.slice(n.start, n.end).join('.') === h) return confirm(bytes.slice(i, n.end));
    if (!numeric && n.tag === 130) {
      let dns = '';
      for (let j = n.start; j < n.end; j++) dns += String.fromCharCode(bytes[j]);
      dns = dns.toLowerCase();
      if (dns === h || (h.split('.').length >= 3 && dns === '*' + h.substring(h.indexOf('.')))) return confirm(bytes.slice(i, n.end));
    }
    i = n.end;
  }
  return false;
}

// Parse only the structure leading to the extnValue of OID 2.5.29.17 in a platform-validated
// DER leaf. Do not depend on an SDK's human-readable extension formatting or parse a subject DN.
function matchesLeaf(host: string, der: Uint8Array, confirm: (name: Uint8Array) => boolean): boolean {
  const certSeq = field(der, 0, der.length);
  if (certSeq.tag !== 48 || certSeq.end !== der.length) return false;
  const tbs = field(der, certSeq.start, certSeq.end);
  if (tbs.tag !== 48) return false;
  for (let i = tbs.start; i < tbs.end;) {
    const child = field(der, i, tbs.end); i = child.end;
    if (child.tag !== 163) continue;
    const exts = field(der, child.start, child.end);
    if (exts.tag !== 48 || exts.end !== child.end) return false;
    for (let j = exts.start; j < exts.end;) {
      const ext = field(der, j, exts.end); j = ext.end;
      if (ext.tag !== 48) return false;
      const id = field(der, ext.start, ext.end);
      if (id.tag !== 6 || id.end - id.start !== 3 || der[id.start] !== 85 || der[id.start+1] !== 29 || der[id.start+2] !== 17) continue;
      let value = field(der, id.end, ext.end);
      if (value.tag === 1) value = field(der, value.end, ext.end); // optional critical flag
      return value.tag === 4 && value.end === ext.end && matchesSan(host, der.subarray(value.start, value.end), confirm);
    }
  }
  return false;
}
const IO_MS = 15000;
class PlatformWire implements Wire {
  private tcp?: socket.TCPSocket;
  private tls?: socket.TLSSocket;
  private buf: string = '';
  private ended: boolean = false;
  private closed: boolean = false;
  private wake?: () => void;
  private deadline: number = Date.now() + 600000;
  private authorized: () => boolean;
  constructor(authorized: () => boolean) { this.authorized = authorized; }

  async wait<T>(op: Promise<T>): Promise<T> {
    let timer = -1;
    try {
      const timeout = new Promise<T>((_resolve, reject) => {
        timer = setTimeout(() => reject(new DeliveryError('网络操作超时')), Math.max(1, Math.min(IO_MS, this.deadline - Date.now())));
      });
      if (Date.now() >= this.deadline) fail('传输总时限已到');
      return await Promise.race([op, timeout]);
    } finally { clearTimeout(timer); }
  }
  private changed(): void {
    const fn = this.wake;
    this.wake = undefined;
    if (fn) fn();
  }
  async connect(host: string, port: number, encrypted: boolean): Promise<void> {
    const onEnd = (): void => { this.ended = true; this.changed(); };
    const onMessage = (m: socket.SocketMessageInfo): void => {
      if (this.ended) return;
      if (this.buf.length + m.message.byteLength > 32768) { this.buf = ''; onEnd(); return; }
      this.buf += util.TextDecoder.create('utf-8').decodeToString(new Uint8Array(m.message));
      this.changed();
    };
    // IPv4 socket, explicit configured DNS name/IPv4; no discovery, proxy or fallback.
    const local: socket.NetAddress = { address: '0.0.0.0', family: 1, port: 0 };
    const remote: socket.NetAddress = { address: host, family: 1, port: port };
    try {
      if (encrypted) {
        const s = socket.constructTLSSocketInstance();
        this.tls = s;
        s.on('message', onMessage); s.on('close', onEnd); s.on('error', onEnd);
        await this.wait(s.bind(local));
        await this.wait(s.connect({ address: remote, timeout: IO_MS, skipRemoteValidation: false,
          secureOptions: { protocols: [socket.Protocol.TLSv12, socket.Protocol.TLSv13] } }));
        const peer = await this.wait(s.getRemoteCertificate());
        const x509 = await cert.createX509Cert(peer);
        const leaf = peer.encodingFormat === cert.EncodingFormat.FORMAT_DER ? peer : await x509.getEncoded();
        const confirm = (der: Uint8Array): boolean => {
          const name: cert.GeneralName = { type: der[0] === 135 ? cert.GeneralNameType.GENERAL_NAME_TYPE_IP_ADDRESS :
            cert.GeneralNameType.GENERAL_NAME_TYPE_DNS_NAME, name: der };
          return x509.match({ subjectAlternativeNames: [name], matchAllSubjectAltNames: false });
        };
        if (leaf.encodingFormat !== cert.EncodingFormat.FORMAT_DER || !matchesLeaf(host, leaf.data, confirm)) fail('证书 SAN 与 SMTP 主机不符');
      } else {
        const s = socket.constructTCPSocketInstance();
        this.tcp = s;
        s.on('message', onMessage); s.on('close', onEnd); s.on('error', onEnd);
        await this.wait(s.bind(local));
        await this.wait(s.connect({ address: remote, timeout: IO_MS }));
      }
    } catch (err) {
      await this.close();
      if (err instanceof DeliveryError) throw err as DeliveryError;
      fail(encrypted ? 'SMTP TLS 连接或证书验证失败，请检查主机名、证书和隐式 TLS 端口' : 'FTP 连接失败，请检查主机、端口和网络');
    }
  }
  async line(): Promise<string> {
    const until = Date.now() + IO_MS;
    while (true) {
      const i = this.buf.indexOf('\r\n');
      if (i >= 0) {
        const s = this.buf.substring(0, i);
        this.buf = this.buf.substring(i + 2);
        return s;
      }
      if (this.ended || this.buf.length > 2048) fail('连接中断或服务器响应过长');
      if (Date.now() >= until) fail('等待服务器响应超时');
      await this.wait(new Promise<void>((resolve) => { this.wake = resolve; }));
    }
  }
  async write(data: string | ArrayBuffer): Promise<void> {
    if (!this.authorized()) fail('配置已更改，已中止传输');
    if (this.ended) fail('连接已中断');
    if (this.tls) await this.wait(this.tls.send(data));
    else if (this.tcp) await this.wait(this.tcp.send({ data: data }));
    else fail('连接未建立');
  }
  async peer(): Promise<string> {
    if (!this.tcp) return fail('无法取得 FTP 控制连接地址');
    const addr = await this.wait(this.tcp.getRemoteAddress());
    if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(addr.address)) return fail('FTP 控制连接不是 IPv4');
    return addr.address;
  }
  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    this.ended = true; this.changed();
    try {
      if (this.tls) await this.wait(this.tls.close());
      if (this.tcp) await this.wait(this.tcp.close());
    } catch (_) { /* No successful transfer is inferred from close. */ }
  }
}
export class PlatformDialer implements Dialer {
  private authorized: () => boolean;
  constructor(authorized: () => boolean) { this.authorized = authorized; }
  async dial(host: string, port: number, tls: boolean): Promise<Wire> {
    if (!this.authorized()) fail('配置已更改，已中止传输');
    const w = new PlatformWire(this.authorized);
    await w.connect(host, port, tls);
    return w;
  }
  base64(b: Uint8Array): string { return new util.Base64Helper().encodeToStringSync(b); }
  utf8(s: string): Uint8Array { return util.TextEncoder.create().encodeInto(s); }
}
