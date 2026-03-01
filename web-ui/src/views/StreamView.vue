<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useRoute } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import { useCameraStore } from '../stores/cameras'

const route = useRoute()
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
    <div class="text-muted-custom text-center py-5">
      <p>Live stream view — coming soon (Step 4)</p>
    </div>
  </AppLayout>
</template>
