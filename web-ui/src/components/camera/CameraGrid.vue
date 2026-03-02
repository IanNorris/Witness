<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import { useCameraStore } from '../../stores/cameras'
import { useSettingsStore } from '../../stores/settings'
import CameraCard from './CameraCard.vue'

const cameraStore = useCameraStore()
const settings = useSettingsStore()
const router = useRouter()

const fullscreenCols = computed(() => Math.ceil(Math.sqrt(cameraStore.cameras.length)))
const fullscreenRows = computed(() => Math.ceil(cameraStore.cameras.length / fullscreenCols.value))

const gridStyle = computed(() => {
  if (settings.fullscreenMode) {
    // Use flexbox for fullscreen so last row centers naturally
    return {
      '--fs-cols': fullscreenCols.value,
      '--fs-rows': fullscreenRows.value,
    }
  }
  const minWidth = Math.max(200, (settings.cameraPreviewScale / 100) * 600)
  return {
    gridTemplateColumns: `repeat(auto-fill, minmax(${minWidth}px, 1fr))`,
  }
})

function openStream(cameraId: number) {
  router.push(`/stream/${cameraId}`)
}

function openClips(cameraId: number) {
  router.push(`/clips/${cameraId}`)
}

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape' && settings.fullscreenMode) {
    settings.toggleFullscreen()
  }
}

onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))
</script>

<template>
  <div v-if="cameraStore.isLoading" class="d-flex justify-content-center py-5">
    <div class="spinner-border text-primary" role="status">
      <span class="visually-hidden">Loading...</span>
    </div>
  </div>

  <div v-else-if="cameraStore.cameras.length === 0" class="text-center py-5 text-muted-custom">
    <p>No cameras configured</p>
  </div>

  <div
    v-else
    class="camera-grid"
    :class="{ fullscreen: settings.fullscreenMode }"
    :style="gridStyle"
  >
    <!-- Fullscreen exit button -->
    <button
      v-if="settings.fullscreenMode"
      class="fullscreen-exit-btn"
      @click="settings.toggleFullscreen"
      title="Exit fullscreen (Esc)"
    >
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
        <polyline points="4 14 10 14 10 20"/><polyline points="20 10 14 10 14 4"/>
        <line x1="14" y1="10" x2="21" y2="3"/><line x1="3" y1="21" x2="10" y2="14"/>
      </svg>
    </button>

    <CameraCard
      v-for="camera in cameraStore.cameras"
      :key="camera.id"
      :camera="camera"
      @open-stream="openStream"
      @open-clips="openClips"
    />
  </div>
</template>
