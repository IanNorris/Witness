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

    <div v-if="camera" class="stream-container">
      <HlsPlayer :camera-id="cameraId" suffix="_stream" :debug="true" />
    </div>
    <div v-else class="text-muted-custom text-center py-5">
      Camera not found
    </div>
  </AppLayout>
</template>

<style scoped>
.stream-container {
  max-width: 1200px;
  margin: 0 auto;
  aspect-ratio: 16 / 9;
  background: #000;
  border-radius: 0.5rem;
  overflow: hidden;
}
</style>
