<script setup lang="ts">
import { ref } from 'vue'
import { useHls } from '../../composables/useHls'

const props = defineProps<{
  cameraId: number
  suffix?: string
  debug?: boolean
}>()

const videoRef = ref<HTMLVideoElement | null>(null)
const { showSpinner, connectionLost } = useHls(
  props.cameraId,
  videoRef,
  props.suffix ?? '',
  props.debug ?? false,
)
</script>

<template>
  <div class="hls-container">
    <video ref="videoRef" playsinline muted />

    <!-- Spinner: connecting / buffering -->
    <div v-if="showSpinner" class="spinner-indicator">
      <div class="spinner-border spinner-border-sm text-light" role="status">
        <span class="visually-hidden">Loading...</span>
      </div>
    </div>

    <!-- Connection lost -->
    <div v-if="connectionLost" class="camera-overlay connection-lost">
      <div class="text-center">
        <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
          <line x1="1" y1="1" x2="23" y2="23" />
          <path d="M16.72 11.06A10.94 10.94 0 0 1 19 12.55" />
          <path d="M5 12.55a10.94 10.94 0 0 1 5.17-2.39" />
        </svg>
        <div class="mt-1 small">No signal</div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.hls-container {
  position: relative;
  width: 100%;
  height: 100%;
}

.hls-container video {
  width: 100%;
  height: 100%;
  object-fit: contain;
  background: #000;
}

.spinner-indicator {
  position: absolute;
  top: 6px;
  right: 8px;
  z-index: 2;
  pointer-events: none;
}
</style>
