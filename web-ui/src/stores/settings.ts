import { defineStore } from 'pinia'
import { useLocalStorage } from '../composables/useLocalStorage'

export const useSettingsStore = defineStore('settings', () => {
  const cameraPreviewScale = useLocalStorage('cameraPreviewScale', 25)
  const streamingMode = useLocalStorage<'hls' | 'jpeg' | 'mse'>('streamingMode', 'mse')
  const fullscreenMode = useLocalStorage('fullscreenMode', false)
  const darkMode = useLocalStorage('darkMode', false)
  const clipsPerPage = useLocalStorage('clipsPerPage', 24)
  const hideShortClips = useLocalStorage('hideShortClips', false)
  const detectionMinConfidence = useLocalStorage('detectionMinConfidence', 45)
  const trailEnabled = useLocalStorage('trailEnabled', true)
  const trailAnchor = useLocalStorage<'bottom-center' | 'center' | 'top-center'>('trailAnchor', 'bottom-center')
  const trailOpacity = useLocalStorage('trailOpacity', 60)
  const trailMaxPoints = useLocalStorage('trailMaxPoints', 200)
  const showTrails = useLocalStorage('showTrails', false)
  const trailColumnWidth = useLocalStorage('trailColumnWidth', 400)

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
    detectionMinConfidence,
    trailEnabled,
    trailAnchor,
    trailOpacity,
    trailMaxPoints,
    showTrails,
    trailColumnWidth,
    increaseScale,
    decreaseScale,
    toggleFullscreen,
    toggleStreamingMode,
  }
})
