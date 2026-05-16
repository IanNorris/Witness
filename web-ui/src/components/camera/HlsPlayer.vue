<script setup lang="ts">
import { ref } from 'vue'
import { useHls } from '../../composables/useHls'
import { useDetectionOverlay } from '../../composables/useDetectionOverlay'

const props = defineProps<{
  cameraId: number
  suffix?: string
  debug?: boolean
  lowLatency?: boolean
}>()

const videoRef = ref<HTMLVideoElement | null>(null)
const canvasRef = ref<HTMLCanvasElement | null>(null)
const { showSpinner, connectionLost, latencyMs, codecUnsupported } = useHls(
  props.cameraId,
  videoRef,
  props.suffix ?? '',
  props.debug ?? false,
  props.lowLatency ?? false,
)

const { enabled: overlayEnabled, toggle: toggleOverlay } = useDetectionOverlay(
  props.cameraId,
  canvasRef,
  videoRef,
)

defineExpose({ latencyMs, overlayEnabled, toggleOverlay, codecUnsupported })
</script>

<template>
  <div class="hls-container">
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

    <!-- Codec unsupported -->
    <div v-if="codecUnsupported" class="camera-overlay codec-unsupported">
      <div class="text-center">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
          <circle cx="12" cy="12" r="10" />
          <line x1="12" y1="8" x2="12" y2="12" />
          <line x1="12" y1="16" x2="12.01" y2="16" />
        </svg>
        <div class="mt-1 small">H.265 unsupported</div>
        <div class="small text-muted">Install HEVC extensions<br/>or switch camera to H.264</div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.hls-container {
  position: relative;
  width: 100%;
  height: 100%;
  overflow: hidden;
}

.hls-container video {
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

.codec-unsupported {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.85);
  color: #fbbf24;
  z-index: 15;
}
</style>
