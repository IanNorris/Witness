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
      id: c.ID,
      name: c.Name,
      status: c.Status,
      isRecording: c.IsRecording,
      groups: c.Groups ? c.Groups.split(',').map(Number).filter(Boolean) : [],
      previewUrl: `/camera/preview/${c.ID}`,
      streamUrl: `/stream/${c.ID}/playlist`,
      stats: {
        fps: c.FPS,
        bitrate: c.Bitrate,
        uptime: c.Uptime,
        reconnects: c.Reconnects,
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

  return {
    cameras,
    isLoading,
    fetchCameras,
    startLongPoll,
    stopLongPoll,
    getCameraById,
  }
})
