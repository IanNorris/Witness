import { ref } from 'vue'

// Session-only: never overwrite the user's saved audio preference.
export const blockedAudioCameras = ref(new Set<number>())
export const playbackWarning = ref('')
let warned = false

export function retryCameraAudio(cameraId: number) {
  const next = new Set(blockedAudioCameras.value)
  next.delete(cameraId)
  blockedAudioCameras.value = next
}

export function createPlaybackGuard(
  cameraId: number,
  getVideo: () => HTMLVideoElement | null,
  log: (type: string, data: Record<string, unknown>) => void,
) {
  let pending: { video: HTMLVideoElement } | null = null
  let revision = 0

  async function play(reason: string) {
    const video = getVideo()
    if (!video || pending?.video === video) return
    const token = revision
    const src = video.src
    const audible = !video.muted
    const request = { video }
    pending = request
    try {
      await video.play()
    } catch (error) {
      // Ignore rejections from a replaced source or a later audio choice.
      if (getVideo() !== video || video.src !== src || token !== revision) return
      const name = error instanceof Error ? error.name : 'UnknownError'
      log('playRejected', { name, reason, message: `Camera ${cameraId}: ${reason}: ${name}` })
      if (name !== 'NotAllowedError' || !audible || video.muted) return
      blockedAudioCameras.value = new Set([...blockedAudioCameras.value, cameraId])
      video.muted = true
      log('audioBlocked', { name, reason, message: `Camera ${cameraId}: audio blocked; retrying muted video` })
      if (!warned) {
        warned = true
        playbackWarning.value = `Camera ${cameraId}: your browser blocked live audio. Video will continue muted. Allow autoplay for this site, then click the audio button to retry.`
      }
      try {
        await video.play()
        log('mutedPlaybackResumed', { reason })
      } catch (fallbackError) {
        if (getVideo() !== video || video.src !== src) return
        const fallbackName = fallbackError instanceof Error ? fallbackError.name : 'UnknownError'
        log('mutedPlayRejected', { name: fallbackName, reason, message: `Camera ${cameraId}: muted playback rejected: ${fallbackName}` })
      }
    } finally {
      if (pending === request) pending = null
    }
  }

  function setAudioEnabled(enabled: boolean) {
    const video = getVideo()
    if (!video) return false
    revision++
    pending = null
    video.muted = !enabled || blockedAudioCameras.value.has(cameraId)
    void play(enabled ? 'audioEnable' : 'audioMute')
    return !video.muted
  }

  return { play, setAudioEnabled }
}
