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
export const rtspClientCount: (server: object) => number;

/** Push one Annex-B H.264/H.265 frame. tsUs = presentation timestamp in microseconds, 0 = use native clock. */
export const rtspSendH264: (server: object, data: ArrayBuffer, tsUs: number) => void;

/** Push one ADTS AAC frame. tsUs in microseconds, 0 = use native clock. */
export const rtspSendAdts: (server: object, data: ArrayBuffer, tsUs: number) => void;

/**
 * Status callback. event: 0=client connected, 1=client disconnected, 2=client playing, 3=error.
 * clients: current number of connected sessions. detail: human readable text.
 */
export const rtspSetStatusCallback: (server: object,
  callback: (event: number, clients: number, detail: string) => void) => void;

/** Feed an internally encoded color-bar test pattern (OH_AVCodec H.264) into the server. */
export const rtspStartTestPattern: (server: object, width: number, height: number,
  fps: number, bitrate: number) => boolean;
export const rtspStopTestPattern: (server: object) => void;

/** Start device camera capture -> encoder (mime: video/avc | video/hevc) -> RTSP. */
export const rtspStartCamera: (server: object, width: number, height: number,
  bitrate: number, mime: string, frontCamera: boolean,
  iFrameIntervalMs: number) => boolean;
export const rtspStopCamera: (server: object) => void;

/** Start microphone capture -> AAC encoder -> ADTS -> RTSP audio track (48kHz mono). */
export const rtspStartMic: (server: object) => boolean;
export const rtspStopMic: (server: object) => void;

/** Publish the same video/audio to an RTMP URL (rtmp://host[:port]/app/streamKey). */
export const rtmpStart: (server: object, url: string) => boolean;
export const rtmpStop: (server: object) => void;

/** Record the shared streams into an MP4 file (H.264/H.265 + AAC). */
export const rtspStartRecord: (server: object, filePath: string) => boolean;
export const rtspStopRecord: (server: object) => void;

/** Decode the latest cached keyframe to a 24-bit BMP (async, result via status callback). */
export const rtspTakeSnapshot: (server: object, filePath: string) => boolean;

export interface HttpOptions {
  port: number;
  user: string;
  password: string;
  filesDir: string;
}

/** Start the web console (dashboard, MJPEG, snapshot, torch, talk-back, archives). */
export const rtspStartHttp: (server: object, options: HttpOptions) => boolean;
export const rtspStopHttp: (server: object) => void;
/** Enable/disable motion detection on the decoded stream (events via status callback). */
export const rtspSetMotion: (server: object, enabled: boolean, timeoutSeconds?: number) => void;
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
