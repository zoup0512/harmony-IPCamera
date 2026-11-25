import { batteryInfo } from '@kit.BasicServicesKit';

export interface OsdFontOptions {
  fontSize: number;   // Encoded pixels; 0 follows the frame height.
  fontFamily: string; // Empty uses the default system family.
  fontPath: string;   // Private app file, never a picker URI.
}

export function defaultOsdFont(): OsdFontOptions {
  return { fontSize: 0, fontFamily: '', fontPath: '' };
}

export function osdRestartRequired(oldText: boolean, oldWatermark: boolean,
  newText: boolean, newWatermark: boolean): boolean {
  return (oldText || oldWatermark) !== (newText || newWatermark);
}

// Match Android's charging-only suffix: FULL/DISABLE are not power sources.
export function osdChargeSource(status: batteryInfo.BatteryChargeState,
  plugged: batteryInfo.BatteryPluggedType): number {
  if (status !== batteryInfo.BatteryChargeState.ENABLE) return 0;
  switch (plugged) {
    case batteryInfo.BatteryPluggedType.AC: return 1;
    case batteryInfo.BatteryPluggedType.USB: return 2;
    case batteryInfo.BatteryPluggedType.WIRELESS: return 3;
    default: return 0;
  }
}
