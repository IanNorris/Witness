import { defineStore } from 'pinia'
import { useLocalStorage } from '../composables/useLocalStorage'
import { validStreamChoice, type StreamChoice } from '../utils/streamChoice'

export const useSettingsStore = defineStore('settings', () => {
  const cameraPreviewScale = useLocalStorage('cameraPreviewScale', 25)
  const streamingMode = useLocalStorage<'hls' | 'jpeg' | 'mse'>('streamingMode', 'mse')
  const fullscreenMode = useLocalStorage('fullscreenMode', false)
  const darkMode = useLocalStorage('darkMode', false)
  const clipsPerPage = useLocalStorage('clipsPerPage', 24)
  const hideShortClips = useLocalStorage('hideShortClips', false)
  const muteBackgroundOnlyClips = useLocalStorage('muteBackgroundOnlyClips', false)
  const detectionMinConfidence = useLocalStorage('detectionMinConfidence', 45)
  const trailEnabled = useLocalStorage('trailEnabled', true)
  const trailAnchor = useLocalStorage<'bottom-center' | 'center' | 'top-center'>('trailAnchor', 'bottom-center')
  const trailOpacity = useLocalStorage('trailOpacity', 60)
  const trailMaxPoints = useLocalStorage('trailMaxPoints', 200)
  const showTrails = useLocalStorage('showTrails', false)
  const trailColumnWidth = useLocalStorage('trailColumnWidth', 400)
  const dashboardReturnLiveSeconds = useLocalStorage('dashboardReturnLiveSeconds', 180)
  const dashboardQuietControls = useLocalStorage('dashboardQuietControls', true)
  const cameraStreamChoices = useLocalStorage<Record<string, StreamChoice>>('witness-camera-stream-choices', {})
  function getCameraStreamChoice(id: number): StreamChoice {
    const values = cameraStreamChoices.value
    return validStreamChoice(values && typeof values === 'object' ? values[String(id)] : undefined)
  }
  function setCameraStreamChoice(id: number, choice: unknown) {
    cameraStreamChoices.value = { ...(cameraStreamChoices.value && typeof cameraStreamChoices.value === 'object' ? cameraStreamChoices.value : {}), [String(id)]: validStreamChoice(choice) }
  }

  function increaseScale() {
    cameraPreviewScale.value = Math.min(100, cameraPreviewScale.value + 5)
  }

  function decreaseScale() {
    cameraPreviewScale.value = Math.max(10, cameraPreviewScale.value - 5)
  }

  function toggleFullscreen() {
    fullscreenMode.value = !fullscreenMode.value
  }

  function toggleStreamingMode() {
    const modes: ('hls' | 'jpeg' | 'mse')[] = ['hls', 'mse', 'jpeg']
    const idx = modes.indexOf(streamingMode.value)
    streamingMode.value = modes[(idx + 1) % modes.length]!
  }

  return {
    cameraPreviewScale,
    streamingMode,
    fullscreenMode,
    darkMode,
    clipsPerPage,
    hideShortClips,
    muteBackgroundOnlyClips,
    detectionMinConfidence,
    trailEnabled,
    trailAnchor,
    trailOpacity,
    trailMaxPoints,
    showTrails,
    trailColumnWidth,
    dashboardReturnLiveSeconds,
    dashboardQuietControls,
    getCameraStreamChoice,
    setCameraStreamChoice,
    increaseScale,
    decreaseScale,
    toggleFullscreen,
    toggleStreamingMode,
  }
})
