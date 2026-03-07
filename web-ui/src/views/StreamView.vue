<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import HlsPlayer from '../components/camera/HlsPlayer.vue'
import MsePlayer from '../components/camera/MsePlayer.vue'
import { useCameraStore } from '../stores/cameras'
import { useSettingsStore } from '../stores/settings'

const route = useRoute()
const router = useRouter()
const cameraStore = useCameraStore()
const settings = useSettingsStore()

const effectiveMode = computed(() => {
  if (settings.streamingMode === 'mse') {
    if (typeof MediaSource === 'undefined' || !MediaSource.isTypeSupported('video/mp4; codecs="avc1.42001e"')) {
      return 'hls'
    }
  }
  return settings.streamingMode
})

const cameraId = computed(() => Number(route.params.cameraId))
const camera = computed(() => cameraStore.getCameraById(cameraId.value))
const hlsPlayerRef = ref<InstanceType<typeof HlsPlayer> | InstanceType<typeof MsePlayer> | null>(null)
const detectionOverlayActive = ref(false)

const latencyLabel = computed(() => {
  const ms = hlsPlayerRef.value?.latencyMs ?? 0
  if (ms === 0) return ''
  const sec = ms / 1000
  return sec >= 0 ? `+${sec.toFixed(1)}s` : `${sec.toFixed(1)}s`
})

function toggleDetectionOverlay() {
  const player = hlsPlayerRef.value
  if (player?.toggleOverlay) {
    player.toggleOverlay()
    detectionOverlayActive.value = player.overlayEnabled ?? false
    localStorage.setItem(`witness-detection-overlay-${cameraId.value}`, detectionOverlayActive.value ? '1' : '0')
  }
}

onMounted(async () => {
  if (cameraStore.cameras.length === 0) {
    await cameraStore.fetchCameras()
  }
  const saved = localStorage.getItem(`witness-detection-overlay-${cameraId.value}`)
  if (saved === null || saved === '1') {
    detectionOverlayActive.value = true
  }
})

// Auto-enable overlay when HlsPlayer becomes available
watch(hlsPlayerRef, (player) => {
  if (player && detectionOverlayActive.value && !player.overlayEnabled) {
    player.toggleOverlay()
  }
})
</script>

<template>
  <AppLayout>
    <template #title>{{ camera?.name || 'Stream' }}</template>
    <template #actions>
      <button
        class="btn btn-sm"
        :class="detectionOverlayActive ? 'btn-success' : 'btn-outline-secondary'"
        @click="toggleDetectionOverlay"
        title="Toggle detection overlay"
      >🔲</button>
      <button class="btn btn-sm btn-outline-secondary" @click="router.push('/')">
        ← Back
      </button>
    </template>

    <div v-if="camera" class="stream-container" @click="cameraStore.toggleRecording(cameraId)">
      <MsePlayer v-if="effectiveMode === 'mse'" ref="hlsPlayerRef" :camera-id="cameraId" suffix="_stream" />
      <HlsPlayer v-else ref="hlsPlayerRef" :camera-id="cameraId" suffix="_stream" :debug="true" :low-latency="camera.lowLatencyHLS" />
      <div v-if="camera.isRecording" class="rec-overlay">
        <span class="rec-dot" /> REC
      </div>
      <div v-if="latencyLabel" class="latency-overlay">
        {{ latencyLabel }}
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
.latency-overlay {
  position: absolute;
  bottom: 8px;
  right: 8px;
  color: rgba(255, 255, 255, 0.7);
  font-size: 0.75rem;
  font-weight: 400;
  background: rgba(0, 0, 0, 0.5);
  padding: 1px 8px;
  border-radius: 3px;
  pointer-events: none;
  z-index: 2;
  font-variant-numeric: tabular-nums;
}
</style>
