<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import MsePlayer from '../components/camera/MsePlayer.vue'
import { useCameraStore } from '../stores/cameras'
import { useSettingsStore } from '../stores/settings'
import { useLiveAudio } from '../composables/useLiveAudio'
import StreamIndicator from '../components/camera/StreamIndicator.vue'
import { cameraStreamInfo } from '../utils/streamChoice'

const route = useRoute()
const router = useRouter()
const cameraStore = useCameraStore()
const settings = useSettingsStore()

const cameraId = computed(() => Number(route.params.cameraId))
const camera = computed(() => cameraStore.getCameraById(cameraId.value))
const runtimeUnsupported = ref(false)
const supportsCodec = (codec?: string) => typeof MediaSource !== 'undefined' &&
  (isHevcCodec(codec) ? ['hev1.1.6.L93.B0', 'hvc1.1.6.L93.B0'] : ['avc1.42001e']).some(value => MediaSource.isTypeSupported(`video/mp4; codecs="${value}"`))
const useSubStream = computed(() => {
  const choice = settings.getCameraStreamChoice(cameraId.value)
  return !!camera.value?.hasSubStream && (choice === 'sub' || (choice === 'auto' && !supportsCodec(camera.value?.codec) && supportsCodec(camera.value?.subCodec)))
})

function isHevcCodec(codec?: string): boolean {
  return /^(hevc|h265|hev1|hvc1)/.test(codec?.toLowerCase() ?? '')
}

const mseCodecHint = computed(() =>
  isHevcCodec(useSubStream.value ? camera.value?.subCodec : camera.value?.codec) ? 'hev1.1.6.L93.B0' : 'avc1.42001e',
)
const unsupported = computed(() => runtimeUnsupported.value || !supportsCodec(useSubStream.value ? camera.value?.subCodec : camera.value?.codec))
const hlsPlayerRef = ref<InstanceType<typeof MsePlayer> | null>(null)
watch(() => hlsPlayerRef.value?.codecUnsupported, value => { if (value) runtimeUnsupported.value = true })
watch(() => [cameraId.value, settings.getCameraStreamChoice(cameraId.value), camera.value?.codec, camera.value?.subCodec], () => { runtimeUnsupported.value = false })
const streamInfo = computed(() => hlsPlayerRef.value && 'streamInfo' in hlsPlayerRef.value ? hlsPlayerRef.value.streamInfo :
  camera.value ? cameraStreamInfo(camera.value, useSubStream.value ? 'sub' : 'main') : {})
const detectionOverlayActive = ref(false)
const {
  mode: audioMode,
  enabled: audioActive,
  toggleAudio,
  toggleMotionAudio,
} = useLiveAudio(() => cameraId.value, () => camera.value?.motionActive ?? false)

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

// Auto-enable overlay when the MSE player becomes available.
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
      <button
        class="btn btn-sm"
        :class="audioActive ? 'btn-success' : 'btn-outline-secondary'"
        @click="toggleAudio"
        :title="audioActive ? 'Mute live audio' : 'Play live audio'"
      >{{ audioActive ? '🔊' : '🔇' }}</button>
      <button
        class="btn btn-sm"
        :class="audioMode === 'motion' ? (audioActive ? 'btn-success' : 'btn-info') : 'btn-outline-secondary'"
        @click="toggleMotionAudio"
        :title="audioMode === 'motion' ? 'Disable audio on motion' : 'Enable audio while motion is active'"
      >Motion audio</button>
      <button class="btn btn-sm btn-outline-secondary" @click="router.push('/')">
        ← Back
      </button>
    </template>

    <div v-if="camera" class="stream-container" @click="cameraStore.toggleRecording(cameraId)">
      <div v-if="unsupported" class="alert alert-warning m-3" @click.stop role="status">Video playback is unavailable for this stream in this browser. Choose Auto or a supported Sub stream in stream details.</div>
      <MsePlayer
        v-else
        :key="`${cameraId}:${mseCodecHint}:${useSubStream}`"
        ref="hlsPlayerRef"
        :camera-id="cameraId"
        suffix="_stream"
        :codec-hint="mseCodecHint"
        :use-sub-stream="useSubStream"
        :audio-enabled="audioActive"
      />
      <div v-if="camera.isRecording" class="rec-overlay">
        <span class="rec-dot" /> REC
      </div>
      <StreamIndicator :camera-id="cameraId" :label="unsupported ? 'Unsupported' : latencyLabel || 'Stream'" :codec-fallback="unsupported" :info="streamInfo" :has-sub="camera.hasSubStream" />
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
