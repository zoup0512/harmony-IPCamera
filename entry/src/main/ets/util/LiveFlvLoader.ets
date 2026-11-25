import { http } from '@kit.NetworkKit';
import { media } from '@kit.MediaKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { LiveEndpoint } from './LiveEndpoint';

// A live FLV is one sequential resource, unlike HLS. Keep Basic inside our own
// transport: no SDK headers, redirects, SDK-selected URLs, byte-range requests or retries.
export class LiveFlvLoader implements media.MediaSourceLoader {
  private target: LiveEndpoint;
  private failure: (code: number) => void;
  private loading?: media.MediaSourceLoadingRequest;
  private req?: http.HttpRequest;
  private id: number = 0;
  private disposed: boolean = false;
  private header: boolean = false;
  private ended: boolean = false;
  private queue: ArrayBuffer[] = [];
  private bytes: number = 0;
  private offset: number = 0;
  private wanted: number = 0;

  constructor(target: LiveEndpoint, failure: (code: number) => void) { this.target = target; this.failure = failure; }

  open = (loading: media.MediaSourceLoadingRequest): number => {
    if (this.disposed || this.loading || loading.url !== this.target.address) return -1;
    this.loading = loading;
    const id = ++this.id;
    // Return the unique handle before any callback can push headers or data.
    Promise.resolve().then(() => { if (this.live(id)) this.connect(id); });
    return id;
  };

  read = (id: number, offset: number, length: number): void => {
    if (!this.live(id)) return;
    if (offset !== this.offset || !Number.isSafeInteger(length) || length < -1) { this.fail(id, 0); return; }
    this.wanted = length;
    this.drain(id);
  };

  close = (id: number): void => {
    if (!this.live(id)) return;
    this.loading = undefined;
    if (this.req) this.req.destroy();
    this.req = undefined;
    this.queue = [];
    this.bytes = 0;
    this.wanted = 0;
    this.offset = 0;
    this.header = false;
    this.ended = false;
  };

  dispose(): void { this.close(this.id); this.disposed = true; }

  private live(id: number): boolean { return !this.disposed && !!this.loading && this.id === id; }

  private fail(id: number, code: number): void {
    if (!this.live(id)) return;
    const loading = this.loading;
    this.dispose();
    try { loading?.finishLoading(id, media.LoadingRequestError.LOADING_ERROR_NO_RESOURCE); } catch (_) {}
    this.failure(code);
  }

  private connect(id: number): void {
    try {
      const req = http.createHttp();
      this.req = req;
      req.on('headersReceive', (obj: Object) => {
        if (!this.live(id)) return;
        const headers = obj as Record<string, string>;
        for (const key of Object.keys(headers)) {
          if (key.toLowerCase() === 'location') { this.fail(id, 302); return; }
        }
        for (const key of Object.keys(headers)) {
          if (key.toLowerCase() !== 'content-type' || !/^video\/(?:x-)?flv(?:\s*;|$)/i.test(String(headers[key]))) continue;
          try { this.loading?.respondHeader(id, { 'Content-Type': 'video/x-flv' }); this.header = true; }
          catch (_) { this.fail(id, 0); }
          return;
        }
      });
      req.on('dataReceive', (data: ArrayBuffer) => {
        if (!this.live(id) || !data.byteLength || !this.header) return;
        if (this.bytes + data.byteLength > 2 * 1024 * 1024 || this.queue.length >= 512) { this.fail(id, 0); return; }
        this.queue.push(data);
        this.bytes += data.byteLength;
        this.drain(id);
      });
      req.on('dataEnd', () => { if (this.live(id)) { this.ended = true; this.drain(id); } });
      req.requestInStream(this.target.address, { method: http.RequestMethod.GET,
        header: { 'Accept': 'video/x-flv', 'Authorization': 'Basic ' + this.target.token },
        maxRedirects: 0, usingProxy: false, usingCache: false, connectTimeout: 6000, readTimeout: 10000 })
        .then((code: number) => { if (code !== 200) this.fail(id, code); else if (this.ended && !this.header) this.fail(id, 0); })
        .catch((e: BusinessError) => this.fail(id, e.code === 2300047 ? 302 : 0));
    } catch (_) { this.fail(id, 0); }
  }

  private drain(id: number): void {
    const loading = this.loading;
    if (!loading || !this.header) return;
    try {
      while (this.wanted !== 0 && this.queue.length) {
        const data = this.queue[0];
        const count = this.wanted < 0 ? data.byteLength : Math.min(this.wanted, data.byteLength);
        const sent = loading.respondData(id, this.offset, count === data.byteLength ? data : data.slice(0, count));
        // -2: end of this read; -3: full. Wait for the player's next read callback.
        if (sent === -2 || sent === -3 || sent === 0) { this.wanted = 0; break; }
        if (!Number.isSafeInteger(sent) || sent < 0 || sent > count) { this.fail(id, 0); return; }
        this.offset += sent;
        this.bytes -= sent;
        if (this.wanted > 0) this.wanted -= sent;
        if (sent === data.byteLength) this.queue.shift();
        else this.queue[0] = data.slice(sent);
        if (sent < count) { this.wanted = 0; break; }
      }
      if (this.ended && !this.bytes) {
        this.close(id);
        loading.finishLoading(id, media.LoadingRequestError.LOADING_ERROR_SUCCESS);
      }
    } catch (_) { this.fail(id, 0); }
  }
}
