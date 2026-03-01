<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import { useCameraStore } from '../../stores/cameras'
import { useSettingsStore } from '../../stores/settings'
import CameraCard from './CameraCard.vue'

const cameraStore = useCameraStore()
const settings = useSettingsStore()
const router = useRouter()

const gridStyle = computed(() => {
  if (settings.fullscreenMode) {
    const cols = Math.ceil(Math.sqrt(cameraStore.cameras.length))
    return {
      gridTemplateColumns: `repeat(${cols}, 1fr)`,
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
    <CameraCard
      v-for="camera in cameraStore.cameras"
      :key="camera.id"
      :camera="camera"
      @open-stream="openStream"
      @open-clips="openClips"
    />
  </div>
</template>
