// Incremental MIME framing. Accept JPEG parts only, with or without Content-Length.
// Bound the unfinished part, MIME headers and the boundary search across chunks.
const MAX_JPEG = 2 * 1024 * 1024;
const MAX_HEADERS = 8192;

export function mjpegBoundary(contentType: string): string {
  if (contentType.split(';')[0].trim().toLowerCase() !== 'multipart/x-mixed-replace') return '';
  const m = /;\s*boundary\s*=\s*(?:"([^"\r\n]+)"|([^;\s]+))/i.exec(contentType);
  if (!m) return '';
  const value = m[1] || m[2];
  return /^[0-9A-Za-z'()+_,.\/:=? -]{1,70}$/.test(value) && !value.endsWith(' ') ? value : '';
}

function bytes(s: string): Uint8Array {
  const b = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i);
  return b;
}

export class MjpegFrames {
  private data: Uint8Array = new Uint8Array(16384);
  private used: number = 0;
  private scan: number = 0;
  private state: number = 0; // boundary, headers, JPEG body
  private size: number = -1;
  private boundary: Uint8Array;
  private separator: Uint8Array;
  private headers: Uint8Array = bytes('\r\n\r\n');

  constructor(value: string) {
    this.boundary = bytes('--' + value);
    this.separator = bytes('\r\n--' + value);
  }

  private find(needle: Uint8Array): number {
    for (; this.scan + needle.length <= this.used; this.scan++) {
      let i = 0;
      while (i < needle.length && this.data[this.scan + i] === needle[i]) i++;
      if (i === needle.length) return this.scan;
    }
    return -1;
  }

  private consume(count: number): void {
    this.data.copyWithin(0, count, this.used);
    this.used -= count;
    this.scan = 0;
  }

  feed(chunk: Uint8Array, frame: (jpeg: ArrayBuffer) => void): void {
    for (let offset = 0; offset < chunk.length; offset += 16384) {
      const part = chunk.subarray(offset, Math.min(offset + 16384, chunk.length));
      if (this.used + part.length > this.data.length) {
        const next = new Uint8Array(Math.min(MAX_JPEG + 32768, this.data.length * 2));
        if (next.length < this.used + part.length) throw new Error('part limit');
        next.set(this.data.subarray(0, this.used));
        this.data = next;
      }
      this.data.set(part, this.used);
      this.used += part.length;
      while (this.parse(frame)) {}
    }
  }

  private parse(frame: (jpeg: ArrayBuffer) => void): boolean {
    if (this.state === 0) {
      const i = this.find(this.boundary);
      if (i < 0 || i + this.boundary.length + 2 > this.used) {
        if (this.used > MAX_HEADERS) throw new Error('invalid boundary');
        return false;
      }
      const after = i + this.boundary.length;
      if (this.data[after] === 45 && this.data[after + 1] === 45) {
        this.state = 3;
        this.consume(this.used);
        return false;
      }
      if (this.data[after] !== 13 || this.data[after + 1] !== 10) throw new Error('invalid boundary');
      this.consume(after + 2);
      this.state = 1;
      return true;
    }
    if (this.state === 1) {
      const i = this.find(this.headers);
      if (i > MAX_HEADERS || (i < 0 && this.used > MAX_HEADERS)) throw new Error('header limit');
      if (i < 0) return false;
      let text = '';
      for (let j = 0; j < i; j++) text += String.fromCharCode(this.data[j]);
      if (!/(^|\r\n)content-type:\s*image\/jpeg\s*(\r\n|$)/i.test(text)) throw new Error('not JPEG');
      const len = /(^|\r\n)content-length:\s*([^\r\n]+)/i.exec(text);
      this.size = len ? Number(len[2].trim()) : -1;
      if (len && (!/^[0-9]+$/.test(len[2].trim()) || this.size < 4 || this.size > MAX_JPEG)) {
        throw new Error('invalid frame size');
      }
      this.consume(i + 4);
      this.state = 2;
      return true;
    }
    if (this.state === 2) {
      const n = this.size < 0 ? this.find(this.separator) : this.size;
      if (n > MAX_JPEG || (n < 0 && this.used > MAX_JPEG + this.separator.length)) throw new Error('frame limit');
      if (n < 0 || this.used < n) return false;
      if (n < 4 || this.data[0] !== 255 || this.data[1] !== 216 ||
        this.data[n - 2] !== 255 || this.data[n - 1] !== 217) throw new Error('invalid JPEG');
      frame(this.data.slice(0, n).buffer as ArrayBuffer);
      this.consume(n);
      this.state = 0;
      return true;
    }
    this.consume(this.used);
    return false;
  }
}
