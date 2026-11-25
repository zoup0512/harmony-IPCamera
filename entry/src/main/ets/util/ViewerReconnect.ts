// Foreground MJPEG/RTSP views only. No transport, credentials, UI or persisted state.
export type ViewerFailure = 'disconnect' | 'fatal';

// Allowlist structured errors, never infer retryability from a display message.
export function rtspViewerFailure(status: number): ViewerFailure {
  return status === -1 || status === -7 ? 'disconnect' : 'fatal';
}

export function httpViewerFailure(status: number): ViewerFailure {
  return status === 408 || status === 502 || status === 503 || status === 504 ? 'disconnect' : 'fatal';
}

export function httpViewerRequestFailure(code: number): ViewerFailure {
  switch (code) {
    case 2300006: case 2300007: case 2300018: case 2300028:
    case 2300052: case 2300055: case 2300056: return 'disconnect';
    default: return 'fatal'; // Includes TLS, redirects, unsupported scheme and unknown errors.
  }
}

export interface ViewerClock {
  after(task: () => void, ms: number): number;
  cancel(id: number): void;
}

const realClock: ViewerClock = {
  after: (task: () => void, ms: number): number => setTimeout(task, ms),
  cancel: (id: number): void => clearTimeout(id)
};
const delays: number[] = [1000, 2000, 4000];

export class ViewerReconnect {
  private clock: ViewerClock;
  private visible: boolean = false;
  private optedIn: boolean = false;
  private established: boolean = false;
  private spent: number = 0;
  private token: number = 0;
  private timer?: number;
  private open?: (attempt: number) => void;

  constructor(clock: ViewerClock = realClock) { this.clock = clock; }

  setForeground(visible: boolean): void {
    this.visible = visible;
    if (!visible) this.cancel(); // Showing the page again never resumes a cancelled intent.
  }

  // Called once per manual Play/Retry on an already selected endpoint (not per reconnect).
  start(open: (attempt: number) => void, enabled: boolean = false): void {
    this.cancel();
    if (!this.visible) return;
    this.open = open;
    this.optedIn = enabled;
    this.established = false;
    this.spent = 0;
    open(this.token);
  }

  setEnabled(enabled: boolean): void {
    this.optedIn = enabled;
    if (!enabled && this.timer !== undefined) this.cancel();
  }

  current(attempt: number): boolean {
    return this.open !== undefined && attempt === this.token && this.timer === undefined;
  }

  // Only after the first decoded MJPEG frame / native RTSP Playing. Never refills the budget.
  playing(attempt: number): void {
    if (this.current(attempt)) this.established = true;
  }

  // Returns the newly scheduled delay, or 0 if no new timer. Caller releases its transport.
  failed(attempt: number, reason: ViewerFailure = 'fatal'): number {
    if (attempt !== this.token || !this.open) return 0;
    if (reason !== 'disconnect' || !this.optedIn || !this.established) {
      this.cancel(); // A confirmed fatal failure also vetoes a queued retry for this attempt.
      return 0;
    }
    if (this.timer !== undefined) return 0;
    if (this.spent === delays.length) { this.cancel(); return 0; }
    const delay = delays[this.spent++];
    this.timer = this.clock.after(() => {
      if (attempt !== this.token || !this.visible || !this.open) return;
      this.timer = undefined;
      this.open(++this.token);
    }, delay);
    return delay;
  }

  // User stop, re-selection, address/credential edit, disable, or departure. Also invalidates
  // a callback already dequeued by the event loop; clearTimeout alone is not sufficient.
  cancel(): void {
    ++this.token;
    this.open = undefined;
    if (this.timer !== undefined) this.clock.cancel(this.timer);
    this.timer = undefined;
  }
}
