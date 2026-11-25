import { AndroidPreferenceValue } from './AndroidPreferenceValue';
import { CameraDeviceOption, CameraResolution, CaptureFpsInfo, CaptureFpsRange, EncoderOption,
  FocusInfo } from '../model/CameraCapabilities';

function rateLabel(range: CaptureFpsRange): string {
  return range.min === range.max ? `${range.min} FPS` : `${range.min}–${range.max} FPS`;
}

@Component
export struct CameraProfilePicker {
  @Prop encoderOnly: boolean = false;
  @Prop resolutions: CameraResolution[] = [];
  @Prop selectedWidth: number = 0;
  @Prop selectedHeight: number = 0;
  @Prop cameraRunning: boolean = false;
  @Prop fpsInfo: CaptureFpsInfo = { ranges: [] };
  @Prop devices: CameraDeviceOption[] = [];
  @Prop deviceId: string = '';
  @Prop encoders: EncoderOption[] = [];
  @Prop encoderName: string = '';
  @Prop avcProfile: number = -1;
  @Prop focusInfo: FocusInfo = { modes: [] };
  onDevice: (device: CameraDeviceOption) => void = () => {};
  onEncoder: (encoder: EncoderOption) => void = () => {};
  onAvcProfile: (profile: number) => void = () => {};
  onFocusMode: (mode: number) => void = () => {};
  onFocusCenter: () => void = () => {};
  onResolution: (size: CameraResolution) => void = () => {};
  onCaptureFps: (range: CaptureFpsRange) => void = () => {};

  private msg(resource: Resource): string {
    return getContext(this).resourceManager.getStringSync(resource.id);
  }

  private lensLabel(device: CameraDeviceOption): string {
    const facing = this.msg(device.front ? $r('app.string.cap_front') : $r('app.string.cap_rear'));
    return `${facing} · ${device.id}`;
  }

  private encoderLabel(encoder: EncoderOption): string {
    const kind = this.msg(encoder.hardware ? $r('app.string.cap_hardware') : $r('app.string.cap_software'));
    return `${kind} · ${encoder.name}`;
  }

  private profileLabel(profile: number): string {
    return profile === 0 ? 'Baseline' : (profile === 4 ? 'High' : (profile === 8 ? 'Main' : this.msg($r('app.string.cap_auto'))));
  }

  private focusLabel(mode: number | undefined): string {
    const labels: Resource[] = [$r('app.string.cap_focus_manual'), $r('app.string.cap_focus_continuous'),
      $r('app.string.cap_focus_single'), $r('app.string.cap_focus_locked')];
    return this.msg(mode === undefined ? $r('app.string.cap_choose_focus') : labels[mode] ?? $r('app.string.cap_choose_focus'));
  }

  private resolutionLabel(): string {
    const size = this.resolutions.find((r: CameraResolution) =>
      r.width === this.selectedWidth && r.height === this.selectedHeight);
    return size ? `${size.width} × ${size.height}` : this.msg($r('app.string.cap_choose_size'));
  }

  private sizeMenu(): MenuElement[] {
    return this.resolutions.map((r: CameraResolution): MenuElement => {
      return { value: `${r.width} × ${r.height}`, action: () => this.onResolution(r) };
    });
  }

  private rateMenu(): MenuElement[] {
    return this.fpsInfo.ranges.map((r: CaptureFpsRange): MenuElement => {
      return { value: rateLabel(r), action: () => this.onCaptureFps(r) };
    });
  }

  private lensMenu(): MenuElement[] {
    return this.devices.map((d: CameraDeviceOption): MenuElement => {
      return { value: this.lensLabel(d), action: () => this.onDevice(d) };
    });
  }

  private selectedLens(): string {
    const d = this.devices.find((v: CameraDeviceOption) => v.id === this.deviceId);
    return d ? this.lensLabel(d) : this.msg($r('app.string.cap_choose_lens'));
  }

  private selectedEncoder(): EncoderOption | undefined {
    return this.encoders.find((e: EncoderOption) => e.name === this.encoderName);
  }

  private encoderText(): string {
    const e = this.selectedEncoder();
    return e ? this.encoderLabel(e) : this.msg($r('app.string.cap_auto'));
  }

  private encoderMenu(): MenuElement[] {
    const automatic: EncoderOption = { name: '', hardware: false, profiles: [], minBitrate: 0, maxBitrate: 0 };
    const menu: MenuElement[] = [{ value: this.msg($r('app.string.cap_auto')), action: () => this.onEncoder(automatic) }];
    this.encoders.forEach((e: EncoderOption) => {
      menu.push({ value: this.encoderLabel(e), action: () => this.onEncoder(e) });
    });
    return menu;
  }

  private profiles(): number[] {
    const e = this.selectedEncoder();
    return e ? e.profiles : [];
  }

  private profileMenu(): MenuElement[] {
    const menu: MenuElement[] = [{ value: this.msg($r('app.string.cap_auto')), action: () => this.onAvcProfile(-1) }];
    this.profiles().forEach((p: number) => {
      menu.push({ value: this.profileLabel(p), action: () => this.onAvcProfile(p) });
    });
    return menu;
  }

  private focusMenu(): MenuElement[] {
    return this.focusInfo.modes.map((m: number): MenuElement => {
      return { value: this.focusLabel(m), action: () => this.onFocusMode(m) };
    });
  }

  build() {
    Column() {
      if (this.encoderOnly) {
        if (this.encoders.length > 0) {
          AndroidPreferenceValue({ title: $r('app.string.cap_encoder'), summary: this.encoderText(), available: !this.cameraRunning })
            .enabled(!this.cameraRunning).bindMenu(this.encoderMenu())
        }
        if (this.profiles().length > 0) {
          AndroidPreferenceValue({ title: 'H.264 Profile', summary: this.profileLabel(this.avcProfile), available: !this.cameraRunning })
            .enabled(!this.cameraRunning).bindMenu(this.profileMenu())
        }
      } else {
        if (this.devices.length > 0) {
          AndroidPreferenceValue({ title: $r('app.string.cap_lens'), summary: this.selectedLens(), available: !this.cameraRunning })
            .enabled(!this.cameraRunning).bindMenu(this.lensMenu())
        }
        if (this.resolutions.length > 0) {
          AndroidPreferenceValue({ title: $r('app.string.cap_size'), summary: this.resolutionLabel(), available: !this.cameraRunning })
            .enabled(!this.cameraRunning).bindMenu(this.sizeMenu())
        } else {
          AndroidPreferenceValue({ title: $r('app.string.cap_size'), summary: $r('app.string.cap_no_sizes'), available: false })
        }
        if (this.cameraRunning && this.focusInfo.modes.length > 0) {
          AndroidPreferenceValue({ title: $r('app.string.cap_focus'), summary: this.focusLabel(this.focusInfo.active) })
            .bindMenu(this.focusMenu())
          if (this.focusInfo.active === 2 && this.focusInfo.modes.includes(2)) {
            AndroidPreferenceValue({ title: $r('app.string.cap_focus_center') }).onClick(() => this.onFocusCenter())
          }
        }
        if (this.cameraRunning && this.fpsInfo.ranges.length > 0) {
          AndroidPreferenceValue({ title: $r('app.string.cap_fps'),
            summary: this.fpsInfo.active ? rateLabel(this.fpsInfo.active) : $r('app.string.cap_choose_fps') })
            .bindMenu(this.rateMenu())
        } else if (this.cameraRunning && this.fpsInfo.active) {
          AndroidPreferenceValue({ title: $r('app.string.cap_fps'), available: false,
            summary: `${rateLabel(this.fpsInfo.active)} · ${this.msg($r('app.string.cap_fps_readonly'))}` })
        }
      }
    }.width('100%')
  }
}
