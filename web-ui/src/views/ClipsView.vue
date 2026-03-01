<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useRoute } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import { useCameraStore } from '../stores/cameras'

const route = useRoute()
const cameraStore = useCameraStore()

const cameraId = computed(() =>
  route.params.cameraId ? Number(route.params.cameraId) : null
)

const title = computed(() => {
  if (cameraId.value) {
    const cam = cameraStore.getCameraById(cameraId.value)
    return cam ? `Clips — ${cam.name}` : 'Clips'
  }
  return 'All Clips'
})

onMounted(async () => {
  if (cameraStore.cameras.length === 0) {
    await cameraStore.fetchCameras()
  }
})
</script>

<template>
  <AppLayout>
    <template #title>{{ title }}</template>
    <div class="text-muted-custom text-center py-5">
      <p>Clips browser — coming soon</p>
    </div>
  </AppLayout>
</template>
