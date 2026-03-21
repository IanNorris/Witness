<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed } from 'vue'
import { api } from '../../composables/useApi'
import { useCameraStore } from '../../stores/cameras'
import { useEventStream } from '../../composables/useEventStream'
import { format } from 'date-fns'

const cameraStore = useCameraStore()
const events = useEventStream()

interface QueuedClip {
  uid: number
  timestamp: number
  camera: number
  detectionVersion: number
  recordMode: number
  tags: string
  duration: number
}

interface QueueResponse {
  total: number
  clips: QueuedClip[]
  detectionVersion: number
}

const loading = ref(true)
const totalCount = ref(0)
const detectionVersion = ref(0)
const clips = ref<QueuedClip[]>([])
const currentClipUID = ref<number | null>(null)
const currentProgress = ref({ frame: 0, totalFrames: 0 })

let refreshTimer: ReturnType<typeof setInterval> | null = null
let unsubscribe: (() => void) | null = null

async function fetchQueue() {
  try {
    const data = await api<QueueResponse>('/api/reprocess/queue')
    totalCount.value = data.total
    clips.value = data.clips
    detectionVersion.value = data.detectionVersion
  } catch {
    // ignore
  } finally {
    loading.value = false
  }
}

function cameraName(id: number): string {
  const cam = cameraStore.getCameraById(id)
  return cam ? cam.name : `Camera ${id}`
}

function formatTime(epoch: number): string {
  return format(new Date(epoch * 1000), 'dd MMM HH:mm:ss')
}

function priorityLabel(version: number): string {
  if (version === -1) return 'Manual'
  if (version === 0) return 'Bulk'
  return `v${version}`
}

function priorityClass(version: number): string {
  if (version === -1) return 'badge bg-warning text-dark'
  if (version === 0) return 'badge bg-info text-dark'
  return 'badge bg-secondary'
}

const progressPercent = computed(() => {
  if (!currentProgress.value.totalFrames) return 0
  return Math.round((currentProgress.value.frame / currentProgress.value.totalFrames) * 100)
})

onMounted(() => {
  fetchQueue()
  refreshTimer = setInterval(fetchQueue, 10000)

  unsubscribe = events.onEvent((evt) => {
    if (evt.event !== 'reprocess:progress') return
    const data = evt.data as unknown as {
      clipUID: number; stage: string; frame: number; totalFrames: number
    }
    if (data.stage === 'processing') {
      currentClipUID.value = data.clipUID
      currentProgress.value = { frame: data.frame, totalFrames: data.totalFrames }
    } else if (data.stage === 'complete') {
      clips.value = clips.value.filter(c => c.uid !== data.clipUID)
      totalCount.value = Math.max(0, totalCount.value - 1)
      currentClipUID.value = null
    } else if (data.stage === 'idle') {
      currentClipUID.value = null
      clips.value = []
      totalCount.value = 0
    }
  })
})

onUnmounted(() => {
  if (refreshTimer) clearInterval(refreshTimer)
  if (unsubscribe) unsubscribe()
})
</script>

<template>
  <div>
    <div class="d-flex justify-content-between align-items-center mb-3">
      <div>
        <h6 class="mb-0">Reprocess Queue</h6>
        <small class="text-muted-custom">
          {{ totalCount }} clips pending · Detection version {{ detectionVersion }}
        </small>
      </div>
      <button class="btn btn-sm btn-outline-secondary" @click="fetchQueue" :disabled="loading">↻</button>
    </div>

    <!-- Current processing -->
    <div v-if="currentClipUID" class="card mb-3" style="background: var(--bs-dark, #1e1e2e); border-color: var(--bs-border-color);">
      <div class="card-body py-2">
        <div class="d-flex justify-content-between align-items-center mb-1">
          <small class="text-muted-custom">Currently processing</small>
          <small class="text-muted-custom">Clip #{{ currentClipUID }}</small>
        </div>
        <div class="progress" style="height: 6px;">
          <div class="progress-bar bg-primary" :style="{ width: progressPercent + '%' }"></div>
        </div>
        <small class="text-muted-custom">
          Frame {{ currentProgress.frame }} / {{ currentProgress.totalFrames }}
          ({{ progressPercent }}%)
        </small>
      </div>
    </div>

    <!-- Loading -->
    <div v-if="loading" class="text-center py-4">
      <div class="spinner-border spinner-border-sm text-primary"></div>
    </div>

    <!-- Empty state -->
    <div v-else-if="clips.length === 0 && !currentClipUID" class="text-muted-custom text-center py-4">
      Queue is empty — all clips are up to date
    </div>

    <!-- Queue table -->
    <div v-else class="table-responsive">
      <table class="table table-sm table-hover" style="color: var(--bs-body-color);">
        <thead>
          <tr class="text-muted-custom small">
            <th>#</th>
            <th>Priority</th>
            <th>Camera</th>
            <th>Date</th>
            <th>Duration</th>
            <th>Tags</th>
          </tr>
        </thead>
        <tbody>
          <tr
            v-for="(clip, idx) in clips"
            :key="clip.uid"
            :class="{ 'table-active': clip.uid === currentClipUID }"
          >
            <td class="text-muted-custom small">{{ idx + 1 }}</td>
            <td>
              <span :class="priorityClass(clip.detectionVersion)">
                {{ priorityLabel(clip.detectionVersion) }}
              </span>
            </td>
            <td>{{ cameraName(clip.camera) }}</td>
            <td>{{ formatTime(clip.timestamp) }}</td>
            <td>{{ Math.round(clip.duration) }}s</td>
            <td class="small text-muted-custom">{{ clip.tags || '—' }}</td>
          </tr>
        </tbody>
      </table>
      <div v-if="totalCount > clips.length" class="text-muted-custom small text-center">
        Showing {{ clips.length }} of {{ totalCount }} queued clips
      </div>
    </div>
  </div>
</template>
