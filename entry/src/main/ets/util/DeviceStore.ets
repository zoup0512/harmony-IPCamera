import { asset } from '@kit.AssetStoreKit';
import { url, util } from '@kit.ArkTS';
import { BusinessError } from '@kit.BasicServicesKit';
import { liveEndpoint } from './LiveEndpoint';

export interface DeviceDraft {
  name: string;
  url: string;
}

export interface DeviceItem {
  id: string;
  name: string;
  endpoint: string;
}

interface AndroidQrDevice {
  title: string;
  url: string;
  user: string;
  password: string;
}

const GROUP = 'ipcam.devices.v1';
const PAGE_SIZE = 32;

function bytes(text: string): Uint8Array {
  return util.TextEncoder.create().encodeInto(text);
}

function text(data: Uint8Array): string {
  return util.TextDecoder.create('utf-8').decodeToString(data);
}

// Only scheme and host are safe to display: credentials can live in the path or query too.
export function publicEndpoint(value: string): string {
  if (/^rtmp:/i.test(value)) {
    try { return value === value.trim() ? liveEndpoint(value).origin : ''; } catch (_) { return ''; }
  }
  if (!new RegExp('^(https?|rtsps?)://[^/?#]', 'i').test(value) ||
    new RegExp('[\\s\\\\\\x00-\\x1f\\x7f]').test(value)) return '';
  try {
    const parsed = url.URL.parseURL(value);
    return parsed.hostname ? `${parsed.protocol.toLowerCase()}//${parsed.host}` : '';
  } catch (_) {
    return '';
  }
}

// Accept the Android camera export as well as a plain stream URL. Keep the original path,
// query and escaping byte-for-byte (in particular /video?pw=); never serialize a parsed URL.
export function readDeviceQr(raw: string): DeviceDraft {
  const value = raw.trim();
  if (publicEndpoint(value)) return { name: '', url: value };
  const item = JSON.parse(value) as AndroidQrDevice;
  if (!item || typeof item.url !== 'string' || typeof item.title !== 'string' ||
    typeof item.user !== 'string' || typeof item.password !== 'string') throw new Error('invalid QR');
  let address = item.url.trim();
  if (!publicEndpoint(address) || (/^rtmp:/i.test(address) && (item.user || item.password))) throw new Error('invalid URL');
  const start = address.indexOf('://') + 3;
  const authority = address.substring(start).split(new RegExp('[/?#]'))[0];
  if ((item.user || item.password) && !authority.includes('@')) {
    const auth = `${encodeURIComponent(item.user)}:${encodeURIComponent(item.password)}@`;
    address = address.slice(0, start) + auth + address.slice(start);
  }
  return { name: item.title.trim(), url: address };
}

function selector(id: string = ''): asset.AssetMap {
  const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
  query.set(asset.Tag.DATA_LABEL_CRITICAL_1, bytes(GROUP));
  if (id !== '') query.set(asset.Tag.ALIAS, bytes(id));
  return query;
}

export async function readDevice(id: string): Promise<DeviceDraft> {
  if (!id) throw new Error('missing device');
  const query = selector(id);
  query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
  const rows = await asset.query(query);
  const draft: DeviceDraft = JSON.parse(text(rows[0].get(asset.Tag.SECRET) as Uint8Array)) as DeviceDraft;
  if (typeof draft.name !== 'string' || typeof draft.url !== 'string' || !publicEndpoint(draft.url)) {
    throw new Error('invalid device');
  }
  return draft;
}

export async function listDevices(): Promise<DeviceItem[]> {
  const result: DeviceItem[] = [];
  const query = selector();
  query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ATTRIBUTES);
  query.set(asset.Tag.RETURN_LIMIT, PAGE_SIZE);
  let offset = 0;
  while (true) {
    query.set(asset.Tag.RETURN_OFFSET, offset);
    let rows: asset.AssetMap[];
    try {
      rows = await asset.query(query);
    } catch (err) {
      if ((err as BusinessError).code === asset.ErrorCode.NOT_FOUND) break;
      throw err as Error;
    }
    for (const row of rows) {
      const id = text(row.get(asset.Tag.ALIAS) as Uint8Array);
      const draft = await readDevice(id);
      const endpoint = publicEndpoint(draft.url);
      result.push({ id: id, name: draft.name || endpoint, endpoint: endpoint });
    }
    if (rows.length < PAGE_SIZE) break;
    offset += rows.length;
  }
  return result.reverse();
}

export function fitsDeviceStore(draft: DeviceDraft): boolean {
  // ASSET's SECRET is limited to 1024 bytes, not 1024 UTF-16 characters.
  return bytes(JSON.stringify(draft)).length <= 1024;
}

export async function saveDevice(id: string, name: string, address: string): Promise<void> {
  const draft: DeviceDraft = { name: name.trim(), url: address.trim() };
  if (!publicEndpoint(draft.url) || !fitsDeviceStore(draft)) throw new Error('invalid device');
  const secret = bytes(JSON.stringify(draft));
  if (id !== '') {
    const change: asset.AssetMap = new Map<asset.Tag, asset.Value>();
    change.set(asset.Tag.SECRET, secret);
    await asset.update(selector(id), change);
    return;
  }
  const record = selector(util.generateRandomUUID());
  record.set(asset.Tag.SECRET, secret);
  record.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_UNLOCKED);
  record.set(asset.Tag.SYNC_TYPE, asset.SyncType.NEVER);
  await asset.add(record);
}

export async function deleteDevice(id: string): Promise<void> {
  if (!id) throw new Error('missing device');
  // remove() rejects DATA_LABEL_CRITICAL_1 on some runtimes. Verify group ownership
  // with an attributes-only query, then remove exactly one app-scoped alias.
  const query = selector(id);
  query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ATTRIBUTES);
  const rows = await asset.query(query);
  if (rows.length !== 1 || text(rows[0].get(asset.Tag.ALIAS) as Uint8Array) !== id) {
    throw new Error('device not found');
  }
  const key: asset.AssetMap = new Map<asset.Tag, asset.Value>();
  key.set(asset.Tag.ALIAS, bytes(id));
  await asset.remove(key);
}
