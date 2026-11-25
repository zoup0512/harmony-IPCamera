import { common } from '@kit.AbilityKit';
import { camera } from '@kit.CameraKit';

export interface DualCameraPlan {
  primaryId: string;
  secondaryId: string;
  primaryType: number;
  secondaryType: number;
  width: number;
  height: number;
  secondaryWidth: number;
  secondaryHeight: number;
}

function usable(p: camera.Profile): boolean {
  const w = p.size.width;
  const h = p.size.height;
  return p.format === camera.CameraFormat.CAMERA_FORMAT_YUV_420_SP && Number.isInteger(w) &&
    Number.isInteger(h) && w >= 4 && h >= 4 && w % 2 === 0 && h % 2 === 0 &&
    w <= 4096 && h <= 4096 && w * h <= 4096 * 2160;
}

function concurrentPreviews(info: camera.CameraConcurrentInfo): camera.Profile[] {
  if (info.type !== camera.CameraConcurrentType.CAMERA_FULL_CAPABILITY &&
    info.type !== camera.CameraConcurrentType.CAMERA_LIMITED_CAPABILITY) return [];
  const i = info.modes.indexOf(camera.SceneMode.NORMAL_VIDEO);
  if (i < 0 || info.modes.length !== info.outputCapabilities.length) return [];
  return info.outputCapabilities[i].previewProfiles.filter((p: camera.Profile) => usable(p));
}

// Query only. Does not open a camera, interrupt the current stream or claim that
// the sessions will start. A plan exists only for this pair and primary geometry.
export function queryDualCamera(context: common.Context, front: boolean, width: number,
  height: number): DualCameraPlan | undefined {
  try {
    const manager = camera.getCameraManager(context);
    const back = manager.getCameraDevice(camera.CameraPosition.CAMERA_POSITION_BACK, camera.CameraType.CAMERA_TYPE_DEFAULT);
    const selfie = manager.getCameraDevice(camera.CameraPosition.CAMERA_POSITION_FRONT, camera.CameraType.CAMERA_TYPE_DEFAULT);
    const primary = front ? selfie : back;
    const secondary = front ? back : selfie;
    if (!primary.cameraId || !secondary.cameraId || primary.cameraId === secondary.cameraId) return undefined;
    const infos = manager.getCameraConcurrentInfos([primary, secondary]);
    if (infos.length !== 2) return undefined;
    const a = infos.find((info: camera.CameraConcurrentInfo) => info.device.cameraId === primary.cameraId);
    const b = infos.find((info: camera.CameraConcurrentInfo) => info.device.cameraId === secondary.cameraId);
    if (!a || !b || !concurrentPreviews(a).some((p: camera.Profile) => p.size.width === width && p.size.height === height)) {
      return undefined;
    }
    // Prefer VGA pixel count for a CPU-composited inset, but use only a size
    // the second device advertises for this specific concurrent mode.
    const options = concurrentPreviews(b);
    if (options.length === 0) return undefined;
    const target = Math.min(width * height, 640 * 480);
    options.sort((x: camera.Profile, y: camera.Profile) =>
      Math.abs(x.size.width * x.size.height - target) - Math.abs(y.size.width * y.size.height - target));
    return { primaryId: primary.cameraId, secondaryId: secondary.cameraId, primaryType: a.type,
      secondaryType: b.type, width: width, height: height, secondaryWidth: options[0].size.width,
      secondaryHeight: options[0].size.height };
  } catch (_) {
    return undefined;
  }
}
