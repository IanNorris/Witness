<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed, watch, inject } from 'vue'
import type { Camera } from '../../types/camera'
import { useSettingsStore } from '../../stores/settings'
import { useCameraStore } from '../../stores/cameras'
import HlsPlayer from './HlsPlayer.vue'
import MsePlayer from './MsePlayer.vue'
import PtzControls from './PtzControls.vue'
import { useLiveAudio } from '../../composables/useLiveAudio'
import CameraPlaybackBar from './CameraPlaybackBar.vue'
import DashboardReplayPlayer from './DashboardReplayPlayer.vue'
import { dashboardPlaybackKey } from '../../composables/useDashboardPlayback'

const props = defineProps<{
  camera: Camera
  dashboardHidden?: boolean
  controlsSelected?: boolean
  layoutMode?: boolean
  fullscreenControlsVisible?: boolean
}>()

const emit = defineEmits<{
  openStream: [cameraId: number]
  openClips: [cameraId: number]
  replay: [cameraId: number]
  dismissControls: []
}>()

const settings = useSettingsStore()
const cameraStore = useCameraStore()
const playback = inject(dashboardPlaybackKey)
const position = computed(() => playback?.get(props.camera.id))
const reviewing = computed(() => position.value?.requestedAt != null)
function touchPlayback() { playback?.touch(props.camera.id) }
const canvasMode = computed(() => settings.fullscreenMode || props.layoutMode === true)

// MSE requires MediaSource API — fall back to HLS if unavailable
const effectiveMode = computed(() => {
  if (settings.streamingMode === 'mse') {
    if (typeof MediaSource === 'undefined' || !MediaSource.isTypeSupported('video/mp4; codecs="avc1.42001e"')) {
      return 'hls'
    }
  }
  return settings.streamingMode
})

// Proactive HEVC detection: check if browser supports H.265 in MSE
const hevcSupported = typeof MediaSource !== 'undefined' &&
  (MediaSource.isTypeSupported('video/mp4; codecs="hev1.1.6.L93.B0"') ||
   MediaSource.isTypeSupported('video/mp4; codecs="hvc1.1.6.L93.B0"'))
const imgSrc = ref('')
const isConnected = ref(false)
const imgRef = ref<HTMLImageElement | null>(null)
const hlsPlayerRef = ref<InstanceType<typeof HlsPlayer> | InstanceType<typeof MsePlayer> | null>(null)
const replayPlayerRef = ref<InstanceType<typeof DashboardReplayPlayer> | null>(null)
const detectionOverlayActive = ref(false)
const selectedMseStream = ref<'main' | 'sub' | null>(null)
const showPtzControls = ref(false)
watch(reviewing, value => { if (value) showPtzControls.value = false })
watch(() => [canvasMode.value, props.controlsSelected, props.dashboardHidden, props.camera.id, props.camera.ptzEnabled, props.camera.status], () => {
  if (props.dashboardHidden || !props.camera.ptzEnabled || props.camera.status !== 'Connected' ||
    (canvasMode.value && !props.controlsSelected)) showPtzControls.value = false
})
const {
  mode: audioMode,
  enabled: audioActive,
  toggleAudio,
  toggleMotionAudio,
} = useLiveAudio(() => props.camera.id, () => props.camera.motionActive)
const detectionStorageKey = `witness-detection-overlay-${props.camera.id}`
let refreshTimer: ReturnType<typeof setInterval> | null = null
let jpegRunning = false
let clickTimer: ReturnType<typeof setTimeout> | null = null

const measuredLatencyLabel = computed(() => {
  const ms = hlsPlayerRef.value?.latencyMs ?? 0
  if (ms === 0) return ''
  const sec = ms / 1000
  return sec >= 0 ? `+${sec.toFixed(1)}s` : `${sec.toFixed(1)}s`
})

function refreshPreview() {
  if (props.camera.status === 'Connected') {
    isConnected.value = true
    imgSrc.value = `/camera/preview/${props.camera.id}?t=${Date.now()}`
  } else {
    isConnected.value = false
  }
}

// Continuous JPEG: reload as soon as previous frame loads
function startJpegLoop() {
  jpegRunning = true
  loadNextFrame()
}

function loadNextFrame() {
  if (!jpegRunning) return
  if (reviewing.value) { refreshTimer = setTimeout(loadNextFrame, 1000); return }
  const img = imgRef.value
  if (!img) {
    refreshTimer = setTimeout(loadNextFrame, 100)
    return
  }
  imgSrc.value = `/camera/preview/${props.camera.id}?t=${Date.now()}`
}

function onJpegLoad() {
  if (jpegRunning) {
    refreshTimer = setTimeout(loadNextFrame, 33)
  }
}

function onJpegError() {
  if (jpegRunning) {
    refreshTimer = setTimeout(loadNextFrame, 1000)
  }
}

// Fullscreen clicks select controls; recording is explicit there. Tile-view
// single click retains its existing recording shortcut.
function onSingleClick() {
  if (playback) { playback.selected.value = props.camera.id; touchPlayback(); return }
  if (canvasMode.value) return
  if (clickTimer) { clearTimeout(clickTimer); clickTimer = null }
  clickTimer = setTimeout(() => {
    clickTimer = null
    if (!canvasMode.value) cameraStore.toggleRecording(props.camera.id)
  }, 250)
}

function onDoubleClick() {
  if (clickTimer) { clearTimeout(clickTimer); clickTimer = null }
  if (settings.fullscreenMode) {
    settings.toggleFullscreen()
  } else {
    emit('openStream', props.camera.id)
  }
}

function toggleDetectionOverlay() {
  const player = reviewing.value ? replayPlayerRef.value : hlsPlayerRef.value
  if (player?.toggleOverlay) {
    player.toggleOverlay()
    detectionOverlayActive.value = player.overlayEnabled ?? false
    localStorage.setItem(detectionStorageKey, detectionOverlayActive.value ? '1' : '0')
  }
}

onMounted(() => {
  refreshPreview()
  if (effectiveMode.value === 'jpeg') {
    startJpegLoop()
  }
  // Default to on; restore per-camera preference from localStorage
  const saved = localStorage.getItem(detectionStorageKey)
  const shouldEnable = saved === null || saved === '1'
  if (shouldEnable) {
    detectionOverlayActive.value = true
  }
})

// Auto-enable overlay when HlsPlayer becomes available
watch(hlsPlayerRef, (player) => {
  if (player && detectionOverlayActive.value && !player.overlayEnabled) {
    player.toggleOverlay()
  }
})

// Auto-fallback when codec is unsupported at runtime (e.g. HEVC without browser support)
// Works for both HLS and MSE player components which both expose codecUnsupported
const codecFallback = ref(false)
watch(() => (hlsPlayerRef.value as any)?.codecUnsupported, (unsupported: boolean | undefined) => {
  if (unsupported) {
    codecFallback.value = true
  }
})

// Helper to check if a codec name represents HEVC
function isHevcCodec(codec?: string): boolean {
  return codec === 'hevc' || codec === 'h265' || codec === 'hev1'
}

// Proactive check: if camera reports HEVC and browser doesn't support it, skip HLS entirely
const proactiveCodecFallback = computed(() => {
  if (hevcSupported) return false
  return isHevcCodec(props.camera.codec)
})

// Determine if we should use the sub-stream (H.264 fallback) instead of JPEG
// Only use sub-stream if it exists AND its codec is actually supported
const useSubStream = computed(() => {
  if (!proactiveCodecFallback.value) return false
  if (!props.camera.hasSubStream) return false
  // Don't use sub-stream if it's also HEVC and browser doesn't support it
  if (isHevcCodec(props.camera.subCodec) && !hevcSupported) return false
  return true
})

const effectiveModeWithFallback = computed(() => {
  // If main codec unsupported and no viable sub-stream, fall back to JPEG
  if ((codecFallback.value || proactiveCodecFallback.value) && !useSubStream.value) return 'jpeg'
  // Sub-stream always uses MSE (WebSocket-based) since we have a WS route for it
  if (useSubStream.value) return 'mse'
  return effectiveMode.value
})

const latencyLabel = computed(() => {
  const measured = measuredLatencyLabel.value
  if (effectiveModeWithFallback.value !== 'mse') return measured
  const streamCode = selectedMseStream.value === 'main'
    ? 'M'
    : selectedMseStream.value === 'sub'
      ? 'P'
      : 'U'
  return measured ? `${measured} ${streamCode}` : streamCode
})

function onStreamChanged(stream: 'main' | 'sub') {
  selectedMseStream.value = stream
}

// Codec hint for MSE SourceBuffer — use the correct codec string for the stream we're actually playing
const mseCodecHint = computed(() => {
  const codec = useSubStream.value ? props.camera.subCodec : props.camera.codec
  if (isHevcCodec(codec)) return 'hev1.1.6.L93.B0'
  // Default H.264 baseline
  return 'avc1.42001e'
})

const adaptiveStreamEligible = computed(() => {
	if (!props.camera.hasSubStream || useSubStream.value || !props.camera.subCodec) return false
	return !isHevcCodec(props.camera.subCodec) || hevcSupported
})

onUnmounted(() => {
  jpegRunning = false
  if (refreshTimer) clearTimeout(refreshTimer)
  if (clickTimer) clearTimeout(clickTimer)
})
</script>

<template>
  <div class="camera-card" :class="{ 'camera-card-fullscreen': canvasMode, 'controls-selected': controlsSelected,
    'has-playback-bar': !!playback, 'reviewing': reviewing, 'playback-quiet': !!playback && settings.dashboardQuietControls && !controlsSelected && !reviewing,
    'playback-idle': !!playback && settings.fullscreenMode && !fullscreenControlsVisible && !reviewing && !playback.isHeld(camera.id) }"
    @pointermove="touchPlayback" @pointerdown="touchPlayback" @keydown="touchPlayback">
    <div class="camera-preview">
      <div class="camera-video-wrap" data-playback-viewport @click.prevent="onSingleClick" @dblclick.prevent="onDoubleClick">
        <!-- HLS preview mode -->
        <DashboardReplayPlayer v-if="reviewing && !dashboardHidden" ref="replayPlayerRef" :camera-id="camera.id" :camera-name="camera.name" :audio-enabled="audioActive" />
        <HlsPlayer
          v-else-if="!reviewing && effectiveModeWithFallback === 'hls' && isConnected"
          ref="hlsPlayerRef"
          :camera-id="camera.id"
          :low-latency="camera.lowLatencyHLS"
          :audio-enabled="audioActive && !dashboardHidden"
        />

        <!-- MSE preview mode -->
        <MsePlayer
          v-else-if="!reviewing && effectiveModeWithFallback === 'mse' && isConnected"
		  :key="`${camera.id}-${useSubStream ? 'forced-sub' : 'main'}-${adaptiveStreamEligible ? 'adaptive' : 'fixed'}`"
          ref="hlsPlayerRef"
          :camera-id="camera.id"
          :use-sub-stream="useSubStream"
          :codec-hint="mseCodecHint"
          :audio-enabled="audioActive && !dashboardHidden"
          :adaptive-stream="adaptiveStreamEligible"
          @stream-changed="onStreamChanged"
        />

        <!-- JPEG preview mode -->
        <img
          v-else-if="!reviewing && imgSrc && effectiveModeWithFallback === 'jpeg'"
          ref="imgRef"
          :src="imgSrc"
          :alt="camera.name"
          @load="onJpegLoad"
          @error="onJpegError"
        />

        <!-- Disconnected state -->
        <div v-if="!isConnected && !reviewing" class="camera-overlay connection-lost">
          <div class="text-center">
            <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <line x1="1" y1="1" x2="23" y2="23"/><path d="M16.72 11.06A10.94 10.94 0 0 1 19 12.55"/><path d="M5 12.55a10.94 10.94 0 0 1 5.17-2.39"/>
            </svg>
            <div class="mt-1">Disconnected</div>
          </div>
        </div>

        <!-- REC overlay -->
        <div v-if="camera.isRecording" class="rec-overlay">
          <span class="rec-dot" /> REC
        </div>

        <!-- H.265 fallback indicator -->
        <div v-if="!reviewing && proactiveCodecFallback && !useSubStream && isConnected" class="codec-fallback-overlay">
          H.265 → JPEG
        </div>

        <!-- Camera name overlay in fullscreen -->
        <div v-if="canvasMode" class="camera-name-overlay">
          {{ camera.name }}
        </div>
        <button v-if="canvasMode && controlsSelected" class="control-selection-label"
          @click.stop="emit('dismissControls')" title="Dismiss camera controls (Esc)">
          Controls selected · ×
        </button>
        <button v-if="canvasMode && controlsSelected" class="fullscreen-record-toggle"
          :class="{ recording: camera.isRecording }" @click.stop="cameraStore.toggleRecording(camera.id)"
          :aria-label="camera.isRecording ? 'Stop recording' : 'Start recording'"
          :aria-pressed="camera.isRecording" :title="camera.isRecording ? 'Stop manual recording' : 'Start manual recording'">
          {{ camera.isRecording ? '■ Stop recording' : '● Record' }}
        </button>

        <!-- Latency overlay -->
        <button v-if="!playback && canvasMode && controlsSelected" class="fullscreen-replay-toggle"
          @click.stop="emit('replay', camera.id)" title="Review recent DVR footage">↶ Replay</button>
        <div v-if="!reviewing && latencyLabel && effectiveMode !== 'jpeg'" class="latency-overlay">
          {{ latencyLabel }}
        </div>

        <!-- Live audio toggle -->
        <button
          v-if="effectiveMode !== 'jpeg' && isConnected"
          class="btn btn-sm audio-toggle"
          :class="audioActive ? 'btn-success' : 'btn-outline-secondary'"
          @click.stop="toggleAudio"
          :title="audioActive ? 'Mute live audio' : 'Play live audio'"
          :aria-label="audioActive ? 'Mute live audio' : 'Play live audio'"
          :aria-pressed="audioActive"
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
            <path d="M11 5 6 9H3v6h3l5 4z" />
            <path v-if="audioActive" d="M15 8a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14" />
            <path v-else d="m16 9 5 6m0-6-5 6" />
          </svg>
        </button>

        <button
          v-if="effectiveMode !== 'jpeg' && isConnected"
          class="btn btn-sm motion-audio-toggle"
          :class="audioMode === 'motion' ? (audioActive ? 'btn-success' : 'btn-info') : 'btn-outline-secondary'"
          @click.stop="toggleMotionAudio"
          :title="audioMode === 'motion' ? 'Disable audio on motion' : 'Enable audio while motion is active'"
          :aria-label="audioMode === 'motion' ? 'Disable audio on motion' : 'Enable audio while motion is active'"
          :aria-pressed="audioMode === 'motion'"
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
            <path d="M2 12h4l3-8 6 16 3-8h4" />
          </svg>
        </button>

        <!-- Detection overlay toggle -->
        <button
          v-if="effectiveMode !== 'jpeg' && isConnected"
          class="btn btn-sm detection-toggle"
          :class="detectionOverlayActive ? 'btn-success' : 'btn-outline-secondary'"
          @click.stop="toggleDetectionOverlay"
          title="Toggle detection overlay"
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="3" y="3" width="18" height="18" rx="2" />
            <circle cx="12" cy="12" r="3" />
          </svg>
        </button>

        <!-- PTZ toggle button -->
        <button
          v-if="!reviewing && camera.ptzEnabled && isConnected"
          class="btn btn-sm ptz-toggle"
          :class="showPtzControls ? 'btn-primary' : 'btn-outline-secondary'"
          @click.stop="showPtzControls = !showPtzControls"
          title="PTZ controls"
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <circle cx="12" cy="12" r="3"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/>
          </svg>
        </button>

        <!-- PTZ controls overlay -->
        <Teleport to="body" :disabled="!canvasMode">
          <div v-if="showPtzControls && camera.ptzEnabled" class="ptz-overlay"
            :class="{ 'ptz-floating-panel': canvasMode }"
            :data-ptz-controls-camera="camera.id" @click.stop>
            <div v-if="canvasMode" class="ptz-panel-heading">
              <span>{{ camera.name }}</span>
              <button aria-label="Close PTZ controls" @click="showPtzControls = false">×</button>
            </div>
            <PtzControls :camera-id="camera.id" />
          </div>
        </Teleport>
      </div>
    </div>

    <!-- Info bar: hidden in fullscreen -->
    <div v-if="!canvasMode" class="camera-info">
      <div class="camera-details">
        <div class="camera-name">{{ camera.name }}</div>
        <div class="camera-status" :class="{ 'is-connected': isConnected }">
          <span class="camera-status-dot" aria-hidden="true" />
          <span>{{ camera.status }}</span>
        </div>
      </div>
      <div class="d-flex gap-1">
        <button v-if="!playback" class="btn btn-sm btn-outline-secondary" @click.stop="emit('replay', camera.id)"
          title="Review recent DVR footage" aria-label="Review recent DVR footage">↶</button>
        <button
          class="btn btn-sm btn-outline-secondary"
          @click.stop="emit('openClips', camera.id)"
          title="Clips"
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polygon points="23 7 16 12 23 17 23 7"/><rect x="1" y="5" width="15" height="14" rx="2"/>
          </svg>
        </button>
        <button
          class="btn btn-sm btn-outline-secondary"
          @click.stop="emit('openStream', camera.id)"
          title="Live stream"
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polygon points="5 3 19 12 5 21 5 3"/>
          </svg>
        </button>
      </div>
    </div>
    <div v-if="reviewing && position?.viewedAt" class="dashboard-replay-badge">Replay · {{ new Date(position.viewedAt * 1000).toLocaleTimeString() }}</div>
    <CameraPlaybackBar v-if="playback" :camera-id="camera.id" :hidden="dashboardHidden" />
  </div>
</template>

<style scoped>
.has-playback-bar { position: relative; }
.camera-card-fullscreen > :deep(.camera-playback-bar) { position: absolute; bottom: 0; left: 0; right: 0; z-index: 30; }
.camera-card-fullscreen.has-playback-bar :is(.audio-toggle, .motion-audio-toggle, .detection-toggle, .ptz-toggle) { bottom: calc(var(--camera-playback-height, 82px) + 8px); }
.camera-card-fullscreen.has-playback-bar .camera-name-overlay { bottom: calc(var(--camera-playback-height, 82px) + 44px); }
.camera-card-fullscreen.has-playback-bar.playback-idle .camera-name-overlay { bottom: 8px; }
.camera-card-fullscreen.reviewing .camera-preview { height: calc(100% - var(--camera-playback-height, 82px)); }
.dashboard-replay-badge { position: absolute; top: 38px; left: 8px; z-index: 15; background: #142235ee; color: #f5d791; border-radius: 4px; padding: 3px 6px; font-size: 11px; pointer-events: none; }
.has-playback-bar :deep(.camera-playback-bar), .has-playback-bar :is(.audio-toggle, .motion-audio-toggle, .detection-toggle, .ptz-toggle, .control-selection-label, .fullscreen-record-toggle) { transition: opacity .2s; }
.playback-quiet:not(:hover):not(:focus-within) :deep(.playback-buttons) { opacity: .25; }
.playback-quiet:not(:hover):not(:focus-within) :deep(.playback-track input) { opacity: 0; }
.has-playback-bar.playback-idle:not(:has(:focus-visible)) :deep(.camera-playback-bar), .has-playback-bar.playback-idle:not(:has(:focus-visible)) :is(.audio-toggle, .motion-audio-toggle, .detection-toggle, .ptz-toggle, .control-selection-label, .fullscreen-record-toggle) { opacity: 0; pointer-events: none; }
.has-playback-bar.playback-idle::after { opacity: 0; }
.reviewing .camera-preview { cursor: default; }
@media (pointer: coarse) { .playback-quiet :deep(.playback-buttons), .playback-quiet :deep(.playback-track input) { opacity: 1 !important; } }
@media (prefers-reduced-motion: reduce) { .has-playback-bar :deep(.camera-playback-bar) { transition: none; } }
.camera-card-fullscreen.controls-selected { position: relative; }
.camera-card-fullscreen.controls-selected::after {
  content: ''; position: absolute; inset: 0; border: 2px solid #67b5ff;
  z-index: 25; pointer-events: none;
}
.control-selection-label {
  position: absolute; top: 8px; left: 8px; z-index: 10;
  border: 1px solid #67b5ff; border-radius: 5px; background: #142940e8;
  color: #e8f3ff; padding: 3px 7px; font-size: 11px;
}
.fullscreen-record-toggle {
  position: absolute; top: 8px; right: 8px; z-index: 10;
  border: 1px solid #bdcedf; border-radius: 5px; background: #142940e8;
  color: #e8f3ff; padding: 3px 7px; font-size: 11px;
}
.fullscreen-record-toggle.recording { border-color: #ff7e83; color: #ffb5b8; }
.fullscreen-replay-toggle { position: absolute; top: 40px; right: 8px; z-index: 10; border: 1px solid #bdcedf; border-radius: 5px; background: #142940e8; color: #e8f3ff; padding: 3px 7px; font-size: 11px; }
.ptz-floating-panel {
  position: fixed; bottom: 24px; right: 24px; left: auto; z-index: 2000;
  max-height: calc(100dvh - 48px); max-width: calc(100vw - 48px); overflow: auto;
  background: #142235; border: 1px solid #61758e; border-radius: 8px;
  box-shadow: 0 8px 30px #0009;
}
.ptz-panel-heading { display: flex; justify-content: space-between; align-items: center; gap: 12px; padding: 7px 10px; color: #e8f3ff; font-size: 12px; }
.ptz-panel-heading span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ptz-panel-heading button { color: #e8f3ff; border: none; border-radius: 4px; background: #34465d; font-size: 18px; line-height: 20px; }
.camera-card-fullscreen:not(.controls-selected):not(:hover) :is(.audio-toggle, .motion-audio-toggle, .detection-toggle, .ptz-toggle) {
  opacity: 0; pointer-events: none;
}
.camera-card-fullscreen.controls-selected :is(.audio-toggle, .motion-audio-toggle, .detection-toggle, .ptz-toggle) {
  opacity: 1; pointer-events: auto;
}
</style>
