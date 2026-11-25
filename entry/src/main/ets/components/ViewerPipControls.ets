import { ViewerPipEnd, ViewerPipPhase } from '../util/ViewerPipSession';

// Place alongside (not instead of) the permanent XComponent. Bind phase/end to the session's
// change callback. Use the page's in-place fullscreen layout; never clone or recycle the node.
@Component
export struct ViewerPipControls {
  @Prop eligible: boolean = false;
  @Prop playing: boolean = false;
  @Prop fullscreen: boolean = false;
  @Prop phase: ViewerPipPhase = 'idle';
  @Prop end: ViewerPipEnd = '';
  onStart: () => void = () => {};
  onStop: () => void = () => {};
  onFullscreen: (enable: boolean) => void = () => {};

  build() {
    Column({ space: 4 }) {
      Row({ space: 8 }) {
        if (this.phase === 'starting' || this.phase === 'active' || this.phase === 'stopFailed') {
          Button($r('app.string.pip_exit')).onClick(() => this.onStop())
        } else if (this.eligible && this.phase === 'idle') {
          Button($r('app.string.pip_enter')).onClick(() => this.onStart())
        }
        Button(this.fullscreen ? $r('app.string.viewer_exit_full') : $r('app.string.viewer_fullscreen'))
          .enabled(this.phase === 'idle' && (this.playing || this.fullscreen))
          .onClick(() => this.onFullscreen(!this.fullscreen))
      }
      if (this.phase === 'active') {
        Text($r('app.string.pip_external'))
      } else if (this.phase === 'stopFailed') {
        Text($r('app.string.pip_stop_failed'))
      } else if (this.phase !== 'idle') {
        Text($r('app.string.pip_transition'))
      } else if (this.end === 'unavailable' || this.end === 'error') {
        Text($r('app.string.pip_unavailable'))
      } else if (this.end === 'surfaceLost') {
        Text($r('app.string.pip_surface_lost'))
      } else if (this.end === 'interrupted') {
        Text($r('app.string.pip_interrupted'))
      }
    }
  }
}
