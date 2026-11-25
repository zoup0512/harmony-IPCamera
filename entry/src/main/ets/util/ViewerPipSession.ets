import { common } from '@kit.AbilityKit';
import { PiPWindow } from '@kit.ArkUI';

// Only construct this for a decoder presenting video to this exact SURFACE XComponent.
// Capture the playback epoch and decoder-bound ID, not a URL, format, or `active` flag.
export interface ViewerPipSurface {
  controller: XComponentController;
  surfaceId: string;
  isRendering: () => boolean;
}

export function viewerPipEligible(surface?: ViewerPipSurface): boolean {
  try {
    return !!surface && !!surface.surfaceId && surface.surfaceId !== '0' && surface.isRendering() &&
      surface.controller.getXComponentSurfaceId() === surface.surfaceId;
  } catch (_) { return false; } // Surface has not loaded or has already been destroyed.
}

export type ViewerPipPhase = 'idle' | 'starting' | 'active' | 'stopping' | 'restoring' | 'stopFailed';
export type ViewerPipEnd = '' | 'stopped' | 'restored' | 'closed' | 'interrupted' | 'unavailable' | 'surfaceLost' | 'error';

class PipAttempt {
  controller?: PiPWindow.PiPController;
  starting: Promise<void> = Promise.resolve();
  stopping?: Promise<void>;
  cancelled: boolean = false;
  requested: boolean = false;
  started: boolean = false;
  restoring: boolean = false;
  end: ViewerPipEnd = 'stopped';
}

// Owner: one Player page. Only call start from an explicit click. Keep the source node mounted;
// while keepsSurface() is true a background transition must not release its decoder. On explicit
// navigation, stop, surface destruction or source replacement call stop() BEFORE release/rebind.
// On actual page destruction use dispose(). Never interpret resolving start() as the UI state.
export class ViewerPipSession {
  private current?: PipAttempt;
  private disposed: boolean = false;
  private changed: (phase: ViewerPipPhase, end: ViewerPipEnd) => void;

  constructor(changed: (phase: ViewerPipPhase, end: ViewerPipEnd) => void) { this.changed = changed; }

  keepsSurface(): boolean { return this.current?.requested === true; }

  start(ctx: common.UIAbilityContext, surface?: ViewerPipSurface): Promise<void> {
    if (this.disposed || this.current) return Promise.resolve();
    if (!surface || !viewerPipEligible(surface)) {
      this.changed('idle', 'surfaceLost');
      return Promise.resolve();
    }
    const a = new PipAttempt();
    this.current = a;
    a.starting = Promise.resolve().then(() => this.open(a, ctx, surface));
    this.changed('starting', '');
    return a.starting;
  }

  private async open(a: PipAttempt, ctx: common.UIAbilityContext, surface: ViewerPipSurface): Promise<void> {
    if (a.cancelled) return;
    try {
      if (!canIUse('SystemCapability.Window.SessionManager') ||
        !canIUse('SystemCapability.WindowManager.WindowManager.Core') || !PiPWindow.isPiPEnabled() ||
        !ctx.windowStage.getMainWindowSync().isWindowShowing()) {
        this.finish(a, 'unavailable');
        return;
      }
      // The public XComponent overload transfers the existing render surface; no second decoder
      // or replacement node. VIDEO_LIVE with no optional groups has native restore/close only.
      const c = await PiPWindow.create({ context: ctx, componentController: surface.controller,
        templateType: PiPWindow.PiPTemplateType.VIDEO_LIVE, controlGroups: [] });
      a.controller = c;
      if (a.cancelled) return;
      if (!viewerPipEligible(surface)) { this.finish(a, 'surfaceLost'); return; }
      if (!ctx.windowStage.getMainWindowSync().isWindowShowing()) { this.finish(a, 'unavailable'); return; }
      c.on('stateChange', (state: PiPWindow.PiPState, reason: string) => this.onState(a, state, reason));
      a.requested = true;
      await c.startPiP(); // Auto-start stays at the SDK default (false).
      // The native STARTED event, not create(), a settings switch, or this promise, owns `active`.
    } catch (_) {
      if (!a.started) a.requested = false;
      if (a.cancelled || this.current !== a) return;
      // If STARTED already arrived, unwind the actual window before reporting failure.
      if (a.started) { a.end = 'error'; this.stop(); return; }
      this.finish(a, 'unavailable');
    }
  }

  private onState(a: PipAttempt, state: PiPWindow.PiPState, reason: string): void {
    if (this.current !== a) return;
    if (state === PiPWindow.PiPState.STARTED) {
      a.started = true;
      if (!a.cancelled) this.changed('active', '');
    } else if (state === PiPWindow.PiPState.ABOUT_TO_RESTORE) {
      a.restoring = true;
      if (!a.cancelled) this.changed('restoring', '');
    } else if (state === PiPWindow.PiPState.ABOUT_TO_STOP) {
      if (!a.cancelled) this.changed(a.restoring ? 'restoring' : 'stopping', '');
    } else if (state === PiPWindow.PiPState.STOPPED) {
      const closed = reason === 'dragDelete' || reason === 'panelActionDelete';
      this.finish(a, a.cancelled ? a.end : a.restoring || reason === 'panelActionRestore' ? 'restored' :
        closed ? 'closed' : 'interrupted');
    } else if (state === PiPWindow.PiPState.ERROR && !a.cancelled) {
      // An ERROR can race the in-flight native start; don't abandon a controller that
      // might still complete. stop() waits for that request before deciding whether to close it.
      a.end = 'error';
      this.stop();
    }
  }

  private finish(a: PipAttempt, end: ViewerPipEnd): void {
    if (this.current !== a) return;
    this.current = undefined;
    try { a.controller?.off('stateChange'); } catch (_) {}
    a.controller = undefined;
    if (!this.disposed) this.changed('idle', end);
  }

  stop(): Promise<void> {
    const a = this.current;
    if (!a) return Promise.resolve();
    if (a.stopping) return a.stopping;
    a.cancelled = true;
    a.stopping = a.starting.then(async () => {
      if (this.current !== a) return;
      try {
        if (a.requested) await a.controller?.stopPiP();
        this.finish(a, a.end);
      } catch (_) {
        if (this.current !== a) return;
        // Do not report restoration on a failed stop. Allow retry or a later native STOPPED.
        a.stopping = undefined;
        if (this.disposed) this.finish(a, 'error');
        else this.changed('stopFailed', 'error');
      }
    });
    if (!this.disposed) this.changed('stopping', '');
    return a.stopping;
  }

  async dispose(): Promise<void> {
    this.disposed = true;
    await this.stop();
  }
}
