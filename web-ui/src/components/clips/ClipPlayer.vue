<script setup lang="ts">
import { ref, onMounted } from 'vue'
import type { Clip } from '../../types/clip'
import { useClipStore } from '../../stores/clips'
import { useDetectionPlayback } from '../../composables/useDetectionOverlay'

const props = defineProps<{
  clip: Clip
}>()

const emit = defineEmits<{ close: [] }>()

const clipStore = useClipStore()
const videoSrc = ref(clipStore.videoUrl(props.clip.camera, props.clip.timestamp))
const videoRef = ref<HTMLVideoElement | null>(null)
const canvasRef = ref<HTMLCanvasElement | null>(null)

const { enabled: overlayEnabled, toggle: toggleOverlay, loadDetections } = useDetectionPlayback(
  props.clip.camera,
  canvasRef,
  videoRef,
  props.clip.timestamp, // epoch seconds offset — detection timestamps are absolute
)

function handleKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') emit('close')
}

onMounted(async () => {
  // Pre-load detection data for this clip's time range
  const from = props.clip.timestamp
  const to = from + props.clip.duration
  await loadDetections(from, to)
})
</script>

<template>
  <Teleport to="body">
    <div class="clip-modal-overlay" @click.self="emit('close')" @keydown="handleKeydown" tabindex="0">
      <div class="clip-modal">
        <div class="clip-modal-header">
          <span>Clip {{ clip.uid }}</span>
          <div class="d-flex gap-2 align-items-center">
            <button
              class="btn btn-sm"
              :class="overlayEnabled ? 'btn-success' : 'btn-outline-secondary'"
              @click="toggleOverlay"
              title="Toggle detection overlay"
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <rect x="3" y="3" width="18" height="18" rx="2" />
                <circle cx="12" cy="12" r="3" />
              </svg>
            </button>
            <button class="btn btn-sm btn-outline-secondary" @click="emit('close')">✕</button>
          </div>
        </div>
        <div class="clip-modal-body">
          <div class="clip-video-wrap">
            <video ref="videoRef" :src="videoSrc" controls autoplay class="clip-video" />
            <canvas ref="canvasRef" class="detection-overlay" v-show="overlayEnabled" />
          </div>
        </div>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.clip-modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.85);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9999;
}
.clip-modal {
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  max-width: 90vw;
  max-height: 90vh;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.clip-modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 0.5rem 1rem;
  border-bottom: 1px solid var(--bs-border-color, #333);
}
.clip-modal-body {
  padding: 0;
}
.clip-video-wrap {
  position: relative;
}
.clip-video {
  width: 100%;
  max-height: 80vh;
  display: block;
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
</style>
