<script setup lang="ts">
import { ref, watch, onMounted, onUnmounted } from 'vue'
import { useMseStream } from '../../composables/useMseStream'
import { useDetectionOverlay } from '../../composables/useDetectionOverlay'

const props = defineProps<{
  cameraId: number
  suffix?: string
  useSubStream?: boolean
  codecHint?: string
  audioEnabled?: boolean
	adaptiveStream?: boolean
}>()

const emit = defineEmits<{
  streamChanged: [stream: 'main' | 'sub']
}>()

const containerRef = ref<HTMLDivElement | null>(null)
const videoRef = ref<HTMLVideoElement | null>(null)
const canvasRef = ref<HTMLCanvasElement | null>(null)
const freezeCanvasRef = ref<HTMLCanvasElement | null>(null)
let viewportWidth = 0
let viewportHeight = 0
let resizeObserver: ResizeObserver | null = null
let resizeTimer: ReturnType<typeof setTimeout> | null = null
let resolutionQuery: MediaQueryList | null = null

function measureViewport() {
	const container = containerRef.value
	if (!container) return
	const rect = container.getBoundingClientRect()
	const scale = window.devicePixelRatio || 1
	// Bucketing avoids stream-policy churn from one-pixel layout changes.
	viewportWidth = Math.max(1, Math.ceil((rect.width * scale) / 64) * 64)
	viewportHeight = Math.max(1, Math.ceil((rect.height * scale) / 64) * 64)
}

function scheduleViewportUpdate() {
	measureViewport()
	if (resizeTimer) clearTimeout(resizeTimer)
	resizeTimer = setTimeout(() => {
		resizeTimer = null
		updateViewport(viewportWidth, viewportHeight)
	}, 250)
}

function watchDevicePixelRatio() {
	resolutionQuery?.removeEventListener('change', handleDevicePixelRatioChange)
	resolutionQuery = window.matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`)
	resolutionQuery.addEventListener('change', handleDevicePixelRatioChange)
}

function handleDevicePixelRatioChange() {
	scheduleViewportUpdate()
	watchDevicePixelRatio()
}

const {
	showSpinner,
	connectionLost,
	latencyMs,
	codecUnsupported,
	renderSuppressed,
	selectedStream,
	updateViewport,
} = useMseStream(
  props.cameraId,
  videoRef,
  props.suffix ?? '',
  props.useSubStream ?? false,
  props.codecHint,
  () => props.audioEnabled ?? false,
	props.adaptiveStream ?? false,
	() => ({ width: viewportWidth, height: viewportHeight }),
)

watch(selectedStream, stream => emit('streamChanged', stream), { immediate: true })

const { enabled: overlayEnabled, toggle: toggleOverlay } = useDetectionOverlay(
  props.cameraId,
  canvasRef,
  videoRef,
)

function setAudioEnabled(enabled: boolean) {
  const video = videoRef.value
  if (!video) return false
  video.muted = !enabled
  if (enabled) video.play().catch(() => {})
  return !video.muted
}

function toggleAudio() {
  return setAudioEnabled(videoRef.value?.muted ?? true)
}

watch(() => props.audioEnabled, enabled => setAudioEnabled(enabled ?? false))

watch(renderSuppressed, suppressed => {
  if (!suppressed) return
  const video = videoRef.value
  const canvas = freezeCanvasRef.value
  if (!video || !canvas || video.videoWidth <= 0 || video.videoHeight <= 0) return

  canvas.width = video.videoWidth
  canvas.height = video.videoHeight
  canvas.getContext('2d')?.drawImage(video, 0, 0, canvas.width, canvas.height)
}, { flush: 'sync' })

onMounted(() => {
	measureViewport()
	updateViewport(viewportWidth, viewportHeight)
	if (!props.adaptiveStream || !containerRef.value) return
	resizeObserver = new ResizeObserver(scheduleViewportUpdate)
	resizeObserver.observe(containerRef.value)
	watchDevicePixelRatio()
})

onUnmounted(() => {
	resizeObserver?.disconnect()
	resolutionQuery?.removeEventListener('change', handleDevicePixelRatioChange)
	if (resizeTimer) clearTimeout(resizeTimer)
})

defineExpose({
	latencyMs,
	overlayEnabled,
	toggleOverlay,
	codecUnsupported,
	selectedStream,
	toggleAudio,
	setAudioEnabled,
})
</script>

<template>
  <div ref="containerRef" class="mse-container">
    <video ref="videoRef" playsinline :muted="!(audioEnabled ?? false)" />
    <canvas ref="freezeCanvasRef" class="render-freeze" v-show="renderSuppressed" />
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

.render-freeze {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  object-fit: contain;
  background: #000;
  pointer-events: none;
  z-index: 4;
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
