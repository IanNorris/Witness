<script setup lang="ts">
import { computed, inject, onMounted, onUnmounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import CameraCard from '../camera/CameraCard.vue'
import ActivityStrip from '../clips/ActivityStrip.vue'
import PtzControls from '../camera/PtzControls.vue'
import TileBoundary from './TileBoundary.vue'
import ClockTile from './tiles/ClockTile.vue'
import StatusTile from './tiles/StatusTile.vue'
import TimelineTile from './tiles/TimelineTile.vue'
import DetectionTile from './tiles/DetectionTile.vue'
import PlaceholderTile from './tiles/PlaceholderTile.vue'
import TextTile from './tiles/TextTile.vue'
import { useCameraStore } from '../../stores/cameras'
import { dashboardPlaybackKey } from '../../composables/useDashboardPlayback'
import {
  ActiveHolds, readingOrderCameras, resolveDashboard, splitCells, spotlightCandidates,
  type CameraTile, type DashboardDocument, type DashboardTile, type Rect, type ScopedTileBase, type SpotlightTile,
} from '../../types/dashboard'
import type { Clip } from '../../types/clip'

// Live renderer for view, kiosk and editor preview.
//
// Invariants carried over from the previous fullscreen grid (and tested):
//  * one element per mounted camera, keyed by camera id in a stable order,
//    positioned only by style and parked off-screen (inert) when not shown;
//    it is never re-parented, so live sockets are not restarted;
//  * the mounted set is fixed cameras plus shown/warm spotlight sources;
//    resolution changes placement, never the set;
//  * spotlights freeze while any camera is in replay;
//  * Esc unwinds maximise, then control selection, then the view.
const props = withDefaults(defineProps<{
  document: DashboardDocument
  kiosk?: boolean
  chromeVisible?: boolean
}>(), { kiosk: false, chromeVisible: true })

const emit = defineEmits<{
  play: [clip: Clip]
  escape: []
}>()

const cameraStore = useCameraStore()
const router = useRouter()
const playback = inject(dashboardPlaybackKey, undefined)
const rootRef = ref<HTMLElement | null>(null)

const available = computed(() => cameraStore.cameras.map(camera => camera.id))
const doc = computed(() => props.document)
const cols = computed(() => doc.value.grid.cols)
const rows = computed(() => doc.value.grid.rows)
const gap = computed(() => doc.value.grid.gap)
const radius = computed(() => doc.value.grid.radius)
const dashboardCameras = computed(() => readingOrderCameras(doc.value, available.value))
const frozen = computed(() => playback?.anyReplay.value ?? false)

// ----- Spotlight runtime -------------------------------------------------
const holds = new ActiveHolds()
const active = ref<Set<number>>(new Set())
let releaseTimer: ReturnType<typeof setTimeout> | null = null
const motion = computed(() => new Set(cameraStore.cameras.filter(camera => camera.motionActive).map(camera => camera.id)))
const motionKey = computed(() => [...motion.value].sort((a, b) => a - b).join(','))

function holdSecondsFor(id: number) {
  let hold = 0
  for (const tile of doc.value.tiles) {
    if (tile.type !== 'spotlight' || tile.trigger !== 'motion') continue
    if (spotlightCandidates(tile, doc.value, available.value).includes(id)) hold = Math.max(hold, tile.holdSeconds)
  }
  return hold || 15
}

function refreshActive() {
  if (releaseTimer) { clearTimeout(releaseTimer); releaseTimer = null }
  if (frozen.value) return
  const now = Date.now()
  active.value = holds.update(motion.value, now, holdSecondsFor)
  const next = holds.nextRelease()
  if (next !== null) releaseTimer = setTimeout(refreshActive, Math.max(50, next - now + 10))
}
watch(motionKey, refreshActive, { immediate: true })
watch(frozen, value => { if (!value) refreshActive() })

const rotation = ref(new Map<string, number>())
const rotationDue = new Map<string, number>()
const lastShown = new Map<string, number[]>()
function rotationInterval(tile: SpotlightTile): number | null {
  if (tile.trigger === 'rotate') return tile.idle.kind === 'rotate' ? tile.idle.everySeconds : tile.holdSeconds
  return tile.idle.kind === 'rotate' ? tile.idle.everySeconds : null
}
const rotationTimer = setInterval(() => {
  if (frozen.value) return
  const now = Date.now()
  let next: Map<string, number> | null = null
  for (const tile of doc.value.tiles) {
    if (tile.type !== 'spotlight') continue
    const every = rotationInterval(tile)
    if (!every) continue
    const due = rotationDue.get(tile.id)
    if (due === undefined) { rotationDue.set(tile.id, now + every * 1000); continue }
    if (now < due) continue
    rotationDue.set(tile.id, now + every * 1000)
    next ??= new Map(rotation.value)
    next.set(tile.id, (next.get(tile.id) ?? 0) + 1)
  }
  if (next) rotation.value = next
}, 1000)

const resolved = computed(() => resolveDashboard(doc.value, available.value, {
  active: active.value, rotation: rotation.value, lastShown,
}))
watch(resolved, value => {
  for (const [tileId, shown] of value.spotlights) if (shown.length) lastShown.set(tileId, shown)
})

// ----- Maximise (double-click) --------------------------------------------
const maximized = ref<number | null>(null)
let maximizeTimer: ReturnType<typeof setTimeout> | null = null
function armMaximizeTimer() {
  if (maximizeTimer) clearTimeout(maximizeTimer)
  maximizeTimer = null
  const seconds = doc.value.behavior.returnToLiveSeconds
  if (maximized.value !== null && seconds > 0) maximizeTimer = setTimeout(() => { maximized.value = null }, seconds * 1000)
}
function toggleMaximize(id: number) {
  maximized.value = maximized.value === id ? null : id
  armMaximizeTimer()
}
watch(() => resolved.value.mounted.join(','), () => {
  if (maximized.value !== null && !resolved.value.mounted.includes(maximized.value)) maximized.value = null
})

// ----- Placement -----------------------------------------------------------
interface CameraSlot extends Rect { tileId: string; spotlight: boolean }
function tileAspect(tile: Rect) {
  return (tile.w / cols.value * doc.value.target.width) / Math.max(1, tile.h / rows.value * doc.value.target.height)
}
const slots = computed(() => {
  const map = new Map<number, CameraSlot>()
  if (maximized.value !== null) {
    map.set(maximized.value, { x: 0, y: 0, w: cols.value, h: rows.value, tileId: '', spotlight: false })
    return map
  }
  const availableSet = new Set(available.value)
  for (const tile of doc.value.tiles) {
    if (tile.type === 'camera' && availableSet.has(tile.camera) && !resolved.value.borrowed.has(tile.id))
      map.set(tile.camera, { x: tile.x, y: tile.y, w: tile.w, h: tile.h, tileId: tile.id, spotlight: false })
    if (tile.type === 'spotlight') {
      const shown = resolved.value.spotlights.get(tile.id) ?? []
      const cells = splitCells(shown.length, tileAspect(tile))
      shown.forEach((id, index) => {
        const cell = cells[index]!
        map.set(id, { x: tile.x + cell.x * tile.w, y: tile.y + cell.y * tile.h, w: cell.w * tile.w, h: cell.h * tile.h, tileId: tile.id, spotlight: true })
      })
    }
  }
  return map
})

const tileById = computed(() => new Map(doc.value.tiles.map(tile => [tile.id, tile])))
const mountedCameras = computed(() => resolved.value.mounted
  .map(id => cameraStore.getCameraById(id))
  .filter(camera => camera !== undefined))

function rectStyle(rect: Rect, extra: Record<string, string> = {}) {
  const half = gap.value / 2
  return {
    left: `calc(${rect.x / cols.value * 100}% + ${half}px)`,
    top: `calc(${rect.y / rows.value * 100}% + ${half}px)`,
    width: `calc(${rect.w / cols.value * 100}% - ${gap.value}px)`,
    height: `calc(${rect.h / rows.value * 100}% - ${gap.value}px)`,
    borderRadius: `${radius.value}px`,
    ...extra,
  }
}

function cameraTileFor(id: number): CameraTile | undefined {
  const slot = slots.value.get(id)
  const tile = slot ? tileById.value.get(slot.tileId) : undefined
  return tile?.type === 'camera' ? tile : undefined
}
function hideName(id: number) {
  const slot = slots.value.get(id)
  const tile = slot ? tileById.value.get(slot.tileId) : undefined
  return tile !== undefined && 'label' in tile && tile.label === false
}

function scopeCameras(tile: ScopedTileBase): number[] {
  if (tile.scope.kind === 'all') return available.value
  if (tile.scope.kind === 'list') return (tile.scope.cameras ?? []).filter(id => available.value.includes(id))
  return dashboardCameras.value
}
function scopeSet(tile: ScopedTileBase): Set<number> | null {
  return tile.scope.kind === 'all' ? null : new Set(scopeCameras(tile))
}
function activityOrientation(tile: DashboardTile) {
  if (tile.type === 'activity' && tile.direction !== 'auto') return tile.direction
  return tileAspect(tile) >= 1 ? 'horizontal' : 'vertical'
}
function spotlightLabel(tile: SpotlightTile) {
  const shown = resolved.value.spotlights.get(tile.id) ?? []
  const names = shown.map(id => cameraStore.getCameraById(id)?.name).filter(Boolean)
  return `${tile.trigger === 'rotate' ? 'ROTATE' : 'AUTO'}${names.length === 1 ? ` · ${names[0]}` : names.length > 1 ? ` · ${names.length}` : ''}`
}
// Widget tiles render as their own elements; camera pixels come from the
// mounted camera layer above, so camera tiles only draw placeholders here.
const widgetTiles = computed(() => maximized.value !== null ? [] : doc.value.tiles)

// ----- Control selection ---------------------------------------------------
const selectedControls = ref<number | null>(null)
function selectControls(id: number) {
  selectedControls.value = id
  if (playback) { playback.selected.value = id; playback.touch(id) }
}
function onOutsidePointer(event: PointerEvent) {
  const target = event.target
  if (target instanceof Element && Number(target.closest('[data-ptz-controls-camera]')?.getAttribute('data-ptz-controls-camera')) === selectedControls.value) return
  if (!(target instanceof Element) || !rootRef.value?.contains(target) || !target.closest('.dash-camera')) selectedControls.value = null
}
watch(() => [...slots.value.keys()].join(','), () => {
  if (selectedControls.value !== null && !slots.value.has(selectedControls.value)) selectedControls.value = null
})
function onKeydown(event: KeyboardEvent) {
  if (event.key !== 'Escape') return
  if (maximized.value !== null) { maximized.value = null; return }
  if (selectedControls.value !== null) { selectedControls.value = null; return }
  emit('escape')
}
function onActivity() { if (maximized.value !== null) armMaximizeTimer() }

onMounted(() => {
  window.addEventListener('keydown', onKeydown)
  document.addEventListener('pointerdown', onOutsidePointer, true)
})
onUnmounted(() => {
  window.removeEventListener('keydown', onKeydown)
  document.removeEventListener('pointerdown', onOutsidePointer, true)
  clearInterval(rotationTimer)
  if (releaseTimer) clearTimeout(releaseTimer)
  if (maximizeTimer) clearTimeout(maximizeTimer)
  holds.clear()
})

defineExpose({ resolved, slots, maximized, selectedControls })
</script>

<template>
  <div ref="rootRef" class="dash-canvas" :class="{ kiosk }" @pointermove.passive="onActivity">
    <!-- Widget and placeholder layer -->
    <div v-for="tile in widgetTiles" :key="tile.id" class="dash-tile" :class="`dash-tile-${tile.type}`" :style="rectStyle(tile)" :data-tile-id="tile.id">
      <TileBoundary>
        <template v-if="tile.type === 'camera'">
          <PlaceholderTile v-if="resolved.borrowed.has(tile.id)" kind="borrowed" />
          <PlaceholderTile v-else-if="!available.includes(tile.camera)" kind="missing" />
        </template>
        <PlaceholderTile v-else-if="tile.type === 'unavailable'" kind="unavailable" />
        <template v-else-if="tile.type === 'spotlight'">
          <PlaceholderTile v-if="(resolved.spotlights.get(tile.id) ?? []).length === 0" kind="waiting"
            :label="tile.trigger === 'rotate' ? 'No cameras to rotate' : 'Waiting for motion'" />
        </template>
        <ActivityStrip v-else-if="tile.type === 'activity'" :camera-ids="scopeSet(tile)" :orientation="activityOrientation(tile)" fill @play="emit('play', $event)" />
        <ClockTile v-else-if="tile.type === 'clock'" :seconds="tile.seconds" :date="tile.date" />
        <StatusTile v-else-if="tile.type === 'status'" :variant="tile.variant" :camera-ids="dashboardCameras.length ? dashboardCameras : available" />
        <TimelineTile v-else-if="tile.type === 'timeline'" :hours="tile.hours" :camera-ids="scopeCameras(tile)" />
        <DetectionTile v-else-if="tile.type === 'detection'" :camera-ids="scopeCameras(tile)" @play="emit('play', $event)" />
        <template v-else-if="tile.type === 'ptz'">
          <div v-if="available.includes(tile.camera)" class="dash-ptz">
            <div v-if="tile.label" class="dash-ptz-name">{{ cameraStore.getCameraById(tile.camera)?.name }}</div>
            <PtzControls :camera-id="tile.camera" />
          </div>
          <PlaceholderTile v-else kind="missing" />
        </template>
        <TextTile v-else-if="tile.type === 'text'" :text="tile.text" :size="tile.size" />
      </TileBoundary>
    </div>

    <!-- Mounted camera layer: stable keys, positioned by style only -->
    <div v-for="camera in mountedCameras" :key="camera.id" class="dash-camera"
      :class="{ 'dash-parked': !slots.has(camera.id), motion: slots.has(camera.id) && camera.motionActive, maximized: maximized === camera.id }"
      :style="slots.has(camera.id) ? rectStyle(slots.get(camera.id)!) : undefined"
      :inert="!slots.has(camera.id)" :aria-hidden="!slots.has(camera.id)" :data-camera-id="camera.id"
      @pointerdown.capture="selectControls(camera.id)" @focusin="selectControls(camera.id)">
      <TileBoundary>
        <CameraCard :camera="camera" layout-mode :kiosk="kiosk" :dashboard-hidden="!slots.has(camera.id)"
          :controls-selected="selectedControls === camera.id" :fullscreen-controls-visible="chromeVisible"
          :tile-stream="cameraTileFor(camera.id)?.stream" :hide-name="hideName(camera.id)"
          :no-playback-bar="cameraTileFor(camera.id)?.playback === false" double-click-maximizes
          @toggle-maximize="toggleMaximize" @dismiss-controls="selectedControls = null"
          @open-stream="router.push(`/live/${$event}`)" @open-clips="router.push(`/clips/${$event}`)" />
      </TileBoundary>
    </div>

    <!-- Spotlight labels sit above their cameras -->
    <template v-if="maximized === null">
      <div v-for="tile in doc.tiles.filter(t => t.type === 'spotlight' && t.label && (resolved.spotlights.get(t.id) ?? []).length > 0)" :key="`label-${tile.id}`"
        class="dash-spotlight-label" :style="rectStyle(tile, { height: 'auto', width: 'auto' })">{{ spotlightLabel(tile as SpotlightTile) }}</div>
    </template>
    <button v-if="maximized !== null" class="dash-restore" type="button" @click="maximized = null" title="Return to the dashboard (Esc)">Restore layout</button>

    <div v-for="badge in doc.badges" :key="badge.id" class="dash-badge" :class="`anchor-${badge.anchor} size-${badge.size}`">
      <ClockTile compact :date="badge.size !== 's'" />
    </div>
  </div>
</template>

<style scoped>
.dash-canvas { position: relative; width: 100%; height: 100%; background: #05070b; overflow: hidden; }
.dash-tile, .dash-camera { position: absolute; overflow: hidden; background: #000; }
.dash-tile { z-index: 1; }
.dash-camera { z-index: 2; }
.dash-camera > :deep(.camera-card), .dash-camera :deep(.camera-card-fullscreen) { width: 100%; height: 100%; }
.dash-camera.motion { box-shadow: inset 0 0 0 2px #f5b54a; }
.dash-camera.motion :deep(.camera-card) { box-shadow: inset 0 0 0 2px #f5b54a; }
.dash-camera.maximized { z-index: 5; }
.dash-parked { left: -10000px; top: -10000px; width: 2px; height: 2px; opacity: .7; pointer-events: none; }
.dash-tile-camera, .dash-tile-spotlight { background: #070b10; }
.dash-spotlight-label { position: absolute; z-index: 4; margin: 8px; padding: 2px 7px; border-radius: 4px; background: rgba(8, 12, 18, .72); color: #cfe6fb; font-size: 11px; font-weight: 600; letter-spacing: .04em; pointer-events: none; }
.dash-restore { position: absolute; z-index: 6; top: 10px; left: 50%; transform: translateX(-50%); border: 1px solid #4a6280; border-radius: 99px; background: rgba(15, 24, 37, .9); color: #e8f3ff; padding: 4px 12px; font-size: 12px; }
.dash-ptz { width: 100%; height: 100%; overflow: auto; background: #0b1119; padding: .4rem; }
.dash-ptz-name { color: #cfe0f2; font-size: .75rem; margin-bottom: .25rem; }
.dash-badge { position: absolute; z-index: 7; pointer-events: none; margin: 12px; }
.anchor-top-left { top: 0; left: 0; }
.anchor-top-right { top: 0; right: 0; }
.anchor-bottom-left { bottom: 0; left: 0; }
.anchor-bottom-right { bottom: 0; right: 0; }
.size-l :deep(.clock-time) { font-size: 1.8rem; }
.size-s :deep(.clock-time) { font-size: .9rem; }
</style>
