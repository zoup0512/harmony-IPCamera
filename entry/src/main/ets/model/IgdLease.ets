import { igdXml, IgdElement } from './IgdXml';

const DEVICE_NS = 'urn:schemas-upnp-org:device-1-0';
const WAN = 'urn:schemas-upnp-org:service:WANIPConnection:2';
const SOAP11 = 'http://schemas.xmlsoap.org/soap/envelope/';
const POLICY = 'urn:ipcamera:igd:create-only:1';
function deny(): never { throw new Error('Unsupported IGD lease'); }
export function validPort(n: number): boolean { return Number.isInteger(n) && n > 0 && n <= 65535; }
export function leaseCredentials(user: string, password: string): boolean {
  // Stay inside both native UTF-8 buffers, reject truncation, controls, Basic delimiters and weak/default secrets.
  return /^[A-Za-z0-9._-]{1,63}$/.test(user) && /^[\x21-\x7e]{12,63}$/.test(password) &&
    !/\s/.test(user + password) && password.toLowerCase() !== user.toLowerCase() && new Set(password.split('')).size >= 6 &&
    /[a-z]/.test(password) && /[A-Z]/.test(password) && /[0-9]/.test(password) &&
    !/^(admin|password|qwerty|123456|letmein|changeme|ipcamera)[\W\d]*$/i.test(password);
}
export function ipv4(s: string): number {
  const a = s.split('.').map((v: string) => Number(v));
  if (a.length !== 4 || a.join('.') !== s || a.some((n: number) => !Number.isInteger(n) || n < 0 || n > 255)) deny();
  return a.reduce((n: number, b: number) => n * 256 + b, 0);
}
export function privateHost(s: string): boolean {
  ipv4(s);
  return /^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)/.test(s);
}
export interface IgdEndpoint { host: string; port: number; path: string; }
export function igdEndpoint(url: string): IgdEndpoint {
  const m = /^https:\/\/([0-9.]+)(?::([1-9]\d{0,4}))?(\/[a-zA-Z0-9_./-]+)$/.exec(url);
  if (!m || m[0] !== url || url.length > 512 || !privateHost(m[1]) || !validPort(Number(m[2] ?? 443)) ||
    m[3].split('/').slice(1).some((s: string) => !s || s === '..' || s === '.')) deny();
  return { host: m[1], port: Number(m[2] ?? 443), path: m[3] };
}
function device(node: IgdElement, kind: string): IgdElement {
  const d = node.one(DEVICE_NS, 'device');
  if (d.one(DEVICE_NS, 'deviceType').value() !== 'urn:schemas-upnp-org:device:' + kind + ':2') deny();
  return d;
}
export function controlPath(raw: string, ep: IgdEndpoint, challenge: string): string {
  const root = igdXml(raw);
  if (root.uri !== DEVICE_NS || root.name !== 'root' || root.optional(DEVICE_NS, 'URLBase')) deny();
  const igd = device(root, 'InternetGatewayDevice');
  const wan = device(igd.one(DEVICE_NS, 'deviceList'), 'WANDevice');
  const conn = device(wan.one(DEVICE_NS, 'deviceList'), 'WANConnectionDevice');
  const services = conn.one(DEVICE_NS, 'serviceList').all(DEVICE_NS, 'service')
    .filter((s: IgdElement) => s.one(DEVICE_NS, 'serviceType').value() === WAN);
  // Admin-only pairing challenge, advertised ONLY by the gateway's atomic create-only handler.
  // A standard v2 descriptor or mere user attestation is insufficient; never probe legacy services.
  if (services.length !== 1 || services[0].one(POLICY, 'createOnlyChallenge').value() !== challenge ||
    !/^[0-9a-f]{32}$/.test(challenge)) deny();
  const value = services[0].one(DEVICE_NS, 'controlURL').value();
  const control = igdEndpoint(value.startsWith('/') ? `https://${ep.host}:${ep.port}${value}` : value);
  if (control.host !== ep.host || control.port !== ep.port || control.path === ep.path) deny();
  return control.path;
}
export function leaseBody(client: string, port: number, challenge: string): string {
  if (!privateHost(client) || !validPort(port) || challenge.length !== 32 || !/^[0-9a-f]{32}$/.test(challenge)) deny();
  return `<s:Envelope xmlns:s="${SOAP11}" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>` +
    `<u:AddAnyPortMapping xmlns:u="${WAN}"><NewRemoteHost></NewRemoteHost><NewExternalPort>0</NewExternalPort>` +
    `<NewProtocol>TCP</NewProtocol><NewInternalPort>${port}</NewInternalPort><NewInternalClient>${client}</NewInternalClient>` +
    `<NewEnabled>1</NewEnabled><NewPortMappingDescription>IPCamera-${challenge}</NewPortMappingDescription><NewLeaseDuration>60</NewLeaseDuration>` +
    '</u:AddAnyPortMapping></s:Body></s:Envelope>';
}
export const leaseAction = WAN + '#AddAnyPortMapping';
function reservedPort(raw: string): number {
  const env = igdXml(raw);
  if (env.uri !== SOAP11 || env.name !== 'Envelope' || env.children.length !== 1) deny();
  const body = env.one(SOAP11, 'Body');
  const action = body.one(WAN, 'AddAnyPortMappingResponse');
  if (body.children.length !== 1 || action.children.length !== 1) deny();
  const p = action.one('', 'NewReservedPort').value();
  if (String(Number(p)) !== p || !validPort(Number(p))) deny();
  return Number(p);
}

export type ServiceKind = 'HTTP' | 'RTSP';
export class BoundService {
  readonly port: number;
  private alive: () => boolean;
  spent: boolean = false;
  constructor(port: number, alive: () => boolean) { this.port = port; this.alive = alive; }
  eligible(): boolean { return !this.spent && this.alive(); }
}
class LeaseBindings {
  private services: Map<ServiceKind, BoundService> = new Map();
  set(kind: ServiceKind, port: number, authenticated: boolean, alive: () => boolean): void {
    this.services.delete(kind);
    if (authenticated && validPort(port)) this.services.set(kind, new BoundService(port, alive));
  }
  stop(kind: ServiceKind): void { this.services.delete(kind); }
  stopAll(): void { this.services.clear(); }
  current(kind: ServiceKind): BoundService | undefined { return this.services.get(kind); }
}
export const leaseBindings = new LeaseBindings();
export interface LeaseRoute { netId: number; iface: string; local: string; gateway: string; prefix: number; }
export interface LeaseTransport {
  route(host: string): LeaseRoute;
  exchange(route: LeaseRoute, ep: IgdEndpoint, pin: string, path: string, body: string, check: () => void, beforeSend: () => void): Promise<string>;
}
export function sameRoute(a: LeaseRoute, b: LeaseRoute): boolean {
  return a.netId === b.netId && a.iface === b.iface && a.local === b.local && a.gateway === b.gateway && a.prefix === b.prefix;
}
// Supplied only by the signed application bundle, never by remote XML, preferences, a Want or the form.
export interface ManagedIgdProfile { description: string; control: string; leafSha256: string; }
export function managedProfile(text: string): ManagedIgdProfile {
  if (text.length > 2048) deny();
  const p = JSON.parse(text) as ManagedIgdProfile;
  if (!p || typeof p.description !== 'string' || typeof p.control !== 'string' || typeof p.leafSha256 !== 'string' ||
    p.leafSha256.length !== 64 || !/^[0-9a-f]{64}$/.test(p.leafSha256)) deny();
  const d = igdEndpoint(p.description), c = igdEndpoint(p.control);
  if (d.host !== c.host || d.port !== c.port || d.path === c.path) deny();
  return p;
}
// Local draft. Neither construction, reviewing, cancellation nor listener events open a socket.
export class LeaseRequest {
  readonly endpoint: IgdEndpoint;
  readonly kind: ServiceKind;
  readonly service: BoundService;
  readonly route: LeaseRoute;
  private wire: LeaseTransport;
  private pin: string;
  private challenge: string;
  private phase: number = 0;
  attempted: boolean = false;
  private expectedControl: string;
  constructor(kind: ServiceKind, url: string, profile: ManagedIgdProfile, challenge: string, wire: LeaseTransport) {
    if (url !== profile.description) deny();
    const pin = profile.leafSha256;
    this.endpoint = igdEndpoint(url); this.kind = kind;
    const ctrl = igdEndpoint(profile.control);
    if (ctrl.host !== this.endpoint.host || ctrl.port !== this.endpoint.port) deny();
    this.expectedControl = ctrl.path;
    const service = leaseBindings.current(kind);
    if (!service || !service.eligible() || !/^[a-f0-9]{64}$/.test(pin) || pin.length !== 64 ||
      !/^[a-f0-9]{32}$/.test(challenge) || challenge.length !== 32) deny();
    this.service = service; this.pin = pin; this.challenge = challenge; this.wire = wire;
    this.route = wire.route(this.endpoint.host);
  }
  cancel(): void { this.phase = -1; }
  private check(phase: number): void {
    if (this.phase !== phase || leaseBindings.current(this.kind) !== this.service || !this.service.eligible() ||
      !sameRoute(this.route, this.wire.route(this.endpoint.host))) deny();
  }
  firstConfirmation(): void { this.check(0); this.phase = 1; }
  async finalConfirmation(): Promise<number> {
    this.check(1); this.phase = 2;
    try {
      const desc = await this.wire.exchange(this.route, this.endpoint, this.pin, this.endpoint.path, '', () => this.check(2), () => this.check(2));
      this.check(2);
      const path = controlPath(desc, this.endpoint, this.challenge);
      if (path !== this.expectedControl) deny();
      const reply = await this.wire.exchange(this.route, this.endpoint, this.pin, path, leaseBody(this.route.local, this.service.port, this.challenge), () => this.check(2), () => {
        this.check(2); this.service.spent = true; this.attempted = true; // consume before the only possible POST write
      });
      if (!this.attempted) deny();
      return reservedPort(reply);
    } finally { this.cancel(); }
  }
}
