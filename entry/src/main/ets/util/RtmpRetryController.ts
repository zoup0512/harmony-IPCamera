import { canPublish } from './RtmpProfiles';

export type RtmpPhase = 'idle' | 'connecting' | 'publishing' | 'waiting';

export interface RtmpRetryPort {
  start: (url: string) => Promise<boolean>;
  stop: () => void;
  // Deliberately excludes both the destination and free-form native error text.
  changed: (phase: RtmpPhase, retry: number, delayMs: number) => void;
}

export interface RtmpRetryClock {
  after: (task: () => void, delayMs: number) => number;
  cancel: (id: number) => void;
}

const systemClock: RtmpRetryClock = {
  after: (task: () => void, ms: number): number => setTimeout(task, ms),
  cancel: (id: number): void => clearTimeout(id)
};
const delays: number[] = [1000, 2000, 4000, 8000];

// Allow only transport failures actually emitted by RtmpPublisher. Rejections,
// unsupported handshake/URL, cancellation and unrecognised errors fail closed.
function isTransportFailure(detail: string): boolean {
  const cause = detail.replace(/^rtmp:\s*/, '').replace(/^(startup failed|connection lost): /, '');
  if (/^(DNS resolver busy|DNS lookup timed out|operation timed out|socket closed while sending|server closed connection( while reading)?)$/.test(cause)) return true;
  if (/^DNS lookup failed: (Try again|Temporary failure in name resolution)$/.test(cause)) return true;
  // These fixed reasons are emitted after authentication. Ambiguous handshake,
  // verification, protocol or bridge errors must never initiate another attempt.
  if (cause === 'RTMPS connection closed' || cause === 'RTMPS write timed out' || cause === 'RTMPS write failed') return true;
  // A generic "connect failed" can also describe TLS/authentication failures.
  // Only retry known socket errors, never arbitrary SDK or peer-supplied text.
  return /^(socket failed|socket wait failed|connect failed|getsockopt failed|poll failed|server poll failed|send failed|recv failed|server recv failed): (Connection refused|Connection reset by peer|Connection reset|Connection timed out|Operation timed out|Network is unreachable|Network unreachable|Network is down|Network dropped connection on reset|Host is unreachable|Host is down|No route to host|Software caused connection abort|Connection aborted|Broken pipe)$/.test(cause);
}

export class RtmpRetryController {
  private port: RtmpRetryPort;
  private clock: RtmpRetryClock;
  private phase: RtmpPhase = 'idle';
  private destination: string = '';
  private generation: number = 0;
  private retry: number = 0;
  private timer: number | undefined = undefined;
  private inFlight: boolean = false;
  private earlyDisconnect: boolean = false;

  constructor(port: RtmpRetryPort, clock: RtmpRetryClock = systemClock) {
    this.port = port;
    this.clock = clock;
  }

  start(address: string): void {
    const url = address.trim();
    if (this.phase !== 'idle' && url === this.destination) return;
    this.stop();
    if (!canPublish(url)) return;
    this.destination = url;
    this.begin();
  }

  stop(): void {
    const before = this.phase;
    ++this.generation;
    this.phase = 'idle';
    this.destination = '';
    this.retry = 0;
    this.earlyDisconnect = false;
    if (this.timer !== undefined) this.clock.cancel(this.timer);
    this.timer = undefined;
    // A waiting attempt has already released its publisher.
    if (before === 'publishing' || before === 'connecting') this.port.stop();
    if (before !== 'idle') this.port.changed('idle', 0, 0);
  }

  // The TSFN and the durable failure snapshot must both belong to the current
  // native generation. A connecting failure can also reject the start promise.
  nativeError(detail: string): void {
    if (this.phase === 'idle' || !detail) return;
    if (!isTransportFailure(detail)) {
      this.stop();
    } else if (this.phase === 'connecting') {
      this.earlyDisconnect = true;
    } else if (this.phase === 'publishing') {
      this.backoff();
    }
  }

  private begin(): void {
    this.phase = 'connecting';
    this.port.changed('connecting', this.retry, 0);
    // stop() invalidates a pending async start but its worker may still be
    // unwinding. Only that worker's completion releases the single-flight gate.
    if (!this.inFlight && this.phase === 'connecting') this.connect();
  }

  private async connect(): Promise<void> {
    const generation = this.generation;
    this.inFlight = true;
    let ok = false;
    let transient = false;
    try {
      ok = await this.port.start(this.destination);
    } catch (err) {
      const message = err instanceof Error ? err.message : '';
      transient = isTransportFailure(message);
    }
    this.inFlight = false;
    if (generation !== this.generation) {
      if (this.phase === 'connecting') this.connect();
      return;
    }
    if (transient || (ok && this.earlyDisconnect)) {
      this.backoff();
    } else if (ok) {
      this.phase = 'publishing';
      this.port.changed('publishing', this.retry, 0);
    } else {
      // NAPI resolves false for cancelled/superseded work; it rejects on failure.
      this.stop();
    }
  }

  private backoff(): void {
    if (this.retry >= delays.length) {
      this.stop();
      return;
    }
    const generation = this.generation;
    const delayMs = delays[this.retry++];
    const retry = this.retry;
    this.phase = 'waiting';
    this.earlyDisconnect = false;
    this.port.stop();
    this.timer = this.clock.after(() => {
      if (this.generation !== generation || this.phase !== 'waiting' || this.retry !== retry) return;
      this.timer = undefined;
      this.begin();
    }, delayMs);
    // Do not reset the budget on a short-lived successful connection. Only a
    // new manual session can refill it, so a flapping server cannot loop forever.
    this.port.changed('waiting', retry, delayMs);
  }
}
