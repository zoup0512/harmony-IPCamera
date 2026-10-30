import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';

// Preference keys mirror the Android SharedPreferences names so behavior maps
// 1:1 to the reference app. Values stored in HarmonyOS preferences db.
const STORE = 'ipcam_settings';

export interface RtmpAddr {
  title: string;
  rtmp_server: string;
  rtmp_stream_key: string;
}

export class Prefs {
  private static store: preferences.Preferences | null = null;

  private static db(ctx: common.UIAbilityContext): preferences.Preferences {
    if (Prefs.store === null) {
      Prefs.store = preferences.getPreferencesSync(ctx, { name: STORE });
    }
    return Prefs.store;
  }

  static getStr(ctx: common.UIAbilityContext, key: string, dft: string): string {
    return Prefs.db(ctx).getSync(key, dft) as string;
  }

  static getInt(ctx: common.UIAbilityContext, key: string, dft: number): number {
    return Prefs.db(ctx).getSync(key, dft) as number;
  }

  static getBool(ctx: common.UIAbilityContext, key: string, dft: boolean): boolean {
    return Prefs.db(ctx).getSync(key, dft) as boolean;
  }

  static put(ctx: common.UIAbilityContext, key: string, value: string | number | boolean): void {
    const p = Prefs.db(ctx);
    p.putSync(key, value);
    p.flush();
  }

  // joinUrl: append stream key to server url (Android AppUtils.joinUrl).
  static joinUrl(server: string, key: string): string {
    if (key.length === 0) return server;
    return server.endsWith('/') ? server + key : server + '/' + key;
  }

  static isValidRtmp(url: string): boolean {
    return /^(rtmp|rtmps|rtmpe|rtmpt):\/\/\S+$/.test(url.trim());
  }

  // pref_tmp_addr_list: JSONArray of {title, rtmp_server, rtmp_stream_key}.
  static rtmpList(ctx: common.UIAbilityContext): RtmpAddr[] {
    try {
      const arr = JSON.parse(Prefs.getStr(ctx, 'pref_tmp_addr_list', '[]')) as RtmpAddr[];
      return Array.isArray(arr) ? arr : [];
    } catch {
      return [];
    }
  }

  static saveRtmpList(ctx: common.UIAbilityContext, list: RtmpAddr[]): void {
    Prefs.put(ctx, 'pref_tmp_addr_list', JSON.stringify(list));
  }

  // Android Y(): remember a successfully used url/key, newest first.
  static rememberRtmp(ctx: common.UIAbilityContext, server: string, key: string, title?: string): void {
    const joined = Prefs.joinUrl(server, key);
    if (!Prefs.isValidRtmp(joined)) return;
    const list = Prefs.rtmpList(ctx);
    if (list.some((e) => Prefs.joinUrl(e.rtmp_server, e.rtmp_stream_key) === joined)) return;
    try {
      const m = /^[a-z]+:\/\/([^/:]+)/i.exec(joined);
      const host = m !== null ? m[1] : '';
      if (host.length === 0) return;
      const t = (title ?? '').length > 0 ? (title as string) : host;
      list.unshift({ title: t, rtmp_server: server, rtmp_stream_key: key });
      Prefs.saveRtmpList(ctx, list);
    } catch {
      // ignore malformed urls
    }
  }
}
