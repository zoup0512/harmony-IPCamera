export interface RtmpTlsEndpoint {
  authority: string;
  host: string;
  port: number;
  family: number;
  dns: boolean;
  generalName: Uint8Array;
}

function invalid(): never { throw new Error('invalid RTMPS endpoint'); }
function ipv4(host: string): number[] {
  const fields = host.split('.');
  if (fields.length !== 4) return invalid();
  return fields.map((s: string): number => {
    if (!/^(0|[1-9][0-9]{0,2})$/.test(s) || Number(s) > 255) return invalid();
    return Number(s);
  });
}
function ipv6(host: string): number[] {
  let value = host;
  if (value.includes('.')) {
    const sep = value.lastIndexOf(':');
    if (sep < 0) return invalid();
    const octets = ipv4(value.slice(sep + 1));
    value = value.slice(0, sep + 1) + ((octets[0] << 8) + octets[1]).toString(16) + ':' +
      ((octets[2] << 8) + octets[3]).toString(16);
  }
  const sides = value.split('::');
  if (sides.length > 2) return invalid();
  const left: string[] = sides[0] ? sides[0].split(':') : [];
  const right: string[] = sides.length > 1 && sides[1] ? sides[1].split(':') : [];
  const gap = 8 - left.length - right.length;
  if (sides.length === 1 ? gap !== 0 : gap < 1) return invalid();
  const groups = left.slice();
  for (let i = 0; i < gap; i++) groups.push('0');
  const bytes: number[] = [];
  for (const s of groups.concat(right)) {
    if (!/^[0-9a-fA-F]{1,4}$/.test(s)) return invalid();
    const v = parseInt(s, 16); bytes.push(v >> 8, v & 255);
  }
  return bytes;
}

// Build DER GeneralName for one exact DNS-ID or binary IP-ID. No CN fallback,
// certificate text parsing, implicit IDN conversion, or wildcard expansion.
export function rtmpTlsEndpoint(url: string): RtmpTlsEndpoint {
  if (url.length > 65535 || /[\x00-\x20\x7f#\\]/.test(url)) return invalid();
  const m = /^rtmps:\/\/(\[[0-9a-fA-F:.]+\]|[a-zA-Z0-9.-]+)(?::([0-9]{1,5}))?\/([^/?]+)\/(?![/?])(.+)$/.exec(url);
  if (!m) return invalid();
  const authority = url.slice(8, url.indexOf('/', 8));
  const bracket = m[1][0] === '[';
  const host = bracket ? m[1].slice(1, -1) : m[1].toLowerCase();
  const port = m[2] ? Number(m[2]) : 443;
  if (port < 1 || port > 65535) return invalid();
  const numeric = /^[0-9.]+$/.test(host);
  const dns = !bracket && !numeric;
  let bytes: number[] = [];
  if (bracket) bytes = ipv6(host);
  else if (numeric) bytes = ipv4(host);
  else {
    if (host.length > 253 || host.split('.').some((s: string) => !/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(s)) ||
      /^(?:0x[\da-f]+|[0-9]+)(?:\.(?:0x[\da-f]+|[0-9]+))*$/i.test(host)) return invalid();
    for (let i = 0; i < host.length; i++) bytes.push(host.charCodeAt(i));
  }
  const tag = dns ? 0x82 : 0x87;
  const header = bytes.length < 128 ? [tag, bytes.length] : [tag, 0x81, bytes.length];
  return { authority, host, port, family: bracket ? 2 : 1, dns, generalName: new Uint8Array(header.concat(bytes)) };
}
