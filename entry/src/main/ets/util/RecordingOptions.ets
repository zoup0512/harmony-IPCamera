// By default the recorder muxes the shared encoded stream. Optional video
// overrides are forwarded to the host; they require a separate encoder there.
export interface RecordingSettings {
  includeAudio: boolean;
  fourGbLimit: boolean;
  segmentMinutes: number;
  videoMime?: string;
  videoBitrate?: number;
  gopMs?: number;
}

export interface RecordingControls {
  micRunning(): boolean;
  // Change whenever mic ownership changes (including an explicit user takeover).
  micGeneration(): number;
  // Synchronous, already permission-checked. The host also owns UI/keep-alive updates.
  startMic(): boolean;
  stopMic(): void;
  startRecord(path: string, options: RecordingSettings): boolean;
  stopRecord(): void;
}

// Use one instance across manual/motion. Reserve BEFORE awaiting permission;
// stop(ticket) cancels pending starts, closes the owned writer and releases only
// a microphone started by this recording. Pass the same ticket on fatal/teardown.
export class RecordingOwnership {
  private sequence: number = 0;
  private ticket: number = 0;
  private recording: boolean = false;
  private ownedMic: number = -1;
  private controls: RecordingControls;

  constructor(controls: RecordingControls) { this.controls = controls; }

  reserve(): number {
    if (this.ticket !== 0) return 0;
    this.ticket = ++this.sequence;
    return this.ticket;
  }

  start(ticket: number, path: string, options: RecordingSettings): boolean {
    if (ticket === 0 || ticket !== this.ticket || this.recording) return false;
    try {
      if (!Number.isSafeInteger(options.segmentMinutes) || options.segmentMinutes < 0 ||
          options.segmentMinutes > 2147483647) return false;
      if (options.includeAudio && !this.controls.micRunning()) {
        if (!this.controls.startMic()) return false;
        this.ownedMic = this.controls.micGeneration();
      }
      this.recording = this.controls.startRecord(path, options);
      return this.recording;
    } finally {
      if (!this.recording) this.stop(ticket);
    }
  }

  stop(ticket: number): boolean {
    if (ticket === 0 || ticket !== this.ticket) return false;
    try {
      if (this.recording) this.controls.stopRecord();
    } finally {
      try {
        if (this.ownedMic >= 0 && this.controls.micGeneration() === this.ownedMic &&
            this.controls.micRunning()) this.controls.stopMic();
      } finally {
        this.ticket = 0;
        this.ownedMic = -1;
        this.recording = false;
      }
    }
    return true;
  }
}
