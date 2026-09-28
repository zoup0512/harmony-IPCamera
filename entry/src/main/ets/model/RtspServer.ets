import { createRtspServer, rtspStart, rtspStop, rtspIsRunning, rtspClientCount,
  rtspSendH264, rtspSendAdts, rtspSetStatusCallback, rtspClearStatusCallback,
  rtspStartTestPattern,
  rtspStopTestPattern, rtspStartCamera, rtspStopCamera, rtspStartMic, rtspStopMic,
  rtmpStart, rtmpStop, rtspStartRecord, rtspStopRecord, rtspTakeSnapshot,
  rtspStartHttp, rtspStopHttp, rtspSetMotion, rtspSetMotionTimeout,
  osdSetConfig, osdSetDeviceName, osdSetBattery, osdSetGps, osdSetWatermark,
  onvifScan,
  RtspStartConfig, HttpOptions, OsdConfig, RecordOptions, OnvifDevice
} from 'libstreaming.so';

export { HttpOptions } from 'libstreaming.so';

export { RtspStartConfig } from 'libstreaming.so';

export { OsdConfig } from 'libstreaming.so';

export { RecordOptions } from 'libstreaming.so';

export { OnvifDevice } from 'libstreaming.so';

export enum RtspEvent {
  Connected = 0,
  Disconnected = 1,
  Playing = 2,
  Error = 3
}

export interface RtspStatusInfo {
  event: RtspEvent;
  clients: number;
  detail: string;
}

export class RtspServer {
  private handle: object = createRtspServer();

  start(config: RtspStartConfig): boolean {
    return rtspStart(this.handle, config);
  }

  stop(): void {
    rtspStop(this.handle);
  }

  isRunning(): boolean {
    return rtspIsRunning(this.handle);
  }

  clientCount(): number {
    return rtspClientCount(this.handle);
  }

  setStatusCallback(callback: (event: number, clients: number, detail: string) => void): void {
    rtspSetStatusCallback(this.handle, callback);
  }

  clearStatusCallback(): void {
    rtspClearStatusCallback(this.handle);
  }

  startTestPattern(width: number, height: number, fps: number, bitrate: number): boolean {
    return rtspStartTestPattern(this.handle, width, height, fps, bitrate);
  }

  stopTestPattern(): void {
    rtspStopTestPattern(this.handle);
  }

  startCamera(width: number, height: number, bitrate: number, mime: string,
    frontCamera: boolean, iFrameIntervalMs: number): boolean {
    return rtspStartCamera(this.handle, width, height, bitrate, mime, frontCamera,
      iFrameIntervalMs);
  }

  stopCamera(): void {
    rtspStopCamera(this.handle);
  }

  startMic(): boolean {
    return rtspStartMic(this.handle);
  }

  stopMic(): void {
    rtspStopMic(this.handle);
  }

  startRtmp(url: string): Promise<boolean> {
    return rtmpStart(this.handle, url);
  }

  startRecord(filePath: string, options?: RecordOptions): boolean {
    if (options !== undefined) {
      return rtspStartRecord(this.handle, filePath, options);
    }
    return rtspStartRecord(this.handle, filePath);
  }

  stopRecord(): void {
    rtspStopRecord(this.handle);
  }

  takeSnapshot(filePath: string): boolean {
    return rtspTakeSnapshot(this.handle, filePath);
  }

  startHttp(options: HttpOptions): boolean {
    return rtspStartHttp(this.handle, options);
  }

  stopHttp(): void {
    rtspStopHttp(this.handle);
  }

  setMotion(enabled: boolean, timeoutSec?: number): void {
    if (timeoutSec !== undefined) {
      rtspSetMotion(this.handle, enabled, timeoutSec);
    } else {
      rtspSetMotion(this.handle, enabled);
    }
  }

  stopRtmp(): void {
    rtmpStop(this.handle);
  }

  sendH264(data: ArrayBuffer, tsUs: number): void {
    rtspSendH264(this.handle, data, tsUs);
  }

  sendAdts(data: ArrayBuffer, tsUs: number): void {
    rtspSendAdts(this.handle, data, tsUs);
  }

  setOsdConfig(config: OsdConfig): boolean {
    return osdSetConfig(this.handle, config);
  }

  setOsdDeviceName(name: string): void {
    osdSetDeviceName(this.handle, name);
  }

  setOsdBattery(level: number, chargeState: number): void {
    osdSetBattery(this.handle, level, chargeState);
  }

  setOsdGps(lat: number, lng: number, speedKmh: number, valid: boolean,
    speedMph: boolean): void {
    osdSetGps(this.handle, lat, lng, speedKmh, valid, speedMph);
  }

  setOsdWatermark(pixelmap: object): boolean {
    return osdSetWatermark(this.handle, pixelmap);
  }

  scanOnvif(timeoutMs?: number): Promise<Array<OnvifDevice>> {
    const p: Promise<string> = onvifScan(timeoutMs);
    return p.then((json: string) => JSON.parse(json) as Array<OnvifDevice>);
  }
}
