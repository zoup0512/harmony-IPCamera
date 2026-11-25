import { createRtspServer, rtspStart, rtspStop, rtspIsRunning, rtspClientCount, serviceBoundV4Port,
  rtspSendH264, rtspSendAdts, rtspSetStatusCallback, rtspClearStatusCallback,
  serviceViewerCounts, ViewerCounts, cameraToggleTorch,
  cameraSetDualLayout, cameraDualLayout, cameraDevices, encoderOptions, cameraFocusInfo, cameraSetFocusMode, cameraFocusAt,
  cameraResolutions, cameraRemoteSizeState, cameraCaptureFps, cameraSetCaptureFps, rtspStartTestPattern,
  rtspStopTestPattern, rtspStartCamera, rtspStopCamera, rtspStartMic, rtspMicFormat, MicFormat, rtspStopMic,
  rtmpStart, rtmpStop, rtmpFailure, rtspStartRecord, rtspSetArchiveCallback, rtspClearArchiveCallback, rtspTakePendingArchives, rtspStopRecord, rtspIsRecording, rtspTakeSnapshot,
  rtspStartHttp, rtspStopHttp, rtspSetMotion, rtspSetMotionTimeout, rtspMotionActive,
  osdSetConfig, osdSetDeviceName, osdSetBattery, osdSetGps, osdSetWatermark,
  onvifScan,
  RtspStartConfig, HttpOptions, OsdConfig, RecordOptions, OnvifDevice
} from 'libstreaming.so';

import { RtmpTlsTransport } from '../util/RtmpTlsTransport';
import { DualCameraPlan } from '../util/DualCamera';

export { HttpOptions } from 'libstreaming.so';

export { RtspStartConfig } from 'libstreaming.so';

export { OsdConfig } from 'libstreaming.so';

export { RecordOptions } from 'libstreaming.so';

export { OnvifDevice } from 'libstreaming.so';

export enum RtspEvent {
  Connected = 0,
  Disconnected = 1,
  Playing = 2,
  Error = 3,
  CameraSizeChanged = 4
}

export interface CameraRemoteSizeState {
  generation: number;
  running: boolean;
  width: number;
  height: number;
}

export interface RtspStatusInfo {
  event: RtspEvent;
  clients: number;
  detail: string;
}

export class RtspServer {
  private handle: object = createRtspServer();
  private tls: RtmpTlsTransport = new RtmpTlsTransport();
  private tlsPublisher: boolean = false;
  private publishGeneration: number = 0;

  start(config: RtspStartConfig): boolean {
    return rtspStart(this.handle, config);
  }

  stop(): void {
    rtspStop(this.handle);
  }

  boundV4Port(http: boolean): number { return serviceBoundV4Port(this.handle, http); }

  isRunning(): boolean {
    return rtspIsRunning(this.handle);
  }

  clientCount(): number {
    return rtspClientCount(this.handle);
  }

  viewerCounts(): ViewerCounts { return serviceViewerCounts(this.handle); }

  toggleTorch(): boolean { return cameraToggleTorch(this.handle); }

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

  setDualLayout(layout: number, plan?: DualCameraPlan): number {
    if (!plan) return cameraSetDualLayout(this.handle, layout);
    return cameraSetDualLayout(this.handle, layout, plan);
  }

  dualLayout(): number { return cameraDualLayout(this.handle); }

  cameraDevices(): string {
    return cameraDevices();
  }

  encoderOptions(mime: string): string {
    return encoderOptions(mime);
  }

  cameraFocusInfo(): string {
    return cameraFocusInfo(this.handle);
  }

  cameraSetFocusMode(mode: number): boolean {
    return cameraSetFocusMode(this.handle, mode);
  }

  cameraFocusAt(x: number, y: number): boolean {
    return cameraFocusAt(this.handle, x, y);
  }

  cameraResolutions(front: boolean, deviceId: string = ''): string {
    return cameraResolutions(front, deviceId);
  }

  cameraRemoteSizeState(): CameraRemoteSizeState | undefined {
    try {
      const s = JSON.parse(cameraRemoteSizeState(this.handle)) as CameraRemoteSizeState;
      if (!s || !Number.isSafeInteger(s.generation) || s.generation < 0 || typeof s.running !== 'boolean' ||
        !Number.isSafeInteger(s.width) || !Number.isSafeInteger(s.height)) return undefined;
      if (s.running ? (s.width <= 0 || s.height <= 0) : (s.width !== 0 || s.height !== 0)) return undefined;
      return s;
    } catch (_) {
      return undefined;
    }
  }

  cameraCaptureFps(): string {
    return cameraCaptureFps(this.handle);
  }

  cameraSetCaptureFps(min: number, max: number): boolean {
    return cameraSetCaptureFps(this.handle, min, max);
  }

  startCamera(width: number, height: number, bitrate: number, mime: string,
    frontCamera: boolean, iFrameIntervalMs: number, deviceId: string = '', encoderName: string = '',
    avcProfile: number = -1, captureRotation: number = -1, captureMirror: boolean = false): boolean {
    return rtspStartCamera(this.handle, width, height, bitrate, mime, frontCamera,
      iFrameIntervalMs, deviceId, encoderName, avcProfile, captureRotation, captureMirror);
  }

  stopCamera(): void {
    rtspStopCamera(this.handle);
  }

  startMic(sampleRate: number = 48000, channels: number = 1): boolean {
    return rtspStartMic(this.handle, sampleRate, channels);
  }

  micFormat(): MicFormat {
    return rtspMicFormat(this.handle);
  }

  stopMic(): void {
    rtspStopMic(this.handle);
  }

  async startRtmp(url: string): Promise<boolean> {
    const generation = ++this.publishGeneration;
    this.tls.stop();
    this.tlsPublisher = url.toLowerCase().startsWith('rtmps://');
    if (this.tlsPublisher) {
      const ok = await this.tls.start(url, (token: number) => rtmpStart(this.handle, url, token));
      if (generation !== this.publishGeneration) return false;
      const cause = this.tls.failure();
      if (!ok && cause) throw new Error(cause); // Local static reason; certificate errors never enter transport retry.
      return ok;
    }
    if (!url.toLowerCase().startsWith('rtmp://')) return false;
    return rtmpStart(this.handle, url);
  }

  setArchiveCallback(callback: (filePath: string) => void): boolean {
    return rtspSetArchiveCallback(this.handle, callback);
  }

  takePendingArchives(): string[] { return rtspTakePendingArchives(this.handle); }

  clearArchiveCallback(): void {
    rtspClearArchiveCallback(this.handle);
  }

  startRecord(filePath: string, options?: RecordOptions): boolean {
    if (options !== undefined) {
      return rtspStartRecord(this.handle, filePath, options);
    }
    return rtspStartRecord(this.handle, filePath);
  }

  isRecording(): boolean {
    return rtspIsRecording(this.handle);
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

  setMotionTimeout(seconds: number): void { rtspSetMotionTimeout(this.handle, seconds); }

  motionActive(): boolean {
    return rtspMotionActive(this.handle);
  }

  rtmpFailure(nativeEvent: string = ''): string {
    const snapshot = rtmpFailure(this.handle);
    // Both sources belong to the current native generation. Never cache a rejection
    // across stop/start or let a simultaneous TLS EOF turn it into a retry.
    const rejected = /^(rtmp: )?(startup failed|connection lost): server rejected:/;
    if (rejected.test(nativeEvent) || rejected.test(snapshot)) return 'server rejected';
    return (this.tlsPublisher ? this.tls.failure() : '') || nativeEvent || snapshot;
  }

  stopRtmp(): void {
    this.publishGeneration++; this.tlsPublisher = false;
    this.tls.stop();
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
