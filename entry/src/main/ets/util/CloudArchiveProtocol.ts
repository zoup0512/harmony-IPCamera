// Platform-independent protocol; the only production transport is in CloudArchiveService.
const LOGIN_HOST = 'https://login.microsoftonline.com/';
const DRIVE = 'https://graph.microsoft.com/v1.0/me/drive';
const SCOPE = 'Files.ReadWrite.AppFolder';
export const CHUNK = 5 * 320 * 1024; // Graph requires a multiple of 320 KiB except for the final PUT.
export interface Reply { status: number; text: string }
export interface Transport {
  send(method: string, url: string, headers: Record<string, string>, body?: string | ArrayBuffer): Promise<Reply>;
}
export interface Clock { now(): number; sleep(ms: number): Promise<void> }
export interface Login { client: string; token: string; expires: number }
export interface Challenge { uri: string; code: string }
export interface Source {
  name: string; size: number;
  check(): void;
  read(offset: number, length: number): Promise<ArrayBuffer>;
}
interface DeviceReply { verification_uri: string; user_code: string; device_code: string; expires_in: number; interval?: number }
interface OAuthReply { error?: string; token_type?: string; access_token?: string; scope?: string; expires_in?: number }
interface Parent { id?: string; driveId?: string }
interface FileFacet { hashes?: FileHashes }
interface FileHashes { sha256Hash?: string }
interface Item { id: string; name: string; size: number; file?: FileFacet; folder?: Object; eTag?: string;
  parentReference?: Parent; remoteItem?: Object; deleted?: Object; package?: Object }
export interface ContentReceipt { folder: string; drive: string; item: string; name: string; size: number; tag: string; sha: string }
// Select these facets explicitly, including the ones whose presence disqualifies an item.
const CONTENT = '?$select=id,name,size,file,folder,eTag,parentReference,remoteItem,deleted,package';
const ROOT = DRIVE + '/special/approot?$select=id,folder,parentReference,remoteItem,deleted';
function localItem(item: Item): boolean { return !item.remoteItem && !item.deleted && !item.package; }
function strong(item: Item, folder: Item): string {
  const sha = item.file?.hashes?.sha256Hash;
  return localItem(item) && !item.folder && localItem(folder) && !!folder.folder &&
    itemId(folder.parentReference?.driveId ?? '') && item.parentReference?.driveId === folder.parentReference?.driveId &&
    item.parentReference?.id === folder.id && typeof sha === 'string' && sha.length === 64 && !/[^0-9a-f]/i.test(sha) ? sha.toLowerCase() : '';
}
// Fresh metadata only, on the fixed Graph origin, for the same token, AppFolder, drive and item.
export async function verifyContent(net: Transport, login: Login, clock: Clock, r: ContentReceipt, check: () => void): Promise<void> {
  const guard = (): void => { check(); if (!usable(login, clock.now())) throw new Error('expired'); };
  guard();
  if (!itemId(r.item) || !itemId(r.folder) || !itemId(r.drive) || !recordingName(r.name) || !r.sha) throw new Error('proof');
  const auth: Record<string, string> = { 'Authorization': 'Bearer ' + login.token, 'Cache-Control': 'no-cache' };
  const root = await net.send('GET', ROOT, auth); guard();
  if (root.status !== 200) throw new Error('proof');
  const folder = JSON.parse(root.text) as Item;
  if (folder.id !== r.folder || folder.parentReference?.driveId !== r.drive) throw new Error('proof');
  const reply = await net.send('GET', DRIVE + '/items/' + encodeURIComponent(r.item) + CONTENT, auth); guard();
  if (reply.status !== 200) throw new Error('proof');
  const item = JSON.parse(reply.text) as Item;
  if (item.id !== r.item || item.name !== r.name || item.size !== r.size || item.eTag !== r.tag || strong(item, folder) !== r.sha) throw new Error('proof');
}
interface UploadSession { uploadUrl: string }
interface Accepted { nextExpectedRanges: string[] }

export function validClient(value: string): boolean {
  return /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(value) &&
    value !== '00000000-0000-0000-0000-000000000000';
}
export function recordingName(name: string): boolean {
  return /^(ipc_\d{6}|(ipc_|IPS_)\d{4}-\d{2}-\d{2}\.\d{2}\.\d{2}\.\d{2}\.\d{4})(_[1-9]\d*)?\.mp4$/.test(name) && !/\s/.test(name);
}
function seconds(value: number | undefined, max: number): boolean {
  return typeof value === 'number' && Number.isInteger(value) && value > 0 && value <= max;
}
function validToken(value: string | undefined): boolean {
  return typeof value === 'string' && value.length > 0 && value.length <= 32768 && /^[A-Za-z0-9._~+/=-]+$/.test(value) && !/\s/.test(value);
}
export function usable(login: Login, now: number): boolean {
  return validClient(login.client) && validToken(login.token) && Number.isSafeInteger(login.expires) && now < login.expires;
}
export async function deviceLogin(net: Transport, clock: Clock, client: string, check: () => void,
  show: (challenge: Challenge) => void, tenant: string = 'common'): Promise<Login> {
  if (!validClient(client) || (tenant !== 'common' && !validClient(tenant))) throw new Error('client');
  const authority = LOGIN_HOST + tenant + '/oauth2/v2.0';
  check();
  const form: Record<string, string> = { 'Content-Type': 'application/x-www-form-urlencoded' };
  const start = clock.now();
  const r = await net.send('POST', authority + '/devicecode', form,
    `client_id=${client}&scope=${encodeURIComponent('https://graph.microsoft.com/' + SCOPE)}`);
  check();
  if (r.status !== 200) throw new Error('registration');
  const d = JSON.parse(r.text) as DeviceReply;
  if (!['https://microsoft.com/devicelogin', 'https://www.microsoft.com/devicelogin', 'https://www.microsoft.com/link'].includes(d.verification_uri) ||
    typeof d.user_code !== 'string' || !/^[A-Z0-9-]{4,24}$/.test(d.user_code) ||
    typeof d.device_code !== 'string' || !d.device_code || d.device_code.length > 16384 || !seconds(d.expires_in, 3600) ||
    (d.interval !== undefined && !seconds(d.interval, 300))) throw new Error('challenge');
  let delay = (d.interval === undefined ? 5 : d.interval) * 1000;
  const deadline = start + d.expires_in * 1000;
  show({ uri: d.verification_uri, code: d.user_code });
  while (clock.now() < deadline) {
    await clock.sleep(Math.min(delay, deadline - clock.now()));
    check();
    if (clock.now() >= deadline) break;
    const polled = clock.now();
    const reply = await net.send('POST', authority + '/token', form,
      `grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&client_id=${client}&device_code=${encodeURIComponent(d.device_code)}`);
    check();
    const t = JSON.parse(reply.text) as OAuthReply;
    if (reply.status === 400 && t.error === 'authorization_pending') continue;
    if (reply.status === 400 && t.error === 'slow_down') { delay += 5000; continue; }
    if (reply.status !== 200 || t.token_type?.toLowerCase() !== 'bearer' || !validToken(t.access_token) ||
      !seconds(t.expires_in, 86400) || typeof t.scope !== 'string' || !t.scope.split(' ').some((s: string) =>
        s === SCOPE || s === 'https://graph.microsoft.com/' + SCOPE)) throw new Error('authorization');
    return { client: client, token: t.access_token!, expires: polled + t.expires_in! * 1000 };
  }
  throw new Error('expired');
}
function itemId(value: string): boolean {
  return typeof value === 'string' && value.length > 0 && value.length <= 512 && !/[^A-Za-z0-9!._-]/.test(value) && value !== '.' && value !== '..';
}
function uploadHost(value: string): boolean {
  // Preauthenticated URL: no bearer, redirects, userinfo, ports, or non-Microsoft destinations.
  return typeof value === 'string' && !/[\s\\#]/.test(value) &&
    /^https:\/\/(?:[a-z0-9-]+\.)+(?:1drv\.com|onedrive\.com|sharepoint\.com|storage\.live\.com)\//i.test(value);
}
function matches(item: Item, src: Source): boolean {
  return itemId(item.id) && item.name === src.name && item.size === src.size && !!item.file && typeof item.file === 'object' && !Array.isArray(item.file);
}
export async function archive(net: Transport, login: Login, clock: Clock, src: Source, check: () => void,
  progress: (bytes: number) => void): Promise<ContentReceipt | undefined> {
  const guard = (): void => { check(); src.check(); if (!usable(login, clock.now())) throw new Error('expired'); };
  guard();
  if (!recordingName(src.name) || !Number.isSafeInteger(src.size) || src.size <= 0) throw new Error('source');
  const auth: Record<string, string> = { 'Authorization': 'Bearer ' + login.token };
  const root = await net.send('GET', ROOT, auth);
  guard();
  if (root.status !== 200) throw new Error('approot');
  const folder = JSON.parse(root.text) as Item;
  if (!itemId(folder.id) || !folder.folder) throw new Error('approot');
  const dest = DRIVE + '/items/' + encodeURIComponent(folder.id) + ':/' + encodeURIComponent(src.name);
  const createHeaders: Record<string, string> = { 'Authorization': auth['Authorization'], 'Content-Type': 'application/json' };
  const created = await net.send('POST', dest + ':/createUploadSession', createHeaders,
    '{"item":{"@microsoft.graph.conflictBehavior":"fail"}}');
  guard();
  if (created.status !== 200) throw new Error('session');
  const session = JSON.parse(created.text) as UploadSession;
  if (!uploadHost(session.uploadUrl)) throw new Error('destination');
  let offset = 0;
  while (offset < src.size) {
    const length = Math.min(CHUNK, src.size - offset);
    const bytes = await src.read(offset, length);
    guard();
    if (bytes.byteLength !== length) throw new Error('short read');
    const next = offset + length;
    const range: Record<string, string> = { 'Content-Type': 'application/octet-stream', 'Content-Length': `${length}`,
      'Content-Range': `bytes ${offset}-${next - 1}/${src.size}` };
    const sent = await net.send('PUT', session.uploadUrl, range, bytes);
    guard();
    if (next < src.size) {
      if (sent.status !== 202) throw new Error('range');
      const ack = JSON.parse(sent.text) as Accepted;
      if (!Array.isArray(ack.nextExpectedRanges) || ack.nextExpectedRanges.length !== 1 || ack.nextExpectedRanges[0] !== `${next}-`) throw new Error('range');
    } else {
      if (sent.status !== 200 && sent.status !== 201) throw new Error('commit');
      const item = JSON.parse(sent.text) as Item;
      if (!matches(item, src)) throw new Error('commit');
      // Metadata-only uploads still succeed, but never produce a cleanup receipt.
      const readback = await net.send('GET', dest + CONTENT, auth);
      guard();
      if (readback.status !== 200) throw new Error('readback');
      const remote = JSON.parse(readback.text) as Item;
      if (!matches(remote, src) || remote.id !== item.id) throw new Error('readback');
      const sha = strong(remote, folder), tag = remote.eTag;
      if (sha && typeof tag === 'string' && tag.length > 0 && tag.length <= 512 && tag === item.eTag && localItem(item)) {
        progress(next);
        return { folder: folder.id, drive: folder.parentReference!.driveId!, item: item.id, name: src.name, size: src.size, tag: tag, sha: sha };
      }
    }
    offset = next;
    progress(next);
  }
}

// Walk only top-level ISO BMFF headers (skip media payload). An abandoned/open file without
// finalized movie metadata is not a completed MP4. Deliberately fail closed on unusual layouts.
export async function finalized(src: Source): Promise<boolean> {
  let pos = 0, count = 0, movie = false, media = false, brand = false;
  while (src.size - pos >= 8 && ++count <= 4096) {
    const data = await src.read(pos, 8);
    if (data.byteLength !== 8) return false;
    const h = new DataView(data);
    let size = h.getUint32(0), header = 8;
    const kind = h.getUint32(4);
    if (size === 1) {
      const more = await src.read(pos + 8, 8);
      if (more.byteLength !== 8) return false;
      const ext = new DataView(more);
      size = ext.getUint32(0) * 4294967296 + ext.getUint32(4); header = 16;
    } else if (!size) size = src.size - pos;
    if (!Number.isSafeInteger(size) || size < header || size > src.size - pos) return false;
    if (size > header) {
      if (kind === 0x66747970) brand = true; // ftyp
      if (kind === 0x6d6f6f76) movie = true; // moov
      if (kind === 0x6d646174) media = true; // mdat
    }
    pos += size;
  }
  return pos === src.size && movie && media && brand;
}
