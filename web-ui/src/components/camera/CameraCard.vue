<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed, watch, inject } from 'vue'
import type { Camera } from '../../types/camera'
import { useSettingsStore } from '../../stores/settings'
import { useCameraStore } from '../../stores/cameras'
import MsePlayer from './MsePlayer.vue'
import PtzControls from './PtzControls.vue'
import { useLiveAudio } from '../../composables/useLiveAudio'
import CameraPlaybackBar from './CameraPlaybackBar.vue'
import DashboardReplayPlayer from './DashboardReplayPlayer.vue'
import { dashboardPlaybackKey } from '../../composables/useDashboardPlayback'
import StreamIndicator from './StreamIndicator.vue'
import { cameraStreamInfo } from '../../utils/streamChoice'

const props = defineProps<{
  camera: Camera
  dashboardHidden?: boolean
  controlsSelected?: boolean
  layoutMode?: boolean
  fullscreenControlsVisible?: boolean
  // Dashboards v2: chromeless wall display (fades idle chrome like fullscreen).
  kiosk?: boolean
  // Dashboards v2: per-tile stream preference; a per-device choice still wins.
  tileStream?: 'auto' | 'main' | 'sub'
  hideName?: boolean
  // Dashboards v2: double-click maximises the tile instead of exiting fullscreen.
  doubleClickMaximizes?: boolean
  // Dashboards v2: the tile's "playback bar" option.
  noPlaybackBar?: boolean
  // Prepare a changed stream in a hidden second player and swap it in once
  // its first frame is presented, instead of tearing down the visible one.
  seamlessSwitch?: boolean
  // Dashboards v2: shown before the camera name, e.g. "AUTO" in a spotlight.
  namePrefix?: string
}>()

const emit = defineEmits<{
  openStream: [cameraId: number]
  openClips: [cameraId: number]
  replay: [cameraId: number]
  dismissControls: []
  toggleMaximize: [cameraId: number]
}>()

const settings = useSettingsStore()
const streamChoice = computed(() => {
  const device = settings.getCameraStreamChoice(props.camera.id)
  return device !== 'auto' ? device : props.tileStream ?? 'auto'
})
const cameraStore = useCameraStore()
const playback = inject(dashboardPlaybackKey)
const position = computed(() => playback?.get(props.camera.id))
const reviewing = computed(() => position.value?.requestedAt != null)
function touchPlayback() { playback?.touch(props.camera.id) }
const canvasMode = computed(() => settings.fullscreenMode || props.layoutMode === true)

// MSE-only live playback; unsupported streams retain the stream-choice control.
const effectiveMode = computed(() => typeof MediaSource !== 'undefined' ? 'mse' : 'unsupported')

// Proactive HEVC detection: check if browser supports H.265 in MSE
const hevcSupported = typeof MediaSource !== 'undefined' &&
  (MediaSource.isTypeSupported('video/mp4; codecs="hev1.1.6.L93.B0"') ||
   MediaSource.isTypeSupported('video/mp4; codecs="hvc1.1.6.L93.B0"'))
const isConnected = computed(() => props.camera.status === 'Connected')
const hlsPlayerRef = ref<InstanceType<typeof MsePlayer> | null>(null)
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
let clickTimer: ReturnType<typeof setTimeout> | null = null

const measuredLatencyLabel = computed(() => {
  const ms = hlsPlayerRef.value?.latencyMs ?? 0
  if (ms === 0) return ''
  const sec = ms / 1000
  return sec >= 0 ? `+${sec.toFixed(1)}s` : `${sec.toFixed(1)}s`
})

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
  if (props.doubleClickMaximizes) {
    emit('toggleMaximize', props.camera.id)
  } else if (settings.fullscreenMode) {
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
  // Default to on; restore per-camera preference from localStorage
  const saved = localStorage.getItem(detectionStorageKey)
  const shouldEnable = saved === null || saved === '1'
  if (shouldEnable) {
    detectionOverlayActive.value = true
  }
})

// Auto-enable overlay when the MSE player becomes available.
watch(hlsPlayerRef, (player) => {
  if (player && detectionOverlayActive.value && !player.overlayEnabled) {
    player.toggleOverlay()
  }
})

// Runtime codec rejection: drop to a playable sub-stream when there is one
// (unless Main is pinned on this device); otherwise keep the failure visible,
// with stream controls available for recovery.
const codecFallback = ref(false)
const runtimeMainRejected = ref(false)
watch(() => (hlsPlayerRef.value as any)?.codecUnsupported, (unsupported: boolean | undefined) => {
  if (!unsupported) return
  if (!useSubStream.value && subPlayable.value && deviceStreamChoice.value !== 'main') runtimeMainRejected.value = true
  else codecFallback.value = true
})

// Helper to check if a codec name represents HEVC
function isHevcCodec(codec?: string): boolean {
  return /^(hevc|h265|hev1|hvc1)/.test(codec?.toLowerCase() ?? '')
}

// Proactive check for an unsupported primary HEVC stream.
const proactiveCodecFallback = computed(() => {
  if (hevcSupported) return false
  return isHevcCodec(props.camera.codec)
})

// A sub-stream exists and this browser can decode it.
const subPlayable = computed(() => !!props.camera.hasSubStream && !(isHevcCodec(props.camera.subCodec) && !hevcSupported))
const deviceStreamChoice = computed(() => settings.getCameraStreamChoice(props.camera.id))

// Use the sub-stream when chosen, or when the main stream cannot be decoded
// here (e.g. HEVC on a wall display without HEVC support) and the sub-stream
// can. Dashboards may ask for Main on large tiles; that is a preference, not
// a pin. Only an explicit per-device Main choice keeps the main stream.
const useSubStream = computed(() => {
  if (streamChoice.value === 'sub' && props.camera.hasSubStream) return true
  if (deviceStreamChoice.value === 'main') return false
  return (proactiveCodecFallback.value || runtimeMainRejected.value) && subPlayable.value
})

// Whether this browser can decode the main stream, so a sub-stream request may
// be served main while the sub-stream is unusable on the server.
const mainPlayable = computed(() => typeof MediaSource !== 'undefined' &&
  (isHevcCodec(props.camera.codec) ? hevcSupported : MediaSource.isTypeSupported('video/mp4; codecs="avc1.42001e"')))

const effectiveModeWithFallback = computed(() => {
  if (effectiveMode.value === 'unsupported' || codecFallback.value) return 'unsupported'
  const hevc = isHevcCodec(useSubStream.value ? props.camera.subCodec : props.camera.codec)
  const supported = hevc ? hevcSupported : MediaSource.isTypeSupported('video/mp4; codecs="avc1.42001e"')
  return supported ? 'mse' : 'unsupported'
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
	if (streamChoice.value !== 'auto') return false
	if (!props.camera.hasSubStream || useSubStream.value || !props.camera.subCodec) return false
	return !isHevcCodec(props.camera.subCodec) || hevcSupported
})
watch(() => [streamChoice.value, props.camera.codec, props.camera.subCodec], () => { codecFallback.value = false; selectedMseStream.value = null })
watch(() => [props.camera.codec, deviceStreamChoice.value], () => { runtimeMainRejected.value = false })

// ----- Seamless stream switching (double buffering) ------------------------
interface PlayerConfig { key: string; useSub: boolean; codecHint: string; adaptive: boolean }
const PENDING_TIMEOUT_MS = 8000
const desiredPlayer = computed<PlayerConfig>(() => ({
  key: `${props.camera.id}-${useSubStream.value ? 'forced-sub' : 'main'}-${adaptiveStreamEligible.value ? 'adaptive' : 'fixed'}`,
  useSub: useSubStream.value, codecHint: mseCodecHint.value, adaptive: adaptiveStreamEligible.value,
}))
const activePlayer = ref<PlayerConfig>(desiredPlayer.value)
const pendingPlayer = ref<PlayerConfig | null>(null)
let pendingTimer: ReturnType<typeof setTimeout> | null = null
function clearPending() {
  if (pendingTimer) clearTimeout(pendingTimer)
  pendingTimer = null
  pendingPlayer.value = null
}
function promotePending() {
  if (!pendingPlayer.value) return
  activePlayer.value = pendingPlayer.value
  clearPending()
}
watch(desiredPlayer, next => {
  if (next.key === activePlayer.value.key) { clearPending(); return }
  // Switch in place when nothing is on screen to protect (or not requested).
  if (!props.seamlessSwitch || reviewing.value || !isConnected.value || props.dashboardHidden || effectiveModeWithFallback.value !== 'mse') {
    activePlayer.value = next
    clearPending()
    return
  }
  if (pendingPlayer.value?.key === next.key) return
  if (pendingTimer) clearTimeout(pendingTimer)
  pendingPlayer.value = next
  // Never strand the switch: if the new stream shows nothing, cut over anyway.
  pendingTimer = setTimeout(promotePending, PENDING_TIMEOUT_MS)
})
const players = computed(() => pendingPlayer.value ? [activePlayer.value, pendingPlayer.value] : [activePlayer.value])
function setPlayerRef(key: string, el: unknown) {
  if (key === activePlayer.value.key && el) hlsPlayerRef.value = el as InstanceType<typeof MsePlayer>
}
function onPlayerReady(key: string) {
  if (pendingPlayer.value?.key === key) promotePending()
}
const selectedStreamInfo = computed(() => reviewing.value ? replayPlayerRef.value?.streamInfo ?? { tier: 'dvr' } :
  effectiveModeWithFallback.value === 'mse' && hlsPlayerRef.value && 'streamInfo' in hlsPlayerRef.value ? hlsPlayerRef.value.streamInfo :
  cameraStreamInfo(props.camera, useSubStream.value ? 'sub' : 'main'))

onUnmounted(() => {
  if (clickTimer) clearTimeout(clickTimer)
  if (pendingTimer) clearTimeout(pendingTimer)
})
</script>

<template>
  <div class="camera-card" :class="{ 'camera-card-fullscreen': canvasMode, 'controls-selected': controlsSelected,
    'has-playback-bar': !!playback && !noPlaybackBar, 'reviewing': reviewing, 'playback-quiet': !!playback && settings.dashboardQuietControls && !controlsSelected && !reviewing,
    'playback-idle': !!playback && (settings.fullscreenMode || kiosk) && !fullscreenControlsVisible && !reviewing && !playback.isHeld(camera.id) }"
    @pointermove="touchPlayback" @pointerdown="touchPlayback" @keydown="touchPlayback">
    <div class="camera-preview">
      <div class="camera-video-wrap" data-playback-viewport @click.prevent="onSingleClick" @dblclick.prevent="onDoubleClick">
        <!-- Recorded replay replaces live decoding for this camera. -->
        <DashboardReplayPlayer v-if="reviewing && !dashboardHidden" ref="replayPlayerRef" :camera-id="camera.id" :camera-name="camera.name" :audio-enabled="audioActive" />

        <!-- MSE preview mode -->
        <!-- MSE preview; during a seamless switch the next stream plays hidden beneath -->
        <template v-else-if="!reviewing && effectiveModeWithFallback === 'mse' && isConnected">
          <MsePlayer
            v-for="player in players"
            :key="player.key"
            :ref="(el: unknown) => setPlayerRef(player.key, el)"
            :class="{ 'mse-pending': player.key !== activePlayer.key }"
            :camera-id="camera.id"
            :use-sub-stream="player.useSub"
            :codec-hint="player.codecHint"
            :audio-enabled="player.key === activePlayer.key && audioActive && !dashboardHidden"
            :adaptive-stream="player.adaptive"
            :allow-main-fallback="player.useSub && mainPlayable"
            @stream-changed="(stream: 'main' | 'sub') => { if (player.key === activePlayer.key) onStreamChanged(stream) }"
            @ready="onPlayerReady(player.key)"
          />
        </template>

        <div v-else-if="!reviewing && isConnected && effectiveModeWithFallback === 'unsupported'" class="camera-overlay" @click.stop role="status">
          <div class="text-center p-3">Video playback unavailable<br><small>This browser cannot play the selected stream. Use stream details to choose Auto or Sub.</small></div>
        </div>

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

        <!-- Camera name overlay in fullscreen -->
        <div v-if="canvasMode && (!hideName || namePrefix)" class="camera-name-overlay">
          <span v-if="namePrefix" class="camera-name-prefix">{{ namePrefix }}</span><template v-if="namePrefix && !hideName"> · </template><template v-if="!hideName">{{ camera.name }}</template>
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
        <StreamIndicator :camera-id="camera.id"
          :label="reviewing ? 'DVR' : effectiveModeWithFallback === 'unsupported' ? 'Unsupported' : latencyLabel || 'U'"
          :codec-fallback="!reviewing && effectiveModeWithFallback === 'unsupported'"
          :info="selectedStreamInfo" :has-sub="camera.hasSubStream" :replay="reviewing" />

        <!-- Live audio toggle -->
        <button
          v-if="(reviewing || effectiveModeWithFallback === 'mse') && isConnected"
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
          v-if="(reviewing || effectiveModeWithFallback === 'mse') && isConnected"
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
          v-if="(reviewing || effectiveModeWithFallback === 'mse') && isConnected"
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
    <CameraPlaybackBar v-if="playback && !noPlaybackBar" :camera-id="camera.id" :hidden="dashboardHidden" />
  </div>
</template>

<style scoped>
/* Prepared stream: decoding and laid out at the tile's size (so its first
   frame is real), but not visible. Near-zero rather than zero opacity keeps
   browsers compositing it, which first-frame detection relies on. */
.mse-pending { position: absolute; inset: 0; opacity: .01; pointer-events: none; }
.reviewing:has(.dvr-error) :is(.audio-toggle, .motion-audio-toggle, .detection-toggle) { visibility: hidden; }
.has-playback-bar { position: relative; }
.camera-card-fullscreen > :deep(.camera-playback-bar) { position: absolute; bottom: 0; left: 0; right: 0; z-index: 30; }
/* Name label and stream indicator share the row above the playback bar; the
   audio/detection/PTZ buttons sit one row above them when they appear. */
.camera-card-fullscreen.has-playback-bar :is(.audio-toggle, .motion-audio-toggle, .detection-toggle, .ptz-toggle) { bottom: calc(var(--camera-playback-height, 82px) + 40px); }
.camera-card-fullscreen.has-playback-bar .camera-name-overlay { bottom: calc(var(--camera-playback-height, 82px) + 8px); }
.camera-name-prefix { color: #9fd0f7; font-size: .68rem; font-weight: 700; letter-spacing: .06em; }
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
/* Dashboards: the playback bar overlays the card bottom. Lift the stream
   indicator (latency, stream choice, codec fallback) to the same row as the
   audio/detection buttons, bottom-right, as in the tile view. */
.camera-card-fullscreen.has-playback-bar :deep(.stream-indicator) { bottom: calc(var(--camera-playback-height, 82px) + 8px); }
.camera-card-fullscreen.has-playback-bar.playback-idle :deep(.stream-indicator) { bottom: 8px; }
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
