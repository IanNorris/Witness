<script setup lang="ts">
import { ref } from 'vue'
import { useMseStream } from '../../composables/useMseStream'
import { useDetectionOverlay } from '../../composables/useDetectionOverlay'

const props = defineProps<{
  cameraId: number
  suffix?: string
}>()

const videoRef = ref<HTMLVideoElement | null>(null)
const canvasRef = ref<HTMLCanvasElement | null>(null)
const { showSpinner, connectionLost, latencyMs } = useMseStream(
  props.cameraId,
  videoRef,
  props.suffix ?? '',
)

const { enabled: overlayEnabled, toggle: toggleOverlay } = useDetectionOverlay(
  props.cameraId,
  canvasRef,
  videoRef,
)

defineExpose({ latencyMs, overlayEnabled, toggleOverlay })
</script>

<template>
  <div class="mse-container">
    <video ref="videoRef" playsinline muted />
    <canvas ref="canvasRef" class="detection-overlay" v-show="overlayEnabled" />

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
.mse-container {
  position: relative;
  width: 100%;
  height: 100%;
  overflow: hidden;
}

.mse-container video {
  width: 100%;
  height: 100%;
  object-fit: contain;
  background: #000;
}

.detection-overlay {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
  z-index: 5;
}

.spinner-indicator {
  position: absolute;
  top: 6px;
  right: 8px;
  z-index: 10;
  pointer-events: none;
}
</style>
