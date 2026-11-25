import { common } from '@kit.AbilityKit';
import { mdns } from '@kit.NetworkKit';

// One serial queue per listener. Nothing here opens sockets, discovers peers or starts a server.
export class ServiceAnnouncement {
  private readonly ctx: common.Context;
  private readonly type: string;
  private optedIn: boolean = false;
  private boundPort: number = 0;
  private publishedPort: number = 0;
  private generation: number = 0;
  private publishedGeneration: number = 0;
  private record?: mdns.LocalServiceInfo;
  private pending: Promise<void> = Promise.resolve();
  private error: string = '';
  private readonly observers: Set<() => void> = new Set();

  constructor(ctx: common.Context, http: boolean) {
    this.ctx = ctx;
    this.type = http ? '_http._tcp' : '_rtsp._tcp';
  }

  get enabled(): boolean { return this.optedIn; }
  get failed(): boolean { return this.error !== ''; }
  get status(): string {
    if (this.error === 'withdraw') return this.ctx.resourceManager.getStringSync($r('app.string.addon_lan_withdraw_failed').id, this.publishedPort);
    if (this.error === 'register') return this.ctx.resourceManager.getStringSync($r('app.string.addon_lan_registration_failed').id);
    if (this.record && this.publishedGeneration !== this.generation) return this.ctx.resourceManager.getStringSync($r('app.string.addon_lan_withdrawing').id, this.publishedPort);
    if (this.record) return this.ctx.resourceManager.getStringSync($r('app.string.addon_lan_published').id, this.publishedPort);
    if (!this.optedIn) return this.ctx.resourceManager.getStringSync($r('app.string.addon_lan_off').id);
    if (!this.boundPort) return this.ctx.resourceManager.getStringSync($r('app.string.addon_lan_waiting').id);
    return this.ctx.resourceManager.getStringSync($r('app.string.addon_lan_publishing').id, this.boundPort);
  }

  observe(fn: () => void): void { this.observers.add(fn); }
  unobserve(fn: () => void): void { this.observers.delete(fn); }
  private notify(): void {
    this.observers.forEach((fn: () => void) => {
      // A disappearing view must not break the queue that owns SDK cleanup.
      try { fn(); } catch (_) {}
    });
  }
  private target(): number { return this.optedIn ? this.boundPort : 0; }

  setEnabled(on: boolean): Promise<void> {
    if (this.optedIn !== on) this.generation++;
    this.optedIn = on;
    return this.retry();
  }

  // Service owner only. Pass the concrete port of its successful bind, never a draft/preference,
  // URL or remote endpoint. Zero means stopped/unknown; port zero is NOT advertised as ephemeral.
  setBoundPort(port: number): Promise<void> {
    const next = Number.isInteger(port) && port > 0 && port <= 65535 ? port : 0;
    if (next !== this.boundPort) this.generation++;
    this.boundPort = next;
    return this.retry();
  }

  retry(): Promise<void> {
    this.pending = this.pending.then(() => this.reconcile());
    this.notify();
    return this.pending;
  }

  private async reconcile(): Promise<void> {
    if (this.record && this.publishedGeneration !== this.generation) {
      try {
        // The SDK may rename the instance. Remove the returned record using the original context.
        await mdns.removeLocalService(this.ctx, this.record);
      } catch (_) {
        this.error = 'withdraw';
        this.notify();
        return; // Keep the handle; never stack a replacement on a failed withdrawal.
      }
      this.record = undefined;
      this.publishedPort = 0;
    }
    const port = this.target(); // Re-read after async withdrawal: stop/disable may have won.
    this.error = '';
    if (!this.record && port !== 0) {
      const info: mdns.LocalServiceInfo = { serviceType: this.type, serviceName: 'IPCamera', port: port };
      const generation = this.generation;
      try {
        this.record = await mdns.addLocalService(this.ctx, info);
        this.publishedGeneration = generation;
        this.publishedPort = port;
      } catch (_) {
        this.error = 'register';
      }
    }
    this.notify(); // Changes during add are already queued, including compensating removal.
  }
}

export class LanServiceAnnouncements {
  readonly http: ServiceAnnouncement;
  readonly rtsp: ServiceAnnouncement;

  constructor(ctx: common.Context) {
    this.http = new ServiceAnnouncement(ctx, true);
    this.rtsp = new ServiceAnnouncement(ctx, false);
  }

  async stopAll(): Promise<boolean> {
    await Promise.all([this.http.setBoundPort(0), this.rtsp.setBoundPort(0)]);
    return !this.http.failed && !this.rtsp.failed;
  }
}

// Shared by the listener owner and settings page; do not retain a page/Ability context.
let shared: LanServiceAnnouncements | undefined;
export function lanServiceAnnouncements(ctx: common.Context): LanServiceAnnouncements {
  if (!shared) shared = new LanServiceAnnouncements(ctx.getApplicationContext());
  return shared;
}
