import { asset } from '@kit.AssetStoreKit';
import { util } from '@kit.ArkTS';
import { BusinessError } from '@kit.BasicServicesKit';
import { MailSettings, FtpSettings, checkMail, checkFtp, restoreBinding, checkStoredBinding, fail } from './DeliveryProtocol';

function bytes(s: string): Uint8Array { return util.TextEncoder.create().encodeInto(s); }
function key(mail: boolean): asset.AssetMap {
  const q: asset.AssetMap = new Map<asset.Tag, asset.Value>();
  q.set(asset.Tag.ALIAS, bytes(mail ? 'ipcam.delivery.smtp.v1' : 'ipcam.delivery.ftp.v1'));
  return q;
}
async function read(mail: boolean): Promise<string> {
  const q = key(mail);
  q.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
  try {
    const rows = await asset.query(q);
    return util.TextDecoder.create('utf-8').decodeToString(rows[0].get(asset.Tag.SECRET) as Uint8Array);
  } catch (err) {
    if ((err as BusinessError).code === asset.ErrorCode.NOT_FOUND) return '';
    return fail('无法读取安全存储，请解锁设备后重试');
  }
}
async function put(mail: boolean, text: string): Promise<void> {
  const secret = bytes(text);
  if (secret.length > 1024) fail('配置超过安全存储的 1024 字节上限');
  const q = key(mail);
  q.set(asset.Tag.SECRET, secret);
  q.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_UNLOCKED);
  q.set(asset.Tag.SYNC_TYPE, asset.SyncType.NEVER);
  q.set(asset.Tag.CONFLICT_RESOLUTION, asset.ConflictResolution.OVERWRITE);
  try { await asset.add(q); } catch (_) { fail('无法保存至安全存储，请解锁设备后重试'); }
}
export async function loadMail(): Promise<MailSettings> {
  const text = await read(true);
  const c = text ? JSON.parse(text) as MailSettings : new MailSettings();
  restoreBinding(true, c);
  return c;
}
export async function loadFtp(): Promise<FtpSettings> {
  const text = await read(false);
  const c = text ? JSON.parse(text) as FtpSettings : new FtpSettings();
  restoreBinding(false, c);
  return c;
}
export async function saveMail(c: MailSettings): Promise<void> {
  if (c.enabled) checkMail(c);
  else checkStoredBinding(true, c);
  await put(true, JSON.stringify(c));
  mailRevision++;
}
export async function saveFtp(c: FtpSettings): Promise<void> {
  if (c.enabled) checkFtp(c);
  else { checkStoredBinding(false, c); c.consent = ''; }
  await put(false, JSON.stringify(c));
  ftpRevision++;
}
let mailRevision = 0;
let ftpRevision = 0;
export function revision(mail: boolean): number { return mail ? mailRevision : ftpRevision; }
