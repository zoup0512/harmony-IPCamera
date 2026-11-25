export interface CameraResolution {
  width: number;
  height: number;
}

export interface CaptureFpsRange {
  min: number;
  max: number;
}

export interface CaptureFpsInfo {
  ranges: CaptureFpsRange[];
  // CameraKit's active range, not a measurement of delivered frames.
  active?: CaptureFpsRange;
}

function positiveInt(value: number): boolean {
  return Number.isInteger(value) && value > 0 && value <= 2147483647;
}

export function parseCameraResolutions(json: string): CameraResolution[] {
  const result: CameraResolution[] = [];
  try {
    const sizes = JSON.parse(json) as CameraResolution[];
    if (!Array.isArray(sizes)) return result;
    sizes.forEach((size: CameraResolution) => {
      if (size && positiveInt(size.width) && positiveInt(size.height) &&
        !result.some((r: CameraResolution) => r.width === size.width && r.height === size.height)) {
        result.push(size);
      }
    });
  } catch (_) {}
  return result;
}

// Persist width and height (per facing), never the index of a previous enumeration.
export function selectCameraResolution(sizes: CameraResolution[], preferredWidth: number,
  preferredHeight: number): CameraResolution | undefined {
  return sizes.find((r: CameraResolution) => r.width === preferredWidth && r.height === preferredHeight) ?? sizes[0];
}

function validRange(range: CaptureFpsRange | undefined): boolean {
  return !!range && positiveInt(range.min) && positiveInt(range.max) && range.min <= range.max;
}

export function parseCaptureFps(json: string): CaptureFpsInfo {
  const result: CaptureFpsInfo = { ranges: [] };
  try {
    const info = JSON.parse(json) as CaptureFpsInfo;
    if (!info || !Array.isArray(info.ranges)) return result;
    info.ranges.forEach((r: CaptureFpsRange) => {
      if (validRange(r) && !result.ranges.some((v: CaptureFpsRange) => v.min === r.min && v.max === r.max)) {
        result.ranges.push(r);
      }
    });
    if (validRange(info.active)) result.active = info.active;
  } catch (_) {}
  return result;
}

export interface CameraDeviceOption {
  id: string;
  front: boolean;
}

export interface EncoderOption {
  name: string;
  hardware: boolean;
  profiles: number[];
  // 0/0 when the implementation reported no usable range.
  minBitrate: number;
  maxBitrate: number;
}

export interface FocusInfo {
  modes: number[];
  active?: number;
}

export interface CodecTuning {
  bitrate: number; // bit/s; zero requests an initial rate based on the image size.
  iFrameMs: number;
}

export function parseCameraDevices(json: string): CameraDeviceOption[] {
  const out: CameraDeviceOption[] = [];
  try {
    const arr = JSON.parse(json) as CameraDeviceOption[];
    if (!Array.isArray(arr)) return out;
    arr.forEach((d: CameraDeviceOption) => {
      if (d && typeof d.id === 'string' && d.id.length > 0 && typeof d.front === 'boolean' &&
        !out.some((v: CameraDeviceOption) => v.id === d.id)) out.push(d);
    });
  } catch (_) {}
  return out;
}

function uniqueKnown(values: number[], known: number[]): number[] {
  if (!Array.isArray(values)) return [];
  return values.filter((v: number, i: number) => known.includes(v) && values.indexOf(v) === i);
}

export function parseEncoderOptions(json: string): EncoderOption[] {
  const out: EncoderOption[] = [];
  try {
    const arr = JSON.parse(json) as EncoderOption[];
    if (!Array.isArray(arr)) return out;
    arr.forEach((e: EncoderOption) => {
      if (!e || typeof e.name !== 'string' || e.name.length === 0 || typeof e.hardware !== 'boolean' ||
        out.some((v: EncoderOption) => v.name === e.name)) return;
      const range = (positiveInt(e.minBitrate) || e.minBitrate === 0) && positiveInt(e.maxBitrate) &&
        e.minBitrate <= e.maxBitrate;
      out.push({ name: e.name, hardware: e.hardware, profiles: uniqueKnown(e.profiles, [0, 4, 8]),
        minBitrate: range ? e.minBitrate : 0, maxBitrate: range ? e.maxBitrate : 0 });
    });
  } catch (_) {}
  return out;
}

export function parseFocusInfo(json: string): FocusInfo {
  const out: FocusInfo = { modes: [] };
  try {
    const info = JSON.parse(json) as FocusInfo;
    const known: number[] = [0, 1, 2, 3];
    if (!info || !Array.isArray(info.modes)) return out;
    out.modes = uniqueKnown(info.modes, known);
    if (info.active !== undefined && known.includes(info.active)) out.active = info.active;
  } catch (_) {}
  return out;
}

export function codecTuningKey(mime: string): string {
  return mime === 'video/hevc' ? 'camera.tuning.hevc' : 'camera.tuning.avc';
}

export function parseCodecTuning(json: string, legacyBitrate: number, legacyIFrameMs: number): CodecTuning {
  const out: CodecTuning = { bitrate: positiveInt(legacyBitrate) ? legacyBitrate : 0,
    iFrameMs: positiveInt(legacyIFrameMs) ? legacyIFrameMs : 2000 };
  try {
    const t = JSON.parse(json) as CodecTuning;
    if (t && (t.bitrate === 0 || positiveInt(t.bitrate))) out.bitrate = t.bitrate;
    if (t && positiveInt(t.iFrameMs)) out.iFrameMs = t.iFrameMs;
  } catch (_) {}
  return out;
}

// Initial target, not congestion control. Match the Android HEVC, AVC hardware,
// and AVC software initial multipliers. MIME auto uses the AVC default policy
// when the chosen implementation is unknown; do not label it hardware.
export function resolveCameraBitrate(tuning: CodecTuning, width: number, height: number,
  encoder?: EncoderOption, mime: string = 'video/avc'): number {
  if (positiveInt(tuning.bitrate)) return tuning.bitrate;
  if (tuning.bitrate !== 0 || !positiveInt(width) || !positiveInt(height)) return 0;
  const multiplier = mime === 'video/hevc' ? 2 : (encoder && !encoder.hardware ? 4 : 6);
  const max = encoder && positiveInt(encoder.maxBitrate) ? encoder.maxBitrate : 2147483647;
  const min = encoder && positiveInt(encoder.minBitrate) ? encoder.minBitrate : 1;
  return Math.max(min, Math.min(max, width * height * multiplier));
}
