<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import { useCameraStore } from '../../stores/cameras'
import { useSettingsStore } from '../../stores/settings'
import CameraCard from './CameraCard.vue'

// Responsive tile grid for the Overview. Canvas layouts (fullscreen, focus,
// automatic slots) moved to the dashboards v2 renderer, DashboardCanvas.
const props = defineProps<{ cameraIds?: Set<number> | null }>()
const emit = defineEmits<{ replay: [cameraId: number] }>()

const cameraStore = useCameraStore()
const settings = useSettingsStore()
const router = useRouter()

const filteredCameras = computed(() => props.cameraIds
  ? cameraStore.cameras.filter(camera => props.cameraIds!.has(camera.id))
  : cameraStore.cameras)
const gridStyle = computed(() => ({
  gridTemplateColumns: `repeat(auto-fill, minmax(${Math.max(200, (settings.cameraPreviewScale / 100) * 600)}px, 1fr))`,
}))
</script>

<template>
  <div v-if="cameraStore.isLoading" class="d-flex justify-content-center py-5">
    <div class="spinner-border text-primary" role="status"><span class="visually-hidden">Loading...</span></div>
  </div>
  <div v-else-if="filteredCameras.length === 0" class="text-center py-5 text-muted-custom"><p>No cameras configured</p></div>
  <div v-else class="camera-grid" :style="gridStyle">
    <div v-for="camera in filteredCameras" :key="camera.id" class="camera-grid-item">
      <CameraCard :camera="camera"
        @open-stream="router.push(`/live/${$event}`)"
        @open-clips="router.push(`/clips/${$event}`)"
        @replay="emit('replay', $event)" />
    </div>
  </div>
</template>
