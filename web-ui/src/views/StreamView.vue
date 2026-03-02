<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import HlsPlayer from '../components/camera/HlsPlayer.vue'
import { useCameraStore } from '../stores/cameras'

const route = useRoute()
const router = useRouter()
const cameraStore = useCameraStore()

const cameraId = computed(() => Number(route.params.cameraId))
const camera = computed(() => cameraStore.getCameraById(cameraId.value))

onMounted(async () => {
  if (cameraStore.cameras.length === 0) {
    await cameraStore.fetchCameras()
  }
})
</script>

<template>
  <AppLayout>
    <template #title>{{ camera?.name || 'Stream' }}</template>
    <template #actions>
      <button class="btn btn-sm btn-outline-secondary" @click="router.push('/')">
        ← Back
      </button>
    </template>

    <div v-if="camera" class="stream-container" @click="cameraStore.toggleRecording(cameraId)">
      <HlsPlayer :camera-id="cameraId" suffix="_stream" :debug="true" />
      <div v-if="camera.isRecording" class="rec-overlay">
        <span class="rec-dot" /> REC
      </div>
    </div>
    <div v-else class="text-muted-custom text-center py-5">
      Camera not found
    </div>
  </AppLayout>
</template>

<style scoped>
.stream-container {
  position: relative;
  max-width: 1200px;
  margin: 0 auto;
  aspect-ratio: 16 / 9;
  background: #000;
  border-radius: 0.5rem;
  overflow: hidden;
  cursor: pointer;
}
.rec-overlay {
  position: absolute;
  top: 8px;
  left: 10px;
  display: inline-flex;
  align-items: center;
  gap: 0.3rem;
  color: #f85149;
  font-size: 0.75rem;
  font-weight: 600;
  background: rgba(0, 0, 0, 0.55);
  padding: 2px 8px;
  border-radius: 3px;
  pointer-events: none;
  z-index: 2;
}
.rec-dot {
  display: inline-block;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: #f85149;
  animation: pulse 1.5s infinite;
}
@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.3; }
}
</style>
