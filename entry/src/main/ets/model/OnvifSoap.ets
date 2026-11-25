import { convertxml } from '@kit.ArkTS';

export const DEVICE = 'http://www.onvif.org/ver10/device/wsdl';
export const MEDIA = 'http://www.onvif.org/ver10/media/wsdl';
export const TT = 'http://www.onvif.org/ver10/schema';
export const SOAP = 'http://www.w3.org/2003/05/soap-envelope';

export function esc(s: string): string {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&apos;');
}

interface XmlItem {
  type: string;
  name?: string;
  text?: string;
  cdata?: string;
  attributes?: Record<string, string>;
  elements?: XmlItem[];
}
interface XmlDoc { elements: XmlItem[]; }
const KEY = /^[A-Za-z_][A-Za-z0-9_.-]*(?::[A-Za-z_][A-Za-z0-9_.-]*)?$/;
function malformed(): Error { return new Error('不支持的 SOAP 响应或编码配置'); }

// The SDK's XML-to-object API doesn't promise to return namespace declarations.
// Pass QName spellings/declarations as ordinary XML names so serialization does
// not depend on whether a particular runtime reports local or qualified names.
// Only markup names are adapted: quoted values, comments, CDATA, PIs and text
// remain untouched. Structure, escaping and entity decoding are handled by the SDK.
function protectNames(raw: string): string {
  const open: string[] = [];
  let count = 0;
  const safe = raw.replace(/<!\[CDATA\[[\s\S]*?\]\]>|<!--[\s\S]*?-->|<\?[\s\S]*?\?>|<\/?[A-Za-z_](?:[^<>"']|"[^"]*"|'[^']*')*>/g,
    (tag: string): string => {
      if (tag.startsWith('<!') || tag.startsWith('<?')) return tag;
      const match = /^<\/?([A-Za-z_][A-Za-z0-9_.:-]*)/.exec(tag);
      if (!match) throw malformed();
      if (tag.startsWith('</')) {
        if (open.pop() !== match[1]) throw malformed();
      } else {
        if (++count > 8192 || open.length >= 48) throw malformed();
        if (!tag.endsWith('/>')) open.push(match[1]);
      }
      return tag.replace(/"[^"]*"|'[^']*'|[A-Za-z_][A-Za-z0-9_.:-]*/g, (name: string): string => {
        if (name.startsWith('"') || name.startsWith("'")) return name;
        if (!KEY.test(name)) throw malformed();
        return 'on_' + name.replace(/_/g, '__').replace(/:/g, '_c');
      });
    });
  if (open.length) throw malformed();
  return safe;
}
function restored(name: string): string {
  if (!name.startsWith('on_')) throw malformed();
  const key = name.slice(3).replace(/__|_c/g, (s: string): string => s === '__' ? '_' : ':');
  if (!KEY.test(key) || key === '__proto__' || key === 'constructor' || key === 'prototype') throw malformed();
  return key;
}

// Keep the original tree and namespace bindings: Set takes a whole Configuration,
// not a patch. Never reconstruct it from just the fields shown in the editor.
export class SoapElement {
  readonly qname: string;
  readonly name: string;
  readonly uri: string;
  readonly attrs: Record<string, string>;
  readonly ns: Map<string, string>;
  readonly children: SoapElement[] = [];
  text: string = '';

  constructor(raw: XmlItem, inherited: Map<string, string>, depth: number) {
    if (depth > 48 || raw.type !== 'element' || !raw.name || !KEY.test(raw.name)) throw malformed();
    this.qname = restored(raw.name);
    this.attrs = {};
    this.ns = new Map(inherited);
    const attrs: Record<string, string> = raw.attributes ?? {};
    const keys = Object.keys(attrs);
    if (keys.length > 128) throw malformed();
    for (const safe of keys) {
      const k = restored(safe);
      if (typeof attrs[safe] !== 'string') throw malformed();
      this.attrs[k] = attrs[safe];
      if (k === 'xmlns') this.ns.set('', attrs[safe]);
      else if (k.startsWith('xmlns:')) this.ns.set(k.substring(6), attrs[safe]);
    }
    for (const k of Object.keys(this.attrs)) {
      const prefix = k.split(':')[0];
      if (k.includes(':') && prefix !== 'xmlns' && prefix !== 'xml' && !this.ns.get(prefix)) throw malformed();
    }
    const parts = this.qname.split(':');
    this.name = parts[parts.length - 1];
    this.uri = this.ns.get(parts.length === 2 ? parts[0] : '') ?? '';
    if (parts.length === 2 && !this.uri) throw malformed();
    for (const item of raw.elements ?? []) {
      if (item.type === 'element') this.children.push(new SoapElement(item, this.ns, depth + 1));
      else if (item.type === 'text') this.text += item.text ?? '';
      else if (item.type === 'cdata') this.text += item.cdata ?? '';
      else throw malformed();
    }
    // Mixed content cannot be round-tripped by this editor; fail closed.
    if (this.children.length > 0 && this.text.trim() !== '') throw malformed();
  }

  all(uri: string, name: string): SoapElement[] { return this.children.filter(x => x.uri === uri && x.name === name); }
  optional(uri: string, name: string): SoapElement | undefined {
    const matches = this.all(uri, name);
    if (matches.length > 1) throw malformed();
    return matches[0];
  }
  one(uri: string, name: string): SoapElement {
    const found = this.optional(uri, name);
    if (!found) throw malformed();
    return found;
  }
  value(): string {
    if (this.children.length || !this.text.trim()) throw malformed();
    return this.text.trim();
  }
  serialize(standalone: boolean = false): string {
    let attrs = '';
    for (const key of Object.keys(this.attrs)) attrs += ` ${key}="${esc(this.attrs[key])}"`;
    if (standalone) {
      if (!this.ns.has('')) attrs += ' xmlns=""'; // Do not inherit Set's default namespace.
      this.ns.forEach((value: string, prefix: string) => {
        const key = prefix ? `xmlns:${prefix}` : 'xmlns';
        if (!Object.keys(this.attrs).includes(key)) attrs += ` ${key}="${esc(value)}"`;
      });
    }
    const inner = this.children.length ? this.children.map(x => x.serialize()).join('') : esc(this.text);
    return `<${this.qname}${attrs}>${inner}</${this.qname}>`;
  }
}

export function response(raw: string, uri: string, action: string): SoapElement {
  // No DTD, entity declarations, XInclude processing or secondary fetches.
  if (raw.length > 524288 || /[\x00-\x08\x0b\x0c\x0e-\x1f]|<!\s*(?:DOCTYPE|ENTITY)/i.test(raw)) throw malformed();
  // Only UTF-8 XML, never reinterpret the response as another encoding in the native parser.
  if (/^\s*<\?xml\s[^?]*encoding\s*=/i.test(raw) && !/^\s*<\?xml\s[^?]*encoding\s*=\s*["']utf-8["']/i.test(raw)) throw malformed();
  const parsed = new convertxml.ConvertXML().fastConvertToJSObject(protectNames(raw), {
    trim: false, ignoreDeclaration: true, ignoreInstruction: true, ignoreComment: true,
    attributesKey: 'attributes', elementsKey: 'elements', nameKey: 'name', typeKey: 'type',
    textKey: 'text', cdataKey: 'cdata', declarationKey: 'declaration', instructionKey: 'instruction',
    doctypeKey: 'doctype', commentKey: 'comment', parentKey: 'parent'
  }) as XmlDoc;
  if (!parsed.elements || parsed.elements.length !== 1) throw malformed();
  const envelope = new SoapElement(parsed.elements[0], new Map<string, string>(), 0);
  if (envelope.name !== 'Envelope' || envelope.uri !== SOAP) throw malformed();
  const body = envelope.one(SOAP, 'Body');
  if (body.children.length !== 1) throw malformed();
  if (body.optional(SOAP, 'Fault')) throw new Error('设备返回 SOAP Fault；请检查账号、权限与设备时钟');
  return body.one(uri, action + 'Response');
}

export interface Endpoint { host: string; origin: string; }
export function endpoint(value: string, stream: boolean = false): Endpoint {
  // Strict canonical local IPv4 literals only; never perform DNS, parse userinfo,
  // accept platform-specific numeric host spellings, or follow a service to another origin.
  const m = /^(https?|rtsps?):\/\/(\d+\.\d+\.\d+\.\d+)(?::([1-9]\d{0,4}))?(\/[^\s\\#]*)?$/.exec(value);
  if (!m || value.length > 2048 || /[\x00-\x20\x7f]/.test(value) || (!stream && value.includes('?'))) throw new Error('不支持的设备地址');
  const ip = m[2].split('.').map(s => Number(s));
  if (ip.join('.') !== m[2] || ip.some(n => n > 255) || !(ip[0] === 10 || ip[0] === 127 ||
    (ip[0] === 192 && ip[1] === 168) || (ip[0] === 172 && ip[1] >= 16 && ip[1] <= 31))) throw new Error('需要明确的私网 IPv4 地址');
  if (stream ? !m[1].startsWith('rtsp') : !(m[1] === 'https' || (m[1] === 'http' && ip[0] === 127))) {
    throw new Error('仅支持可信 HTTPS；明文 HTTP 仅限回环');
  }
  const def = m[1] === 'https' ? 443 : (m[1] === 'http' ? 80 : (m[1] === 'rtsp' ? 554 : 322));
  const port = m[3] ? Number(m[3]) : def;
  if (port > 65535) throw new Error('无效端口');
  return { host: m[2], origin: `${m[1]}://${m[2]}${port === def ? '' : ':' + port}` };
}
