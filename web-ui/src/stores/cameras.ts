import { defineStore } from 'pinia'
import { ref } from 'vue'
import { api } from '../composables/useApi'
import type { Camera, CameraData } from '../types/camera'

export const useCameraStore = defineStore('cameras', () => {
  const cameras = ref<Camera[]>([])
  const isLoading = ref(true)
  let pollAbort: AbortController | null = null

  function parseCameraData(data: CameraData[]): Camera[] {
    return data.map((c) => ({
      id: c.id,
      name: c.name,
      status: c.status ?? 'Unknown',
      isRecording: c.recording ?? false,
      groups: Array.isArray(c.groups) ? c.groups : [],
      previewUrl: `/camera/preview/${c.id}`,
      streamUrl: `/stream/${c.id}/playlist`,
      lowLatencyHLS: !!c.lowLatencyHLS,
      stats: {
        fps: 0,
        bitrate: 0,
        uptime: '',
        reconnects: 0,
      },
    }))
  }

  async function fetchCameras() {
    try {
      const data = await api<CameraData[]>('/camera/enum')
      if (data) {
        cameras.value = parseCameraData(data)
      }
    } catch (err) {
      console.error('Failed to fetch cameras:', err)
    } finally {
      isLoading.value = false
    }
  }

  async function startLongPoll() {
    while (true) {
      try {
        pollAbort = new AbortController()
        const response = await fetch('/camera/enum_longpoll', {
          credentials: 'same-origin',
          signal: pollAbort.signal,
        })
        if (response.ok) {
          const data = (await response.json()) as CameraData[]
          cameras.value = parseCameraData(data)
        }
      } catch (err) {
        if ((err as Error).name === 'AbortError') return
        // Wait before retry on error
        await new Promise((r) => setTimeout(r, 3000))
      }
    }
  }

  function stopLongPoll() {
    pollAbort?.abort()
    pollAbort = null
  }

  function getCameraById(id: number): Camera | undefined {
    return cameras.value.find((c) => c.id === id)
  }

  async function toggleRecording(cameraId: number) {
    const camera = getCameraById(cameraId)
    if (!camera) return
    const newState = !camera.isRecording
    try {
      await api(`/camera/record/${cameraId}`, {
        method: 'POST',
        body: { record: newState },
      })
      camera.isRecording = newState
    } catch (err) {
      console.error('Failed to toggle recording:', err)
    }
  }

  return {
    cameras,
    isLoading,
    fetchCameras,
    startLongPoll,
    stopLongPoll,
    getCameraById,
    toggleRecording,
  }
})
