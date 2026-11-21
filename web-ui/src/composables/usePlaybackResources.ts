import { ref, watch, onUnmounted, type Ref } from 'vue'
import type { StreamInfo } from '../utils/streamChoice'
export interface PlaybackResource extends StreamInfo { id: string; cameraId: number; mode: string; readyState: number; paused: boolean; hasSource: boolean; errored?: boolean }
const resources = new Map<string, () => PlaybackResource>()
export const decoderPressureWarning = ref<{ cameraId: number; activeStreams: number; streams: PlaybackResource[] } | null>(null)
let lastWarningAt = -Infinity
let lastFailure: { t: string; cameraId: number; code: number; streams: PlaybackResource[] } | undefined
export function playbackResourceSnapshot(): PlaybackResource[] {
  return [...resources.values()].map(snapshot => snapshot()).filter(stream => stream.hasSource)
}
export function suspectDecoderPressure(code: number, streams: PlaybackResource[]): boolean {
  // Network errors, unsupported formats and ordinary buffering are not evidence
  // of decoder pressure. MEDIA_ERR_DECODE remains ambiguous even here.
  return code === 3 && streams.length > 1
}
// A decode error is often followed by automatic recovery: e.g. a wall display
// that cannot decode an HEVC main stream drops to the camera's H.264
// sub-stream. Only warn if, after a short grace period, the failing camera
// still has no stream presenting frames.
export const PRESSURE_GRACE_MS = 5000
export function cameraRecovered(cameraId: number, streams: PlaybackResource[]): boolean {
  return streams.some(stream => stream.cameraId === cameraId && stream.readyState >= 2 && !stream.paused && !stream.errored)
}
let pendingWarning: ReturnType<typeof setTimeout> | null = null
if (typeof window !== 'undefined') (window as unknown as Record<string, unknown>)._witnessPlaybackResources = {
  snapshot: () => ({ streams: playbackResourceSnapshot(), lastFailure }),
}
export function usePlaybackResources(cameraId: number, video: Ref<HTMLVideoElement | null>, mode: string,
  info: () => StreamInfo, log?: (type: string, data: Record<string, unknown>) => void) {
  const id = `${cameraId}_${mode}_${crypto.randomUUID()}`
  const snapshot = (): PlaybackResource => ({ id, cameraId, mode, ...info(),
    width: video.value?.videoWidth || info().width, height: video.value?.videoHeight || info().height,
    readyState: video.value?.readyState ?? 0, paused: video.value?.paused ?? true, errored: !!video.value?.error,
    hasSource: !!(video.value?.currentSrc || video.value?.src || video.value?.srcObject) })
  let detach: (() => void) | undefined
  const stop = watch(video, element => {
    detach?.()
    if (!element) { resources.delete(id); return }
    resources.set(id, snapshot)
    const onError = () => {
      const code = element.error?.code ?? 0
      const streams = playbackResourceSnapshot()
      lastFailure = { t: new Date().toISOString(), cameraId, code, streams }
      log?.('decoderFailure', { code, activeStreams: streams.length, streams })
      if (!suspectDecoderPressure(code, streams)) return
      if (Date.now() - lastWarningAt < 60000 || pendingWarning) return
      pendingWarning = setTimeout(() => {
        pendingWarning = null
        const current = playbackResourceSnapshot()
        if (cameraRecovered(cameraId, current)) return // degraded and playing: nothing to report
        lastWarningAt = Date.now()
        decoderPressureWarning.value = { cameraId, activeStreams: streams.length, streams }
      }, PRESSURE_GRACE_MS)
    }
    element.addEventListener('error', onError)
    detach = () => element.removeEventListener('error', onError)
  }, { flush: 'post', immediate: true })
  onUnmounted(() => { stop(); detach?.(); resources.delete(id) })
  return { snapshot }
}
