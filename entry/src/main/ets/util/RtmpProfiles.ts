import { rtmpTlsEndpoint } from './RtmpTlsIdentity';

// Keep `url` readable by the original address list. keyStart is a boundary in
// that same string, not another copy of the credential. Never infer it for old URLs.
export interface RtmpProfile {
  url: string;
  name?: string;
  keyStart?: number;
}

export interface RtmpConfig {
  list: RtmpProfile[];
  current: string;
}

export function readProfiles(raw: string): RtmpProfile[] {
  const list = JSON.parse(raw) as RtmpProfile[];
  if (!Array.isArray(list)) throw new Error('地址列表格式错误');
  for (const p of list) {
    if (!p || typeof p.url !== 'string' || (p.name !== undefined && typeof p.name !== 'string') ||
      (p.keyStart !== undefined && (!Number.isInteger(p.keyStart) || p.keyStart <= 0 ||
        p.keyStart >= p.url.length || p.url.charAt(p.keyStart - 1) !== '/'))) {
      throw new Error('地址列表格式错误');
    }
  }
  return list;
}

// Validate bracketed IPv6 (including an optional final dotted IPv4 address).
// Preserve the original authority and opaque stream key; no URL reserialization.
function validIpv6(host: string): boolean {
  let s = host;
  if (s.includes('.')) {
    const last = s.lastIndexOf(':');
    const v4 = s.slice(last + 1).split('.');
    if (last < 0 || v4.length !== 4 || v4.some((p: string) => !/^(0|[1-9][0-9]{0,2})$/.test(p) || Number(p) > 255)) return false;
    s = s.slice(0, last + 1) + '0:0';
  }
  const halves = s.split('::');
  if (halves.length > 2) return false;
  let count = 0;
  for (const half of halves) {
    if (!half) continue;
    const words = half.split(':');
    if (words.some((p: string) => !/^[a-f0-9]{1,4}$/i.test(p))) return false;
    count += words.length;
  }
  return halves.length === 1 ? count === 8 : count < 8;
}

export function canPublish(url: string): boolean {
  // TLS identity validation owns the entire secure URL grammar, including
  // hostname/IP forms, default port 443 and rejection of userinfo. No fallback.
  if (url.startsWith('rtmps://')) {
    try { rtmpTlsEndpoint(url); return true; } catch (_) { return false; }
  }
  if (url.length > 65535 || /[\s\\#\x00-\x1f\x7f]/.test(url)) return false;
  const match = /^rtmp:\/\/(\[[A-Fa-f0-9:.]+\]|[A-Za-z0-9.-]+)(?::([0-9]+))?\/[^/?]+\/[^/?][^?]*(?:\?.*)?$/.exec(url);
  if (!match || (match[1].startsWith('[') && !validIpv6(match[1].slice(1, -1)))) return false;
  return !match[2] || (Number(match[2]) > 0 && Number(match[2]) <= 65535);
}

export function endpointLabel(url: string, hidden: string = '（地址已隐藏）'): string {
  if (!canPublish(url)) return hidden;
  return url.slice(0, url.indexOf('/', url.indexOf('://') + 3)) + '/…';
}

export function makeProfile(name: string, address: string, key: string): RtmpProfile {
  const base = address.trim();
  const profile: RtmpProfile = { name: name.trim(), url: base };
  if (!profile.name) throw new Error('name_required');
  if (key !== '') {
    if (base.includes('?')) throw new Error('full_url');
    if (base.endsWith('//') || key.startsWith('//')) throw new Error('slash');
    const prefix = base.endsWith('/') ? base : base + '/';
    const stream = key.startsWith('/') ? key.slice(1) : key;
    if (!stream) throw new Error('key_path');
    profile.url = prefix + stream;
    profile.keyStart = prefix.length;
  }
  if (!canPublish(profile.url)) throw new Error('format');
  return profile;
}

export function editAddress(p: RtmpProfile): string {
  return p.keyStart === undefined ? p.url : p.url.slice(0, p.keyStart - 1);
}

export function editKey(p: RtmpProfile): string {
  return p.keyStart === undefined ? '' : p.url.slice(p.keyStart);
}

// A new entry does not select itself. Only the explicitly selected destination
// follows edits/deletion; an unrelated URL set directly on Index is untouched.
export function saveProfile(state: RtmpConfig, index: number, profile: RtmpProfile): RtmpConfig {
  if (state.list.some((p: RtmpProfile, i: number) => i !== index && p.url === profile.url)) {
    throw new Error('duplicate');
  }
  if (index === -1) return { list: [profile, ...state.list], current: state.current };
  const list = state.list.slice();
  const selected = list[index].url === state.current;
  list[index] = profile;
  return { list: list, current: selected ? profile.url : state.current };
}

export function selectProfile(state: RtmpConfig, index: number): RtmpConfig {
  const url = state.list[index].url;
  if (!canPublish(url)) throw new Error('旧地址不符合当前推流格式，请先编辑');
  return { list: state.list, current: url };
}

export function deleteProfile(state: RtmpConfig, index: number): RtmpConfig {
  const list = state.list.slice();
  const removed = list.splice(index, 1)[0];
  return { list: list, current: removed.url === state.current ? '' : state.current };
}
