/**
 * Native RTSP streaming module (replacement of the original libstreaming.so
 * JNI interface, re-exposed to ArkTS through Node-API).
 */

export interface RtspStartConfig {
  port: number;
  path: string;
  user: string;
  password: string;
}

/** Create a server handle. Returns an opaque native object. */
export const createRtspServer: () => object;

export const rtspStart: (server: object, config: RtspStartConfig) => boolean;
export const rtspStop: (server: object) => void;
export const rtspIsRunning: (server: object) => boolean;
export const serviceBoundV4Port: (server: object, http: boolean) => number;
export const rtspClientCount: (server: object) => number;
export interface ViewerCounts {
  rtsp: number;
  mjpeg: number;
  flv: number;
  opus: number;
}
export const serviceViewerCounts: (server: object) => ViewerCounts;
export const cameraToggleTorch: (server: object) => boolean;

/** Push one Annex-B H.264/H.265 frame. tsUs = presentation timestamp in microseconds, 0 = use native clock. */
export const rtspSendH264: (server: object, data: ArrayBuffer, tsUs: number) => void;

/** Push one ADTS AAC frame. tsUs in microseconds, 0 = use native clock. */
export const rtspSendAdts: (server: object, data: ArrayBuffer, tsUs: number) => void;

/**
 * Status callback. event: 0=client connected, 1=client disconnected, 2=client playing, 3=error,
 * 4=camera size changed (broadcast, not a session event; re-read cameraRemoteSizeState).
 * clients: current number of connected sessions. detail: human readable text.
 */
export const rtspSetStatusCallback: (server: object,
  callback: (event: number, clients: number, detail: string) => void) => void;
export const rtspClearStatusCallback: (server: object) => void;

/** Feed an internally encoded color-bar test pattern (OH_AVCodec H.264) into the server. */
export const rtspStartTestPattern: (server: object, width: number, height: number,
  fps: number, bitrate: number) => boolean;
export const rtspStopTestPattern: (server: object) => void;

/** Start device camera capture -> encoder (mime: video/avc | video/hevc) -> RTSP. */
export const rtspStartCamera: (server: object, width: number, height: number,
  bitrate: number, mime: string, frontCamera: boolean,
  iFrameIntervalMs: number, deviceId?: string, encoderName?: string, avcProfile?: number,
  captureRotation?: number, captureMirror?: boolean) => boolean;
export const rtspStopCamera: (server: object) => void;

/** Defaults to 48 kHz mono. An explicit sample rate of 0 requests automatic selection. */
export const rtspStartMic: (server: object, sampleRate?: number, channels?: number) => boolean;
export interface MicFormat { sampleRate: number; channels: number; }
/** Actual capture format; both fields are zero when stopped. */
export const rtspMicFormat: (server: object) => MicFormat;
export const rtspStopMic: (server: object) => void;

/** Publish the same video/audio to an RTMP URL (rtmp://host[:port]/app/streamKey). */
export const rtmpStart: (server: object, url: string, tlsToken?: number) => Promise<boolean>;
export const rtmpTlsOpen: (authority: string) => number;
export const rtmpTlsRead: (token: number) => ArrayBuffer | undefined;
export const rtmpTlsWrite: (token: number, data: ArrayBuffer) => void;
export const rtmpTlsClose: (token: number) => void;
export const rtmpStop: (server: object) => void;
export const rtmpFailure: (server: object) => string;

export const cameraDevices: () => string;
export const encoderOptions: (mime: string) => string;
export const cameraResolutions: (front: boolean, deviceId?: string) => string;
export interface DualCameraPlan {
  primaryId: string;
  secondaryId: string;
  primaryType: number;
  secondaryType: number;
  width: number;
  height: number;
  secondaryWidth: number;
  secondaryHeight: number;
}
/** Layout 0 single, 1 PiP, 2 side by side, 3 stacked. Layouts 1..3 require a concurrent plan.
 * Result: 0 applied/unchanged, 1 rejected/busy, 2 failed but restored, 3 stopped. */
export const cameraSetDualLayout: (server: object, layout: number, plan?: DualCameraPlan) => number;
export const cameraDualLayout: (server: object) => number;
export const cameraFocusInfo: (server: object) => string;
export const cameraSetFocusMode: (server: object, mode: number) => boolean;
/** Coordinates are normalized to [0, 1] in camera space. */
export const cameraFocusAt: (server: object, x: number, y: number) => boolean;
/** JSON {generation,running,width,height}: generation starts at 0 and advances only on remote
 * reconfiguration (including rollback/failure). Dimensions are capture pixels, or 0x0 if stopped. */
export const cameraRemoteSizeState: (server: object) => string;
export const cameraCaptureFps: (server: object) => string;
export const cameraSetCaptureFps: (server: object, min: number, max: number) => boolean;

/** Record the shared streams into an MP4 file (H.264/H.265 + AAC). */
export interface RecordOptions {
  /** four_gb_limit: rotate before MP4 32-bit sample offsets overflow (default true) */
  fourGbLimit: boolean;
  /** each_segment_length: new numbered segment every N minutes (0 = off, default 10) */
  segmentMinutes: number;
  /** Defaults to true. False records video only, without stopping the microphone. */
  includeAudio?: boolean;
  /** Omit or use '' for passthrough. 'video/avc' or 'video/hevc' requests a separate codec. */
  videoMime?: string;
  /** Independent encoder only: bits/s (250000..20000000, default 4000000). */
  videoBitrate?: number;
  /** Independent encoder only: keyframe interval in milliseconds (500..10000, default 2000). */
  gopMs?: number;
}
export const rtspStartRecord: (server: object, filePath: string,
  options?: RecordOptions) => boolean;
export const rtspIsRecording: (server: object) => boolean;
export const recordingFileBusy: (filePath: string) => boolean;
export interface CloudRecordingPin { current(): boolean; remove(): boolean; close(): void }
export const pinCloudRecording: (filesDir: string, name: string, uploadFd: number) => CloudRecordingPin | undefined;
export const rtspStopRecord: (server: object) => void;
/** Local, opt-in notification after a nonempty segment was finalized and closed successfully. */
export const rtspSetArchiveCallback: (server: object, callback: (filePath: string) => void) => boolean;
/** After StopRecord joins the writer, take any still-undelivered paths before clearing. A path
 * is consumed either here or by the callback, never twice. Up to 256 pending paths per registration;
 * no persistence across process death. Clear cancels rather than drains the current registration. */
export const rtspTakePendingArchives: (server: object) => string[];
export const rtspClearArchiveCallback: (server: object) => void;

/** Decode the latest cached keyframe to a 24-bit BMP (async, result via status callback). */
export const rtspTakeSnapshot: (server: object, filePath: string) => boolean;

export interface HttpOptions {
  port: number;
  user: string;
  password: string;
  filesDir: string;
  allowLegacyVideoPassword?: boolean;
}

/** Start the web console (dashboard, MJPEG, snapshot, torch, talk-back, archives). */
export const rtspStartHttp: (server: object, options: HttpOptions) => boolean;
export const rtspStopHttp: (server: object) => void;
/** Enable/disable motion detection on the decoded stream (events via status callback). */
export const rtspSetMotion: (server: object, enabled: boolean, timeoutSeconds?: number) => void;
/** Authoritative, time-based state; independent of best-effort status callbacks. */
export const rtspMotionActive: (server: object) => boolean;
/** Set motion detection hold time in seconds. */
export const rtspSetMotionTimeout: (server: object, seconds: number) => void;

/**
 * OSD ("text overlay") configuration, mirroring the Android text-overlay
 * settings sub-page. Takes effect on the next rendered frame except for
 * `enabled`, which also decides whether the camera pipeline runs through the
 * GL interception path (requires a camera restart when flipped).
 */
export interface OsdConfig {
  enabled: boolean;
  /** osd_padding_int 0..100, scaled against the frame height */
  padding: number;
  /** text_position: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */
  position: number;
  /** font_style: 0 normal, 1 bold, 2 italic, 3 bold-italic */
  fontStyle: number;
  fontSize?: number;
  fontFamily?: string;
  /** Imported font in the app-private font directory. */
  fontPath?: string;
  /** text_color as #RRGGBBAA */
  color: string;
  /** display_timestamp (yyyy-MM-dd HH:mm:ss) */
  showTimestamp: boolean;
  /** display_dev_name */
  showDevName: boolean;
  /** display_battery_info */
  showBattery: boolean;
  /** display_gps_location */
  showGps: boolean;
  /** speed_unit: false km/h, true mph */
  speedMph: boolean;
  /** text_custom */
  customText: string;
  /** watermark_overlay */
  wmEnabled: boolean;
  /** watermark_padding_int 0..100 */
  wmPadding: number;
  /** watermark_max_area_occupied_int 1..100 (% of frame area) */
  wmMaxAreaPercent: number;
  /** watermark_position: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */
  wmPosition: number;
}
export const osdSetConfig: (server: object, config: OsdConfig) => boolean;
/** Device name shown on the OSD (deviceInfo.productModel). */
export const osdSetDeviceName: (server: object, name: string) => void;
/** Battery percent and charging state (batteryInfo.batterySOC/chargingState). */
export const osdSetBattery: (server: object, level: number, chargeState: number) => void;
/** GPS fix; speed in km/h, unit flag only picks the label unit. */
export const osdSetGps: (server: object, lat: number, lng: number, speedKmh: number,
  valid: boolean, speedMph: boolean) => void;
/** Watermark image from a decoded RGBA_8888 PixelMap. */
export const osdSetWatermark: (server: object, pixelmap: object) => boolean;

/** ONVIF device discovered by onvifScan (JSON fields). */
export interface OnvifDevice {
  /** device service endpoint, http://ip:port/onvif/device_service */
  xaddrs: string;
  /** onvif://www.onvif.org/Model scope value (may be empty) */
  model: string;
  /** first media profile token (empty when resolution failed) */
  profile: string;
  /** rtsp://... stream URI of that profile (empty when unavailable) */
  streamUri: string;
  /** last SOAP/parse error (empty on success) */
  error: string;
}
/**
 * WS-Discovery probe + GetProfiles + GetStreamUri. Resolves with a JSON array
 * of OnvifDevice objects (parse with JSON.parse). timeoutMs default 4000.
 */
export const onvifScan: (timeoutMs?: number) => Promise<string>;

/** Interleaved H.264. No userinfo in URL. Optional AAC negotiation never unmutes the renderer. */
export const liveAvcStart: (url: string, basicToken: string, surfaceId: string, offerAudio?: boolean) => object;
/** Digest only: explicit nonempty credentials, userinfo-free URL. No Basic fallback or credential retries.
 * Uses the same playback handle/status APIs. -2: authentication denied; -8: unsupported Digest challenge. */
export const liveAvcStartDigest: (url: string, username: string, password: string, surfaceId: string, offerAudio?: boolean) => object;
/** Explicit unicast UDP, one H.264/H.265 video track, no audio, PAUSE/resume or TCP fallback.
 * Same handle/stop/status as TCP. Empty token selects anonymous; otherwise legacy Basic on RTSP only. */
export const liveAvcStartUdp: (url: string, basicToken: string, surfaceId: string) => object;
/** Same receive-only UDP transport, challenge-only Digest on the RTSP control connection. */
export const liveAvcStartDigestUdp: (url: string, username: string, password: string, surfaceId: string) => object;
/** -2 output failed, -1 no compatible/negotiated track, 0 muted, 1 waiting for AAC, 2 feeding decoder. */
export const liveAvcAudioStatus: (handle: object) => number;
export const liveAvcSetAudio: (handle: object, enabled: boolean) => void;
/** 0 connecting, 1 frame rendered; negative values are terminal (no URL or secrets). */
export const liveAvcStatus: (handle: object) => number;
export const liveAvcStop: (handle: object) => void;
/** Selected RTSP video MIME: 'video/avc' or 'video/hevc'; '' before selection, after stop or for an invalid handle. Not a render signal. */
export const liveVideoCodec: (handle: object) => string;

/** Live playback only: rtmp://host[:port]/app/stream[?query], no userinfo, redirects or retries.
 * Decimal XComponent surface ID. Returns immediately, audio initially muted. Stop on surface loss.
 * Up to 2 workers (including teardown), 2 resolver jobs, 16 retained handles. Release with Stop.
 * Bad argument types/surface IDs or exhausted handle slots throw; connection errors are polled. */
export const rtmpViewerStart: (url: string, surfaceId: string) => object;
/** 0 connecting/waiting for render; 1 AVC frame rendered to Surface. Negative = terminal:
 * -1 network, -2 server rejected, -3 unsupported AVC budget/video codec, -4 unsupported transport/handshake
 * (including RTMPS), -5 malformed URL/protocol/invalid handle, -6 decoder/surface, -7 timeout,
 * -8 enhanced video, -9 stream ended, -10 stopped, -11 workers busy, -12 enhanced HEVC,
 * -13 stream not found, -14 aggregate messages unsupported. Never returns peer text or a stream key. */
export const rtmpViewerStatus: (handle: object) => number;
/** -3 unsupported audio, -2 audio output failed, -1 no track/closed, 0 muted, 1 enabled, 2 feeding AAC-LC. */
export const rtmpViewerAudioStatus: (handle: object) => number;
export const rtmpViewerSetAudio: (handle: object, enabled: boolean) => void;
/** Idempotent, nonblocking cancellation. Do not reuse a stopped handle. */
export const rtmpViewerStop: (handle: object) => void;
