export interface ThumbnailBox { crop?: string; cls?: string; conf?: number; baseline?: boolean }
// Only accept relative saved-crop paths for this camera, never arbitrary URLs.
export function activityCrop(cameraId: number, frames: Array<{ boxes?: ThumbnailBox[] }>): string | null {
  const candidates = frames.flatMap(frame => Array.isArray(frame.boxes) ? frame.boxes : [])
    .map(box => ({ ...box, crop: typeof box.crop === 'string' ? box.crop.replace(/\\/g, '/').split(/\/crops\//i).pop() : undefined }))
    .filter(box => !box.baseline && Number.isFinite(box.conf) && box.conf! >= 0.45 &&
      typeof box.crop === 'string' && box.crop.startsWith(`${cameraId}/`) &&
      !box.crop.includes('..') && !/[\\?#:]/.test(box.crop))
    .sort((a, b) => b.conf! - a.conf!)
  const crop = candidates[0]?.crop
  return crop ? `/api/detection/crop/${crop.split('/').map(encodeURIComponent).join('/')}` : null
}
