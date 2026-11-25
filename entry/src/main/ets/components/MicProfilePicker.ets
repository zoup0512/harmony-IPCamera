import { AndroidPreferenceValue } from './AndroidPreferenceValue';

// The preference and the actual read-back are intentionally separate: a fallback
// must not silently overwrite the user's preferred format for the next start.
@Component
export struct MicProfilePicker {
  @Prop sampleRate: number = 0;
  @Prop channels: number = 1;
  @Prop running: boolean = false;
  @Prop actualRate: number = 0;
  @Prop actualChannels: number = 0;
  onSampleRate: (rate: number) => void = () => {};
  onChannels: (count: number) => void = () => {};

  private rateLabel(rate: number): ResourceStr {
    return rate === 0 ? $r('app.string.mic_rate_auto') : `${rate} Hz`;
  }

  private channelLabel(count: number): ResourceStr {
    return count === 2 ? $r('app.string.mic_stereo') : $r('app.string.mic_mono');
  }

  private ratesMenu(): MenuElement[] {
    return [0, 8000, 12000, 16000, 22050, 24000, 32000, 44100, 48000]
      .map((rate: number): MenuElement => {
        return { value: this.rateLabel(rate), action: () => this.onSampleRate(rate) };
      });
  }

  private channelsMenu(): MenuElement[] {
    return [1, 2].map((count: number): MenuElement => {
      return { value: this.channelLabel(count), action: () => this.onChannels(count) };
    });
  }

  build() {
    Column() {
      AndroidPreferenceValue({ title: $r('app.string.ui_capture_rate'), summary: this.rateLabel(this.sampleRate), available: !this.running })
        .enabled(!this.running).bindMenu(this.ratesMenu())
      AndroidPreferenceValue({ title: $r('app.string.ui_capture_channels'), summary: this.channelLabel(this.channels), available: !this.running })
        .enabled(!this.running).bindMenu(this.channelsMenu())
      if (this.running && this.actualRate > 0 && this.actualChannels > 0) {
        AndroidPreferenceValue({ title: $r('app.string.mic_actual_format'),
          summary: $r('app.string.ui_capture_readback', this.actualRate,
            this.getUIContext().getHostContext()!.resourceManager.getStringSync(
              (this.actualChannels === 2 ? $r('app.string.mic_stereo') : $r('app.string.mic_mono')).id)) })
      }
    }.width('100%')
  }
}
