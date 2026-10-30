import { RtspServer } from './RtspServer';

// App-wide streaming hub, mirroring Android IPCameraServer being reachable
// from any activity. The HTTP layer always runs while the camera streams;
// the RTSP listener and RTMP publisher are optional outputs.
export class AppServer {
  private static inner: RtspServer | null = null;
  static cameraRunning: boolean = false;
  static micRunning: boolean = false;
  static rtspOn: boolean = false;
  static rtmpOn: boolean = false;
  static recording: boolean = false;
  static torchOn: boolean = false;
  static frontCamera: boolean = false;
  static motionEnabled: boolean = false;
  static streamWidth: number = 1280;
  static streamHeight: number = 720;
  static mime: string = 'video/avc';

  static server(): RtspServer {
    if (AppServer.inner === null) {
      AppServer.inner = new RtspServer();
    }
    return AppServer.inner;
  }
}
