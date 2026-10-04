<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed, watch } from 'vue'
import { api } from '../../composables/useApi'
import { createPtzMovement } from '../../composables/ptzMovement'

const props = defineProps<{
  cameraId: number
}>()

const isMoving = ref(false)
const error = ref('')
const speed = ref(32)
const zoomPos = ref(0)
const zoomMin = ref(0)
const zoomMax = ref(33)
const zoomLoaded = ref(false)
const presets = ref<Array<{ id: number; name: string }>>([])
const presetId = ref<number | ''>('')
const presetsLoading = ref(false)
const presetsUnavailable = ref(false)
const presetBusy = ref(false)
let zoomSendTimeout: ReturnType<typeof setTimeout> | null = null
let zoomRevision = 0
let zoomPending = 0
let alive = true
let cameraRevision = 0
let zoomFetchPending = false
let zoomUnavailable = false

const zoomMultiplier = computed(() => {
  if (zoomMax.value <= zoomMin.value) return '1.0x'
  const range = zoomMax.value - zoomMin.value
  const fraction = (zoomPos.value - zoomMin.value) / range
  return (fraction * 100).toFixed(0) + '%'
})

const movement = createPtzMovement(
  (cameraId, command, moveSpeed) => api(`/ptz/${cameraId}/${command}`, {
    method: 'POST', body: { speed: moveSpeed }, redirectOnFail: false,
  }),
  moving => { isMoving.value = moving },
  (command, failure) => { error.value = `${command === 'stop' ? 'Stop failed' : command === 'position' ? 'Position change failed' : 'Movement failed'}: ${failure instanceof Error ? failure.message : String(failure)}` },
)
let heldPointer: number | null = null

function startMove(command: string) {
  error.value = ''
  movement.start(props.cameraId, command, speed.value)
}

function stopMove() {
  heldPointer = null
  movement.stop()
}
function onMovePointer(event: PointerEvent, command: string) {
  if (event.button !== 0 || !event.isPrimary) return
  heldPointer = event.pointerId
  try { (event.currentTarget as HTMLElement).setPointerCapture(event.pointerId) }
  catch { heldPointer = null; return }
  startMove(command)
}
function onMoveKey(event: KeyboardEvent, command: string) {
  if (!event.repeat) startMove(command)
}
function onPointerStop(event: PointerEvent) {
  if (event.pointerId === heldPointer) stopMove()
}
function onVisibility() {
  if (document.visibilityState !== 'visible') stopMove()
}

async function fetchPresets() {
  const cameraId = props.cameraId
  const revision = cameraRevision
  presetsLoading.value = true
  presetsUnavailable.value = false
  try {
    const data = await api<{ presets: Array<{ id: number; name: string; enabled: boolean }> }>(
      `/ptz/${cameraId}/presets`, { redirectOnFail: false },
    )
    if (alive && cameraId === props.cameraId && revision === cameraRevision) {
      presets.value = (data?.presets ?? []).filter(p => p.enabled && Number.isInteger(p.id) && p.id >= 0 && p.id <= 65535)
        .map(p => ({ id: p.id, name: p.name || `Position ${p.id}` }))
    }
  } catch {
    if (alive && cameraId === props.cameraId && revision === cameraRevision) presetsUnavailable.value = true
  } finally {
    if (alive && cameraId === props.cameraId && revision === cameraRevision) presetsLoading.value = false
  }
}
async function goToPreset() {
  const cameraId = props.cameraId
  const revision = cameraRevision
  const id = presetId.value
  if (presetBusy.value || id === '' || !presets.value.some(p => p.id === id)) return
  presetBusy.value = true
  error.value = ''
  await movement.position(async () => {
    // Selecting another camera or dismissing controls cancels undispatched work.
    if (!alive || cameraId !== props.cameraId || revision !== cameraRevision) return
    await api(`/ptz/${cameraId}/preset`, { method: 'POST', body: { id }, redirectOnFail: false })
  })
  if (alive && cameraId === props.cameraId && revision === cameraRevision) presetBusy.value = false
}

async function fetchZoomState() {
  const cameraId = props.cameraId
  const revision = zoomRevision
  if (isMoving.value || zoomSendTimeout || zoomPending || zoomFetchPending || zoomUnavailable) return
  zoomFetchPending = true
  try {
    const data = await api<{ valid: boolean; zoom: number; zoomMin: number; zoomMax: number }>(
      `/ptz/${cameraId}/zoom`, { redirectOnFail: false }
    )
    if (alive && cameraId === props.cameraId && revision === zoomRevision && data?.valid && !zoomSendTimeout && !zoomPending) {
      zoomPos.value = data.zoom
      if (data.zoomMin !== undefined) zoomMin.value = data.zoomMin
      if (data.zoomMax > 0) zoomMax.value = data.zoomMax
      zoomLoaded.value = true
    }
    if (alive && cameraId === props.cameraId && revision === zoomRevision && data?.valid === false) zoomUnavailable = true
  } catch {
    // Ignore - might not support absolute zoom
  } finally { zoomFetchPending = false }
}

async function setZoomAbsolute(cameraId: number, pos: number) {
  const revision = cameraRevision
  const editRevision = zoomRevision
  error.value = ''
  // Clamp to valid range
  pos = Math.max(zoomMin.value, Math.min(zoomMax.value, Math.round(pos)))
  zoomPending++
  try {
    await movement.position(async () => {
      if (!alive || cameraId !== props.cameraId || revision !== cameraRevision || editRevision !== zoomRevision) return
      await api(`/ptz/${cameraId}/zoom/set`, {
        method: 'POST', body: { zoom: pos }, redirectOnFail: false,
      })
    })
  } catch (e: any) {
    if (alive && cameraId === props.cameraId) error.value = e.message
  } finally { zoomPending-- }
}

function onZoomInput(event: Event) {
  const target = event.target as HTMLInputElement
  const newVal = Number(target.value)
  zoomRevision++
  zoomPos.value = newVal

  // Debounce: send after 100ms of no further input
  if (zoomSendTimeout) clearTimeout(zoomSendTimeout)
  const cameraId = props.cameraId
  zoomSendTimeout = setTimeout(() => {
    zoomSendTimeout = null
    if (alive && cameraId === props.cameraId) void setZoomAbsolute(cameraId, newVal)
  }, 100)
}

// Poll zoom position periodically while controls are visible
let pollInterval: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  window.addEventListener('blur', stopMove)
  window.addEventListener('pointerup', onPointerStop)
  window.addEventListener('pointercancel', onPointerStop)
  document.addEventListener('visibilitychange', onVisibility)
  fetchZoomState()
  void fetchPresets()
  pollInterval = setInterval(fetchZoomState, 5000)
})

onUnmounted(() => {
  alive = false
  movement.dispose()
  window.removeEventListener('blur', stopMove)
  window.removeEventListener('pointerup', onPointerStop)
  window.removeEventListener('pointercancel', onPointerStop)
  document.removeEventListener('visibilitychange', onVisibility)
  if (pollInterval) clearInterval(pollInterval)
  if (zoomSendTimeout) clearTimeout(zoomSendTimeout)
})
watch(() => props.cameraId, () => {
  cameraRevision++
  zoomRevision++
  stopMove()
  if (zoomSendTimeout) clearTimeout(zoomSendTimeout)
  zoomSendTimeout = null
  zoomLoaded.value = false
  zoomUnavailable = false
  presets.value = []
  presetId.value = ''
  presetBusy.value = false
  void fetchPresets()
  void fetchZoomState()
})
</script>

<template>
  <div class="ptz-controls">
    <div class="ptz-hint">Hold to move · release to stop · 10s maximum</div>
    <!-- D-pad + Zoom row -->
    <div class="ptz-main">
      <!-- D-pad -->
      <div class="ptz-dpad">
      <button
        class="ptz-btn ptz-up"
        @pointerdown.prevent.stop="onMovePointer($event, 'up')"
        @pointerup="onPointerStop"
        @pointercancel="onPointerStop"
        @lostpointercapture="onPointerStop"
        @keydown.space.prevent="onMoveKey($event, 'up')"
        @keydown.enter.prevent="onMoveKey($event, 'up')"
        @keyup.space.prevent="stopMove"
        @keyup.enter.prevent="stopMove"
        @blur="stopMove"
        title="Tilt Up"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M12 4l-8 8h16z"/>
        </svg>
      </button>

      <button
        class="ptz-btn ptz-left"
        @pointerdown.prevent.stop="onMovePointer($event, 'left')"
        @pointerup="onPointerStop"
        @pointercancel="onPointerStop"
        @lostpointercapture="onPointerStop"
        @keydown.space.prevent="onMoveKey($event, 'left')"
        @keydown.enter.prevent="onMoveKey($event, 'left')"
        @keyup.space.prevent="stopMove"
        @keyup.enter.prevent="stopMove"
        @blur="stopMove"
        title="Pan Left"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M4 12l8-8v16z"/>
        </svg>
      </button>

      <button
        class="ptz-btn ptz-center"
        @click="movement.emergencyStop(cameraId, speed)"
        title="Stop"
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
          <rect x="6" y="6" width="12" height="12" rx="2"/>
        </svg>
      </button>

      <button
        class="ptz-btn ptz-right"
        @pointerdown.prevent.stop="onMovePointer($event, 'right')"
        @pointerup="onPointerStop"
        @pointercancel="onPointerStop"
        @lostpointercapture="onPointerStop"
        @keydown.space.prevent="onMoveKey($event, 'right')"
        @keydown.enter.prevent="onMoveKey($event, 'right')"
        @keyup.space.prevent="stopMove"
        @keyup.enter.prevent="stopMove"
        @blur="stopMove"
        title="Pan Right"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M20 12l-8-8v16z"/>
        </svg>
      </button>

      <button
        class="ptz-btn ptz-down"
        @pointerdown.prevent.stop="onMovePointer($event, 'down')"
        @pointerup="onPointerStop"
        @pointercancel="onPointerStop"
        @lostpointercapture="onPointerStop"
        @keydown.space.prevent="onMoveKey($event, 'down')"
        @keydown.enter.prevent="onMoveKey($event, 'down')"
        @keyup.space.prevent="stopMove"
        @keyup.enter.prevent="stopMove"
        @blur="stopMove"
        title="Tilt Down"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M12 20l8-8H4z"/>
        </svg>
      </button>
      </div>

      <!-- Zoom slider (vertical) -->
      <div class="ptz-zoom">
        <button class="ptz-btn" title="Hold to zoom in" aria-label="Zoom in"
          @pointerdown.prevent.stop="onMovePointer($event, 'zoomin')"
          @pointerup="onPointerStop" @pointercancel="onPointerStop" @lostpointercapture="onPointerStop"
          @keydown.space.prevent="onMoveKey($event, 'zoomin')" @keydown.enter.prevent="onMoveKey($event, 'zoomin')"
          @keyup.space.prevent="stopMove" @keyup.enter.prevent="stopMove" @blur="stopMove">+</button>
        <span class="ptz-zoom-label">+</span>
        <input
          type="range"
          :value="zoomPos"
          :min="zoomMin"
          :max="zoomMax"
          step="1"
          class="ptz-zoom-slider"
          :disabled="!zoomLoaded"
          :title="zoomLoaded ? 'Absolute zoom position' : 'Absolute zoom unavailable for this camera'"
          orient="vertical"
          @input="onZoomInput"
        />
        <span class="ptz-zoom-label">−</span>
        <span class="ptz-zoom-level">{{ zoomLoaded ? zoomMultiplier : 'Hold ±' }}</span>
        <button class="ptz-btn" title="Hold to zoom out" aria-label="Zoom out"
          @pointerdown.prevent.stop="onMovePointer($event, 'zoomout')"
          @pointerup="onPointerStop" @pointercancel="onPointerStop" @lostpointercapture="onPointerStop"
          @keydown.space.prevent="onMoveKey($event, 'zoomout')" @keydown.enter.prevent="onMoveKey($event, 'zoomout')"
          @keyup.space.prevent="stopMove" @keyup.enter.prevent="stopMove" @blur="stopMove">−</button>
      </div>
    </div>

    <!-- Speed slider -->
    <div class="ptz-speed">
      <label class="ptz-speed-label">Speed</label>
      <input
        type="range"
        v-model.number="speed"
        min="1"
        max="64"
        step="1"
        class="ptz-speed-slider"
      />
      <span class="ptz-speed-value">{{ speed }}</span>
    </div>

    <div class="ptz-presets">
      <select v-model="presetId" aria-label="Saved camera position" :disabled="presetsLoading || presetBusy || !presets.length">
        <option value="">{{ presetsLoading ? 'Loading positions…' : presetsUnavailable ? 'Positions unavailable' : presets.length ? 'Saved position…' : 'No saved positions' }}</option>
        <option v-for="preset in presets" :key="preset.id" :value="preset.id">{{ preset.name }}</option>
      </select>
      <button :disabled="presetId === '' || presetBusy" @click="goToPreset" title="Move to selected saved position">{{ presetBusy ? 'Moving…' : 'Go' }}</button>
    </div>
    <!-- Error display -->
    <div v-if="error" class="ptz-error" role="alert">{{ error }}</div>
  </div>
</template>

<style scoped>
.ptz-controls {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
  padding: 8px;
  background: rgba(0, 0, 0, 0.7);
  border-radius: 8px;
  backdrop-filter: blur(4px);
  user-select: none;
}
.ptz-hint { color: #b9c7d9; font-size: 10px; }
.ptz-presets { display: flex; gap: 5px; max-width: 240px; }
.ptz-presets select { min-width: 0; flex: 1; }
.ptz-presets :is(select, button) { color: #e8f3ff; background: #243449; border: 1px solid #52677f; border-radius: 4px; font-size: 11px; padding: 4px; }
.ptz-presets :disabled { opacity: 0.6; }
.ptz-btn { touch-action: none; }

.ptz-main {
  display: flex;
  align-items: center;
  gap: 8px;
}

.ptz-dpad {
  display: grid;
  grid-template-columns: 32px 32px 32px;
  grid-template-rows: 32px 32px 32px;
  gap: 2px;
}

.ptz-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  border: none;
  border-radius: 4px;
  background: rgba(255, 255, 255, 0.15);
  color: white;
  cursor: pointer;
  transition: background 0.1s;
}

.ptz-btn:hover {
  background: rgba(255, 255, 255, 0.3);
}

.ptz-btn:active {
  background: rgba(59, 130, 246, 0.6);
}

.ptz-up { grid-column: 2; grid-row: 1; }
.ptz-left { grid-column: 1; grid-row: 2; }
.ptz-center { grid-column: 2; grid-row: 2; }
.ptz-right { grid-column: 3; grid-row: 2; }
.ptz-down { grid-column: 2; grid-row: 3; }

.ptz-zoom {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
}

.ptz-zoom-label {
  color: rgba(255, 255, 255, 0.8);
  font-size: 14px;
  font-weight: bold;
  line-height: 1;
}

.ptz-zoom-slider {
  writing-mode: vertical-lr;
  direction: rtl;
  width: 20px;
  height: 80px;
  accent-color: #3b82f6;
  cursor: pointer;
}

.ptz-zoom-level {
  color: rgba(255, 255, 255, 0.9);
  font-size: 9px;
  font-weight: 600;
  white-space: nowrap;
  margin-top: 2px;
}

.ptz-speed {
  display: flex;
  align-items: center;
  gap: 4px;
  width: 100%;
}

.ptz-speed-label {
  color: rgba(255, 255, 255, 0.7);
  font-size: 10px;
  white-space: nowrap;
}

.ptz-speed-slider {
  flex: 1;
  height: 4px;
  accent-color: #3b82f6;
}

.ptz-speed-value {
  color: rgba(255, 255, 255, 0.9);
  font-size: 10px;
  min-width: 16px;
  text-align: right;
}

.ptz-error {
  color: #ef4444;
  font-size: 10px;
  text-align: center;
  max-width: 120px;
  word-break: break-word;
}
</style>
