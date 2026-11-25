import { util } from '@kit.ArkTS';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { http } from '@kit.NetworkKit';
import { DEVICE, MEDIA, TT, SOAP, Endpoint, endpoint, esc, response, SoapElement } from './OnvifSoap';
import { EncoderEditor, EncoderValues } from './OnvifEncoder';

const OASIS = 'http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-';
const WSSE = OASIS + 'wssecurity-secext-1.0.xsd';
const WSU = OASIS + 'wssecurity-utility-1.0.xsd';
function security(user: string, password: string): string {
  const created = new Date().toISOString();
  const nonce = cryptoFramework.createRandom().generateRandomSync(24);
  const digest = cryptoFramework.createMd('SHA1'); // algorithm mandated by WS-Security UsernameToken Profile 1.0
  const enc = util.TextEncoder.create();
  const secret = enc.encodeInto(password);
  try {
    digest.updateSync(nonce);
    digest.updateSync({ data: enc.encodeInto(created) });
    digest.updateSync({ data: secret });
    const b64 = new util.Base64Helper();
    return `<s:Header><wsse:Security xmlns:wsse="${WSSE}" xmlns:wsu="${WSU}" s:mustUnderstand="true">` +
      `<wsse:UsernameToken><wsse:Username>${esc(user)}</wsse:Username>` +
      `<wsse:Password Type="${OASIS}username-token-profile-1.0#PasswordDigest">${b64.encodeToStringSync(digest.digestSync().data)}</wsse:Password>` +
      `<wsse:Nonce EncodingType="${OASIS}soap-message-security-1.0#Base64Binary">${b64.encodeToStringSync(nonce.data)}</wsse:Nonce>` +
      `<wsu:Created>${created}</wsu:Created></wsse:UsernameToken></wsse:Security></s:Header>`;
  } finally { secret.fill(0); }
}

export interface ManagedProfile { token: string; name: string; encoder: string; }
function token(value: string): string {
  if (!value || value.length > 128 || /[\x00-\x1f\x7f]/.test(value)) throw new Error('无效 Profile 或编码标识');
  return value;
}
export class OnvifManagedSession {
  readonly target: Endpoint;
  private address: string;
  private user: string;
  private password: string;
  private closed: boolean = false;
  private media: string = '';
  private profiles: ManagedProfile[] = [];
  private selected: number = -1;
  private editor?: EncoderEditor;
  private pending: string = '';
  private generation: number = 0;
  private request?: http.HttpRequest;

  constructor(address: string, user: string, password: string) {
    this.target = endpoint(address);
    if (!user.trim() || !password || user.length > 128 || password.length > 256 || /[\x00-\x1f\x7f]/.test(user)) {
      throw new Error('需要显式账号和密码');
    }
    this.address = address; this.user = user; this.password = password;
  }
  close(): void {
    this.closed = true; this.password = ''; this.user = ''; this.media = ''; this.address = '';
    this.profiles = []; this.selected = -1; this.invalidate();
    this.request?.destroy(); this.request = undefined;
  }
  private invalidate(): void { this.generation++; this.pending = ''; this.editor = undefined; }

  private async call(address: string, ns: string, action: string, content: string): Promise<SoapElement> {
    if (this.closed || this.request || endpoint(address).origin !== this.target.origin) throw new Error('会话或服务地址无效');
    const body = `<s:Envelope xmlns:s="${SOAP}" xmlns:tt="${TT}">${security(this.user, this.password)}` +
      `<s:Body><${action} xmlns="${ns}">${content}</${action}></s:Body></s:Envelope>`;
    const req = http.createHttp();
    this.request = req;
    try {
      // Fresh HTTP/1.1 connection for every POST. No redirects (including 307/308),
      // proxies, credentials cache, auth negotiation, TLS exceptions, or app retries.
      const res = await req.request(address, { method: http.RequestMethod.POST, extraData: body,
        header: { 'Content-Type': `application/soap+xml; charset=utf-8; action="${ns}/${action}"`,
          'Connection': 'close', 'Cache-Control': 'no-store' },
        maxRedirects: 0, usingCache: false, usingProxy: false, remoteValidation: 'system',
        usingProtocol: http.HttpProtocol.HTTP1_1, maxLimit: 524288, connectTimeout: 4000, readTimeout: 6000,
        expectDataType: http.HttpDataType.STRING });
      if (this.closed || res.responseCode !== 200 || typeof res.result !== 'string') throw new Error('请求未获成功响应');
      return response(res.result as string, ns, action);
    } finally {
      if (this.request === req) { req.destroy(); this.request = undefined; }
    }
  }
  async login(): Promise<ManagedProfile[]> {
    const cap = await this.call(this.address, DEVICE, 'GetCapabilities', '<Category>Media</Category>');
    const media = cap.one(DEVICE, 'Capabilities').one(TT, 'Media').one(TT, 'XAddr').value();
    if (endpoint(media).origin !== this.target.origin) throw new Error('拒绝跨源媒体服务');
    const list = await this.call(media, MEDIA, 'GetProfiles', '');
    const profiles: ManagedProfile[] = [];
    for (const p of list.all(MEDIA, 'Profiles')) {
      const id = token(p.attrs['token']);
      if (profiles.length >= 128 || profiles.some(x => x.token === id)) throw new Error('Profile 列表无效');
      const enc = p.optional(TT, 'VideoEncoderConfiguration');
      profiles.push({ token: id, name: p.optional(TT, 'Name')?.text.trim().slice(0, 80) ?? '',
        encoder: enc ? token(enc.attrs['token']) : '' });
    }
    if (profiles.length === 0) throw new Error('设备未提供 Media1 Profile');
    this.media = media; this.profiles = profiles;
    return profiles;
  }
  async choose(index: number): Promise<string> {
    this.selected = -1; this.invalidate();
    const gen = this.generation;
    const p = this.profiles[index];
    if (!p) throw new Error('未选择 Profile');
    const r = await this.call(this.media, MEDIA, 'GetStreamUri',
      `<StreamSetup><tt:Stream>RTP-Unicast</tt:Stream><tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></StreamSetup>` +
      `<ProfileToken>${esc(p.token)}</ProfileToken>`);
    const uri = r.one(MEDIA, 'MediaUri').one(TT, 'Uri').value();
    if (this.generation !== gen || endpoint(uri, true).host !== this.target.host) throw new Error('流地址不是当前设备');
    this.selected = index;
    return uri; // Treat even a URI without userinfo as secret (query/path may contain a bearer token).
  }
  async readEncoder(): Promise<EncoderEditor> {
    this.invalidate();
    const gen = this.generation;
    const p = this.profiles[this.selected];
    if (!p || !p.encoder) throw new Error('此 Profile 没有编码配置');
    const cfg = `<ConfigurationToken>${esc(p.encoder)}</ConfigurationToken>`;
    const r = await this.call(this.media, MEDIA, 'GetVideoEncoderConfiguration', cfg);
    const opt = await this.call(this.media, MEDIA, 'GetVideoEncoderConfigurationOptions', cfg + `<ProfileToken>${esc(p.token)}</ProfileToken>`);
    if (gen !== this.generation) throw new Error('选择已更改');
    const editor = new EncoderEditor(r.one(MEDIA, 'Configuration'), opt.one(MEDIA, 'Options'), p.encoder);
    this.editor = editor;
    return editor;
  }
  preview(v: EncoderValues): number {
    this.pending = ''; this.generation++;
    if (!this.editor) throw new Error('请重新读取配置');
    this.pending = this.editor.prepare(v);
    return this.generation;
  }
  cancel(ticket: number): void { if (ticket === this.generation) { this.pending = ''; this.generation++; } }
  async confirm(ticket: number): Promise<void> {
    if (this.closed || !this.pending || ticket !== this.generation) throw new Error('确认已失效');
    const body = this.pending;
    this.invalidate(); // Consume before awaiting, even on disconnect, fault or ambiguous timeout.
    await this.call(this.media, MEDIA, 'SetVideoEncoderConfiguration', body + '<ForcePersistence>true</ForcePersistence>');
  }
}
