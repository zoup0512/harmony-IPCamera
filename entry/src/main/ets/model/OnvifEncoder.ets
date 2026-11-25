import { MEDIA, TT, SoapElement } from './OnvifSoap';

export interface EncoderValues {
  size: string;
  fps: string;
  bitrate: string;
  level: string;
}
export class Bounds {
  readonly min: number;
  readonly max: number;
  constructor(min: number, max: number) { this.min = min; this.max = max; }
  allows(s: string): boolean {
    return /^(0|[1-9]\d{0,9})$/.test(s) && Number(s) >= this.min && Number(s) <= this.max;
  }
}
function range(parent: SoapElement | undefined, tag: string): Bounds | undefined {
  const r = parent?.optional(TT, tag);
  if (!r) return undefined;
  const min = r.one(TT, 'Min').value();
  const max = r.one(TT, 'Max').value();
  if (!/^\d{1,10}$/.test(min) || !/^\d{1,10}$/.test(max) || Number(max) < Number(min) || Number(max) > 2147483647) {
    throw new Error('无效设备范围');
  }
  return new Bounds(Number(min), Number(max));
}
function dimensions(n: SoapElement): string {
  const w = n.one(TT, 'Width').value();
  const h = n.one(TT, 'Height').value();
  if (!/^[1-9]\d{0,5}$/.test(w) || !/^[1-9]\d{0,5}$/.test(h)) throw new Error('无效分辨率');
  return `${w}x${h}`;
}

export class EncoderEditor {
  readonly codec: string;
  readonly original: EncoderValues;
  readonly sizes: string[];
  readonly levels: string[];
  readonly fpsRange?: Bounds;
  readonly bitrateRange?: Bounds;
  private readonly config: SoapElement;
  private readonly sizeNode: SoapElement;
  private readonly fpsNode?: SoapElement;
  private readonly bitrateNode?: SoapElement;
  private readonly levelNode?: SoapElement;

  constructor(config: SoapElement, options: SoapElement, token: string) {
    if (config.uri !== MEDIA || config.name !== 'Configuration' || config.attrs['token'] !== token) {
      throw new Error('编码配置标识不匹配');
    }
    this.config = config;
    this.codec = config.one(TT, 'Encoding').value();
    if (!['JPEG', 'H264', 'MPEG4'].includes(this.codec)) throw new Error('不支持此编码格式');
    const opt = options.optional(TT, this.codec);
    const ext = options.optional(TT, 'Extension')?.optional(TT, this.codec);
    this.sizeNode = config.one(TT, 'Resolution');
    const rate = config.optional(TT, 'RateControl');
    this.fpsNode = rate?.optional(TT, 'FrameRateLimit');
    this.bitrateNode = rate?.optional(TT, 'BitrateLimit');
    const levelName = this.codec === 'H264' ? 'H264Profile' : 'Mpeg4Profile';
    this.levelNode = config.optional(TT, this.codec)?.optional(TT, levelName);
    this.original = { size: dimensions(this.sizeNode), fps: this.fpsNode?.value() ?? '',
      bitrate: this.bitrateNode?.value() ?? '', level: this.levelNode?.value() ?? '' };
    this.sizes = (opt?.all(TT, 'ResolutionsAvailable') ?? []).map(x => dimensions(x));
    this.levels = (opt?.all(TT, levelName + 'sSupported') ?? []).map(x => x.value());
    this.fpsRange = range(opt, 'FrameRateRange');
    // Media1's bitrate bounds are in VideoEncoderOptionsExtension/<codec>.
    this.bitrateRange = range(ext, 'BitrateRange');
  }

  // Apply only to the previously read tree, serialize, and restore it. Unknown
  // settings (multicast, extensions, use count, session timeout, etc.) survive.
  prepare(v: EncoderValues): string {
    const o = this.original;
    if (v.size === o.size && v.fps === o.fps && v.bitrate === o.bitrate && v.level === o.level) throw new Error('无修改');
    if ((v.size !== o.size && !this.sizes.includes(v.size)) ||
      (v.fps !== o.fps && (!this.fpsNode || !this.fpsRange?.allows(v.fps))) ||
      (v.bitrate !== o.bitrate && (!this.bitrateNode || !this.bitrateRange?.allows(v.bitrate))) ||
      (v.level !== o.level && (!this.levelNode || !this.levels.includes(v.level)))) throw new Error('超出设备选项');
    const w = this.sizeNode.one(TT, 'Width');
    const h = this.sizeNode.one(TT, 'Height');
    const previous = [w.text, h.text, this.fpsNode?.text ?? '', this.bitrateNode?.text ?? '', this.levelNode?.text ?? ''];
    try {
      if (v.size !== o.size) { w.text = v.size.split('x')[0]; h.text = v.size.split('x')[1]; }
      if (this.fpsNode && v.fps !== o.fps) this.fpsNode.text = v.fps;
      if (this.bitrateNode && v.bitrate !== o.bitrate) this.bitrateNode.text = v.bitrate;
      if (this.levelNode && v.level !== o.level) this.levelNode.text = v.level;
      return this.config.serialize(true);
    } finally {
      w.text = previous[0]; h.text = previous[1];
      if (this.fpsNode) this.fpsNode.text = previous[2];
      if (this.bitrateNode) this.bitrateNode.text = previous[3];
      if (this.levelNode) this.levelNode.text = previous[4];
    }
  }
}
