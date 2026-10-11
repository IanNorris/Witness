import type { Camera } from '../types/camera'
export type StreamChoice = 'auto' | 'main' | 'sub'
export interface StreamInfo { tier?: string; codec?: string; width?: number; height?: number; fps?: number }
export function codecLabel(codec?: string): string {
  const value = codec?.toLowerCase() ?? ''
  if (/^(hevc|h265|hvc1|hev1)/.test(value)) return 'H.265 / HEVC'
  if (/^(h264|avc1|avc3)/.test(value)) return 'H.264 / AVC'
  if (/^(vp09|vp9)/.test(value)) return 'VP9'
  if (/^(av01|av1)/.test(value)) return 'AV1'
  if (/^(mp4v|mpeg4)/.test(value)) return 'MPEG-4'
  if (/^(mjpeg|jpeg)/.test(value)) return 'MJPEG'
  return codec || 'Codec not reported'
}
export function isH264(codec?: string) { return codecLabel(codec) === 'H.264 / AVC' }
export function cameraStreamInfo(camera: Camera, tier: 'main' | 'sub'): StreamInfo {
  return tier === 'sub' ? { tier, codec: camera.subCodec, width: camera.subWidth, height: camera.subHeight } :
    { tier, codec: camera.codec, width: camera.width, height: camera.height }
}
export function streamSummary(info: StreamInfo): string {
  return [info.tier === 'sub' ? 'Sub' : info.tier === 'main' ? 'Main' : info.tier === 'dvr' ? 'DVR recording' : 'Stream not selected',
    codecLabel(info.codec), info.width && info.height ? `${info.width}×${info.height}` : 'Resolution not reported',
    info.fps && info.fps > 0 ? `${info.fps.toFixed(1)} fps` : 'Frame rate not reported'].join(' · ')
}
export function validStreamChoice(value: unknown): StreamChoice { return value === 'main' || value === 'sub' ? value : 'auto' }

export function recordingPlaybackError(code: number, codec?: string): string {
  const format = codec ? `${codecLabel(codec)} recording` : 'recording'
  if (code === 4) {
    const guidance = codecLabel(codec) === 'H.265 / HEVC'
      ? ' Check HEVC support on this viewing machine (browser, OS codec and graphics driver). Live H.264 Sub playback does not make the recorded primary stream H.264.'
      : ' Check codec support on this viewing machine or try another device.'
    return `This browser could not open the ${format}. Its decoder may be unavailable or unsupported; invalid recording metadata or delivery can also cause this error.${guidance}`
  }
  if (code === 3) return `This browser could not decode the ${format}. Damaged media or unavailable decoder resources may be responsible. Try fewer video streams or another device.`
  return 'Could not load or seek this recording. Retry or check the playback diagnostics.'
}
