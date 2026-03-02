import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { api } from '../composables/useApi'
import type { Clip } from '../types/clip'
import { LightingCondition } from '../types/clip'
import { useSettingsStore } from './settings'
import { useFilterStore } from './filters'

export const useClipStore = defineStore('clips', () => {
  const clips = ref<Clip[]>([])
  const totalCount = ref(0)
  const loading = ref(false)
  const pageOffset = ref(0)
  const currentCameraId = ref<number | null>(null)

  const settings = useSettingsStore()
  const filterStore = useFilterStore()
  const pageSize = computed(() => settings.clipsPerPage)
  const currentPage = computed(() => Math.floor(pageOffset.value / pageSize.value))
  const totalPages = computed(() => Math.ceil(totalCount.value / pageSize.value))

  async function fetchClips(cameraId: number | null, offset = 0) {
    loading.value = true
    currentCameraId.value = cameraId
    pageOffset.value = offset

    const camParam = cameraId ?? -1
    const startDate = Math.floor(Date.now() / 1000)
    const rangePeriod = 9999999999

    try {
      const data = await api<{ count: number; clips: Record<string, unknown>[] }>(
        `/clip/enum/${camParam}/${pageSize.value}/${startDate}/${rangePeriod}/${offset}${filterStore.filterQueryString}`
      )
      totalCount.value = data.count ?? 0
      clips.value = (data.clips ?? []).map(mapClip)
    } catch {
      clips.value = []
      totalCount.value = 0
    } finally {
      loading.value = false
    }
  }

  async function toggleSave(clipUid: number, value: boolean) {
    await api('/clip/toggleSave', { method: 'POST', body: { id: clipUid, value } })
    const clip = clips.value.find(c => c.uid === clipUid)
    if (clip) clip.saved = value
  }

  async function deleteClip(clipUid: number) {
    await api('/clip/delete', { method: 'POST', body: { id: clipUid } })
    clips.value = clips.value.filter(c => c.uid !== clipUid)
    totalCount.value = Math.max(0, totalCount.value - 1)
  }

  async function retagClip(clipUid: number) {
    await api('/clip/retag', { method: 'POST', body: { id: clipUid } })
  }

  async function reviewClip(clipUid: number) {
    await api('/clip/review', { method: 'POST', body: { id: clipUid, value: true } })
    const clip = clips.value.find(c => c.uid === clipUid)
    if (clip) clip.reviewed = true
  }

  async function reviewAllClips() {
    await api('/clip/review', { method: 'POST', body: { all: true } })
    clips.value.forEach(c => { c.reviewed = true })
  }

  const recentClips = ref<Clip[]>([])

  async function fetchRecent(count = 20) {
    try {
      const data = await api<{ clips: Record<string, unknown>[] }>(`/clip/recent/${count}`)
      recentClips.value = (data.clips ?? []).map(mapClip)
    } catch {
      recentClips.value = []
    }
  }

  function nextPage() {
    if (currentPage.value < totalPages.value - 1) {
      fetchClips(currentCameraId.value, pageOffset.value + pageSize.value)
    }
  }

  function prevPage() {
    if (pageOffset.value > 0) {
      fetchClips(currentCameraId.value, Math.max(0, pageOffset.value - pageSize.value))
    }
  }

  function goToPage(page: number) {
    fetchClips(currentCameraId.value, page * pageSize.value)
  }

  function thumbnailUrl(cameraId: number, timestamp: number) {
    return `/clip/thumb/${cameraId}/${timestamp}`
  }

  function videoUrl(cameraId: number, timestamp: number) {
    return `/clip/video/${cameraId}/${timestamp}`
  }

  return {
    clips, totalCount, loading, pageSize, pageOffset,
    currentCameraId, currentPage, totalPages,
    recentClips,
    fetchClips, toggleSave, deleteClip, retagClip,
    reviewClip, reviewAllClips, fetchRecent,
    nextPage, prevPage, goToPage,
    thumbnailUrl, videoUrl,
  }
})

function mapClip(raw: Record<string, unknown>): Clip {
  return {
    uid: raw.clipUID as number,
    camera: raw.cameraID as number,
    cameraName: '',
    timestamp: raw.timestamp as number,
    duration: raw.duration as number,
    tags: (raw.tags as string) ?? '',
    saved: (raw.saved as number) === 1,
    recordMode: (raw.recordMode as number) === 0 ? 'Manual' : 'Auto',
    description: (raw.description as string) ?? '',
    detectionVersion: (raw.detectionVersion as number) ?? 0,
    lighting: (raw.lighting as number ?? 0) as LightingCondition,
    reviewed: (raw.reviewed as number) === 1,
  }
}
