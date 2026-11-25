// One recorder-open attempt per observed motion episode. A held native motion
// level is not a new detection or permission to retry a failed writer.
export class MotionRecordingGate {
  private consumed: boolean = false;

  // Claim before any fallible work (mkdir, recorder reservation or file open).
  // Never defer an attempt seen while hidden until the page returns.
  claim(pageVisible: boolean): boolean {
    if (this.consumed) return false;
    this.consumed = true;
    return pageVisible;
  }

  // Only after native motion clears or the user explicitly arms a new session;
  // never call as a consequence of a writer stopping or an error notification.
  rearm(): void {
    this.consumed = false;
  }
}
