import { defineStore } from 'pinia'
import { useLocalStorage } from '../composables/useLocalStorage'

export const useSettingsStore = defineStore('settings', () => {
  const cameraPreviewScale = useLocalStorage('cameraPreviewScale', 25)
  const streamingMode = useLocalStorage<'hls' | 'jpeg'>('streamingMode', 'hls')
  const fullscreenMode = useLocalStorage('fullscreenMode', false)
  const darkMode = useLocalStorage('darkMode', false)
  const clipsPerPage = useLocalStorage('clipsPerPage', 24)
  const hideShortClips = useLocalStorage('hideShortClips', false)

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
    streamingMode.value = streamingMode.value === 'hls' ? 'jpeg' : 'hls'
  }

  return {
    cameraPreviewScale,
    streamingMode,
    fullscreenMode,
    darkMode,
    clipsPerPage,
    hideShortClips,
    increaseScale,
    decreaseScale,
    toggleFullscreen,
    toggleStreamingMode,
  }
})
