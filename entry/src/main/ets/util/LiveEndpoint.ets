import { url, util } from '@kit.ArkTS';

export interface LiveEndpoint {
  scheme: string;
  address: string;
  origin: string;
  token: string;
  digest?: DigestLogin;
}

interface DigestLogin { user: string; password: string }

// Never put userinfo into a network URL, log, label, clipboard or navigation target.
// Keep the original stream path/query encoding; cameras can use signed URLs.
export function liveEndpoint(input: string, user: string = '', password: string = '', digest: boolean = false): LiveEndpoint {
  const raw = input.trim();
  if (raw.length > 4096 || /[\s\\#\x00-\x1f\x7f]/.test(raw)) throw new Error('invalid address');
  const m = /^(https?|rtsps?|rtmp):\/\/([^/?#]+)([^#]*)$/i.exec(raw);
  if (!m) throw new Error('invalid scheme');
  const scheme = m[1].toLowerCase();
  const authority = m[2];
  const at = authority.lastIndexOf('@');
  const host = authority.slice(at + 1);
  if (!host || host.includes('%') || authority.indexOf('@') !== at) throw new Error('invalid host');
  const origin = `${scheme}://${host}`;
  const parsed = url.URL.parseURL(origin);
  if (!parsed.hostname || !/^(?:\[[a-f0-9:.]+\]|[a-z0-9.-]+)(?::[0-9]+)?$/i.test(host)) throw new Error('invalid host');
  if (scheme === 'rtmp') {
    // Match the native viewer's ASCII /app/stream[?query] contract; never strip auth and retry.
    if (at >= 0 || user || password || /[^\x21-\x7e]/.test(raw) || parsed.port === '0' ||
      parsed.hostname.length > 253 || !/^\/[^/?]+\/[^/?]/.test(m[3])) throw new Error('invalid RTMP address');
  }
  if (at >= 0 && !user && !password) {
    const info = authority.slice(0, at);
    const colon = info.indexOf(':');
    user = decodeURIComponent(colon < 0 ? info : info.slice(0, colon));
    password = colon < 0 ? '' : decodeURIComponent(info.slice(colon + 1));
  }
  if (user.includes(':') || /[\x00-\x1f\x7f]/.test(user + password)) throw new Error('invalid credentials');
  const suffix = m[3];
  const dest: LiveEndpoint = { scheme: scheme, origin: origin, address: origin + (suffix.startsWith('/') ? suffix : '/' + suffix), token: '' };
  if (digest) {
    if (scheme !== 'rtsp' || !user || !password) throw new Error('invalid Digest credentials');
    dest.digest = { user: user, password: password };
  } else if (user || password) {
    dest.token = new util.Base64Helper().encodeToStringSync(util.TextEncoder.create().encodeInto(user + ':' + password));
  }
  return dest;
}
