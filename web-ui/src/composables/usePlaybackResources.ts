import { ref, watch, onUnmounted, type Ref } from 'vue'
import type { StreamInfo } from '../utils/streamChoice'
export interface PlaybackResource extends StreamInfo { id: string; cameraId: number; mode: string; readyState: number; paused: boolean; hasSource: boolean }
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
if (typeof window !== 'undefined') (window as unknown as Record<string, unknown>)._witnessPlaybackResources = {
  snapshot: () => ({ streams: playbackResourceSnapshot(), lastFailure }),
}
export function usePlaybackResources(cameraId: number, video: Ref<HTMLVideoElement | null>, mode: string,
  info: () => StreamInfo, log?: (type: string, data: Record<string, unknown>) => void) {
  const id = `${cameraId}_${mode}_${crypto.randomUUID()}`
  const snapshot = (): PlaybackResource => ({ id, cameraId, mode, ...info(),
    width: video.value?.videoWidth || info().width, height: video.value?.videoHeight || info().height,
    readyState: video.value?.readyState ?? 0, paused: video.value?.paused ?? true,
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
      if (Date.now() - lastWarningAt < 60000) return
      lastWarningAt = Date.now()
      decoderPressureWarning.value = { cameraId, activeStreams: streams.length, streams }
    }
    element.addEventListener('error', onError)
    detach = () => element.removeEventListener('error', onError)
  }, { flush: 'post', immediate: true })
  onUnmounted(() => { stop(); detach?.(); resources.delete(id) })
  return { snapshot }
}
