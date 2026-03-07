<script setup lang="ts">
import { onMounted } from 'vue'
import AppLayout from '../components/layout/AppLayout.vue'
import CameraGrid from '../components/camera/CameraGrid.vue'
import { useCameraStore } from '../stores/cameras'
import { useSettingsStore } from '../stores/settings'

const cameraStore = useCameraStore()
const settings = useSettingsStore()

onMounted(async () => {
  await cameraStore.fetchCameras()
})
</script>

<template>
  <AppLayout>
    <template #title>Dashboard</template>
    <template #actions>
      <div class="d-flex align-items-center gap-2">
        <button
          class="btn btn-sm btn-outline-secondary mobile-hide"
          @click="settings.decreaseScale"
          title="Zoom out"
        >−</button>
        <span class="small text-muted-custom mobile-hide" style="min-width: 3rem; text-align: center;">
          {{ settings.cameraPreviewScale }}%
        </span>
        <button
          class="btn btn-sm btn-outline-secondary mobile-hide"
          @click="settings.increaseScale"
          title="Zoom in"
        >+</button>
        <button
          class="btn btn-sm"
          :class="settings.fullscreenMode ? 'btn-primary' : 'btn-outline-secondary'"
          @click="settings.toggleFullscreen"
          title="Fullscreen"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/>
            <line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/>
          </svg>
        </button>
        <div class="vr mx-1 mobile-hide" style="border-color: var(--bs-border-color);"></div>
        <button
          class="btn btn-sm mobile-hide"
          :class="settings.streamingMode !== 'jpeg' ? 'btn-outline-primary' : 'btn-outline-secondary'"
          @click="settings.toggleStreamingMode"
          :title="'Mode: ' + settings.streamingMode.toUpperCase()"
        >
          {{ settings.streamingMode.toUpperCase() }}
        </button>
      </div>
    </template>

    <CameraGrid />
  </AppLayout>
</template>
