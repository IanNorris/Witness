<script setup lang="ts">
import { onMounted, onUnmounted, ref, computed, watch } from 'vue'
import AppLayout from '../components/layout/AppLayout.vue'
import CameraGrid from '../components/camera/CameraGrid.vue'
import ActivityStrip from '../components/clips/ActivityStrip.vue'
import ClipPlayer from '../components/clips/ClipPlayer.vue'
import { useCameraStore } from '../stores/cameras'
import { useClipStore } from '../stores/clips'
import { useSettingsStore } from '../stores/settings'
import { useGroupStore } from '../stores/groups'
import type { Clip } from '../types/clip'
import {
  createDefaultDashboardLayout,
  createDefaultFocusLayout,
  normaliseDashboardLayout,
  generatedDashboardTiles,
  dashboardAreasOverlap,
  type ActivityDock,
  type DashboardMode,
  type DashboardPreset,
  type DashboardLayout,
  type DashboardTileLayout,
  type DashboardGridArea,
} from '../types/dashboardLayout'

const cameraStore = useCameraStore()
const clipStore = useClipStore()
const settings = useSettingsStore()
const groupStore = useGroupStore()

const DASHBOARD_GROUP_KEY = 'witness-dashboard-group'
const DASHBOARD_ACTIVITY_KEY = 'witness-dashboard-activity-visible'
const savedGroup = localStorage.getItem(DASHBOARD_GROUP_KEY)
const selectedGroupId = ref<number | null>(savedGroup !== null ? Number(savedGroup) : null)
const showRecentActivity = ref(localStorage.getItem(DASHBOARD_ACTIVITY_KEY) !== '0')
const playingClip = ref<Clip | null>(null)
const editingLayout = ref(false)
const editingLayoutMode = ref<'regular' | 'focus'>('regular')
const focusPreviewCameraId = ref<number | null>(null)
const dashboardLayout = ref<DashboardLayout>(createDefaultDashboardLayout([]))
const stageRef = ref<HTMLElement | null>(null)
const layoutMessage = ref('')
const rectangleRequested = computed(() => canvasMode.value && showRecentActivity.value && dashboardLayout.value.activityPlacement === 'rectangle')
const regularTiles = computed(() => dashboardLayout.value.preset === 'custom'
  ? dashboardLayout.value.tiles.filter(tile => dashboardLayout.value.visibleCameraIds.includes(tile.cameraId))
  : generatedDashboardTiles(dashboardLayout.value.preset, dashboardLayout.value.priorityCameraIds))
const regularActivityConflict = computed(() => rectangleRequested.value &&
  regularTiles.value.some(tile => dashboardAreasOverlap(tile, dashboardLayout.value.activityRegion)))
// A changed group can fill a previously empty slot. Keep activity available in
// its dock until the user rearranges, rather than covering a newly added camera.
const rectangleActive = computed(() => rectangleRequested.value && (editingLayout.value || !regularActivityConflict.value))
const occupiedFocusAreas = computed(() => [dashboardLayout.value.focusRegion,
  ...dashboardLayout.value.focusSlots.slice(0, displayedCameraIds.value.length)])
const focusActivityConflict = computed(() => rectangleActive.value &&
  occupiedFocusAreas.value.some(area => dashboardAreasOverlap(area, dashboardLayout.value.activityRegion)))
const focusCameraConflict = computed(() => occupiedFocusAreas.value.some((area, index) =>
  occupiedFocusAreas.value.slice(index + 1).some(other => dashboardAreasOverlap(area, other))))
const focusAllowed = computed(() => !focusActivityConflict.value && !focusCameraConflict.value)
const regularCameraConflict = computed(() => regularTiles.value.some((tile, index) =>
  regularTiles.value.slice(index + 1).some(other => dashboardAreasOverlap(tile, other))))
const canSaveLayout = computed(() => !regularCameraConflict.value && !regularActivityConflict.value &&
  (!dashboardLayout.value.autoFocusEnabled || focusAllowed.value))
let activityDrag: { action: 'move' | 'resize'; x: number; y: number; region: DashboardGridArea } | null = null
function stopActivityDrag() {
  activityDrag = null
  window.removeEventListener('pointermove', moveActivity)
  window.removeEventListener('pointerup', stopActivityDrag)
  window.removeEventListener('pointercancel', stopActivityDrag)
}
function startActivityDrag(event: PointerEvent, action: 'move' | 'resize') {
  if (!editingLayout.value) return
  event.preventDefault()
  activityDrag = { action, x: event.clientX, y: event.clientY, region: { ...dashboardLayout.value.activityRegion } }
  window.addEventListener('pointermove', moveActivity)
  window.addEventListener('pointerup', stopActivityDrag)
  window.addEventListener('pointercancel', stopActivityDrag)
}
function moveActivity(event: PointerEvent) {
  if (!activityDrag) return
  const bounds = settings.fullscreenMode ? { width: window.innerWidth, height: window.innerHeight } : stageRef.value?.getBoundingClientRect()
  if (!bounds || bounds.width <= 0 || bounds.height <= 0) return
  const dx = Math.round((event.clientX - activityDrag.x) * 12 / bounds.width)
  const dy = Math.round((event.clientY - activityDrag.y) * 12 / bounds.height)
  const region = { ...activityDrag.region }
  if (activityDrag.action === 'move') {
    region.x = Math.max(0, Math.min(12 - region.width, region.x + dx))
    region.y = Math.max(0, Math.min(12 - region.height, region.y + dy))
  } else {
    region.width = Math.max(1, Math.min(12 - region.x, region.width + dx))
    region.height = Math.max(1, Math.min(12 - region.y, region.height + dy))
  }
  dashboardLayout.value.activityRegion = region
}
function setActivityPlacement(placement: 'dock' | 'rectangle') {
  stopActivityDrag()
  dashboardLayout.value.activityPlacement = placement
}
function setActivityCoordinate(field: keyof DashboardGridArea, event: Event) {
  const value = Number((event.target as HTMLInputElement).value)
  if (!Number.isFinite(value)) return
  const region = { ...dashboardLayout.value.activityRegion, [field]: Math.round(value) }
  region.x = Math.max(0, Math.min(11, region.x)); region.y = Math.max(0, Math.min(11, region.y))
  region.width = Math.max(1, Math.min(12 - region.x, region.width))
  region.height = Math.max(1, Math.min(12 - region.y, region.height))
  dashboardLayout.value.activityRegion = region
}
const layoutView = ref(localStorage.getItem('witness-dashboard-windowed-mode') === 'layout')
const canvasMode = computed(() => settings.fullscreenMode || layoutView.value)
const dashboardMode = computed<DashboardMode>(() => settings.fullscreenMode ? 'fullscreen' : layoutView.value ? 'layout' : 'tiles')
function selectDashboardMode(mode: DashboardMode) {
  if (editingLayout.value) cancelLayoutEdit()
  settings.fullscreenMode = mode === 'fullscreen'
  if (mode !== 'fullscreen') {
    layoutView.value = mode === 'layout'
    localStorage.setItem('witness-dashboard-windowed-mode', mode)
  }
}
let layoutBeforeEditing: DashboardLayout | null = null
const focusedCameraId = ref<number | null>(null)
const manuallyFocused = ref(false)
const fullscreenControlsVisible = ref(true)
let focusReleaseTimer: ReturnType<typeof setTimeout> | null = null
let fullscreenControlsTimer: ReturnType<typeof setTimeout> | null = null

function showFullscreenControls() {
  if (!settings.fullscreenMode) return
  fullscreenControlsVisible.value = true
  if (fullscreenControlsTimer) clearTimeout(fullscreenControlsTimer)
  fullscreenControlsTimer = setTimeout(() => {
    fullscreenControlsVisible.value = false
    fullscreenControlsTimer = null
  }, 2500)
}

const layoutStorageKey = computed(() =>
  `witness-dashboard-layout-v1-${selectedGroupId.value === null ? 'all' : selectedGroupId.value}`,
)

const currentCameraIds = computed(() => {
  if (selectedGroupId.value === null) return cameraStore.cameras.map(camera => camera.id)
  return groupStore.camerasInGroup(selectedGroupId.value).map(camera => camera.id)
})

const currentCameras = computed(() =>
  currentCameraIds.value
    .map(id => cameraStore.getCameraById(id))
    .filter(camera => camera !== undefined),
)

const focusEditorTiles = computed<DashboardTileLayout[]>(() => {
  const previewId = focusPreviewCameraId.value ?? currentCameraIds.value[0]
  if (previewId === undefined) return []
  const others = currentCameraIds.value.filter(id => id !== previewId)
  return [
    { cameraId: previewId, ...dashboardLayout.value.focusRegion },
    ...others.map((cameraId, index) => ({
      cameraId,
      ...(dashboardLayout.value.focusSlots[index] ?? { x: 8, y: 0, width: 4, height: 12 }),
    })),
  ]
})

const editorTiles = computed(() =>
  editingLayout.value && editingLayoutMode.value === 'focus'
    ? focusEditorTiles.value
    : dashboardLayout.value.preset !== 'custom' && !editingLayout.value
      ? generatedDashboardTiles(dashboardLayout.value.preset, dashboardLayout.value.priorityCameraIds)
      : dashboardLayout.value.tiles,
)
const displayedCameraIds = computed(() => dashboardLayout.value.preset === 'custom' || editingLayout.value
  ? dashboardLayout.value.visibleCameraIds : editorTiles.value.map(tile => tile.cameraId))
function changePreset(event: Event) {
  const select = event.target as HTMLSelectElement
  const next = select.value as DashboardPreset
  const tiles = next === 'custom' ? dashboardLayout.value.tiles.filter(tile => dashboardLayout.value.visibleCameraIds.includes(tile.cameraId))
    : generatedDashboardTiles(next, dashboardLayout.value.priorityCameraIds)
  if (rectangleActive.value && tiles.some(tile => dashboardAreasOverlap(tile, dashboardLayout.value.activityRegion))) {
    layoutMessage.value = 'This preset overlaps activity. Dock activity or adjust the custom layout first.'
    select.value = dashboardLayout.value.preset
    return
  }
  layoutMessage.value = ''
  dashboardLayout.value.preset = next
  focusedCameraId.value = null
  manuallyFocused.value = false
  persistLayout()
  updateAutomaticFocus()
}
function moveCameraPriority(cameraId: number, direction: number) {
  const order = [...dashboardLayout.value.priorityCameraIds]
  const index = order.indexOf(cameraId), next = index + direction
  if (index < 0 || next < 0 || next >= order.length) return
  ;[order[index], order[next]] = [order[next]!, order[index]!]
  dashboardLayout.value.priorityCameraIds = order
}

const displayedFocusedCameraId = computed(() => {
  if (!canvasMode.value) return null
  if (editingLayout.value) {
    return editingLayoutMode.value === 'focus' ? focusPreviewCameraId.value : null
  }
  return focusedCameraId.value
})

function cloneLayout(layout: DashboardLayout): DashboardLayout {
  return JSON.parse(JSON.stringify(layout)) as DashboardLayout
}

function loadLayout() {
  if (focusReleaseTimer) { clearTimeout(focusReleaseTimer); focusReleaseTimer = null }
  let saved: Partial<DashboardLayout> | null = null
  try {
    const value = localStorage.getItem(layoutStorageKey.value)
    saved = value ? JSON.parse(value) : null
  } catch {
    saved = null
  }
  dashboardLayout.value = normaliseDashboardLayout(saved, currentCameraIds.value)
  focusedCameraId.value = null
  manuallyFocused.value = false
}

function persistLayout() {
  localStorage.setItem(layoutStorageKey.value, JSON.stringify(dashboardLayout.value))
}

function beginLayoutEdit() {
  layoutMessage.value = ''
  layoutBeforeEditing = cloneLayout(dashboardLayout.value)
  // Generated presets do not overwrite the independently saved custom canvas.
  dashboardLayout.value.preset = 'custom'
  editingLayoutMode.value = 'regular'
  focusPreviewCameraId.value = currentCameraIds.value[0] ?? null
  editingLayout.value = true
}

function openLayoutEditor() {
  if (!canvasMode.value) selectDashboardMode('layout')
  beginLayoutEdit()
}

function saveLayout() {
  if (!canSaveLayout.value) return
  stopActivityDrag()
  persistLayout()
  layoutBeforeEditing = null
  editingLayout.value = false
}

function cancelLayoutEdit() {
  stopActivityDrag()
  if (layoutBeforeEditing) dashboardLayout.value = layoutBeforeEditing
  layoutBeforeEditing = null
  editingLayout.value = false
  editingLayoutMode.value = 'regular'
}

function resetLayout() {
  if (editingLayoutMode.value === 'focus') {
    dashboardLayout.value = { ...dashboardLayout.value, ...createDefaultFocusLayout(currentCameraIds.value.length) }
  } else {
    dashboardLayout.value = {
      ...dashboardLayout.value,
      tiles: createDefaultDashboardLayout(currentCameraIds.value).tiles,
    }
  }
}

function updateFullscreenLayout(tiles: DashboardTileLayout[]) {
  if (!editingLayout.value || editingLayoutMode.value === 'regular') {
    dashboardLayout.value = { ...dashboardLayout.value, tiles }
    return
  }

  const previewId = focusPreviewCameraId.value
  const focusTile = tiles.find(tile => tile.cameraId === previewId)
  if (!focusTile) return
  const otherIds = currentCameraIds.value.filter(id => id !== previewId)
  const slots = otherIds.map(cameraId => tiles.find(tile => tile.cameraId === cameraId)).filter(tile => tile !== undefined)
  if (slots.length !== otherIds.length) return
  dashboardLayout.value = {
    ...dashboardLayout.value,
    focusRegion: { x: focusTile.x, y: focusTile.y, width: focusTile.width, height: focusTile.height },
    focusSlots: slots.map(tile => ({ x: tile.x, y: tile.y, width: tile.width, height: tile.height })),
  }
}

function setLayoutEditorMode(mode: 'regular' | 'focus') {
  editingLayoutMode.value = mode
  if (mode === 'focus' && focusPreviewCameraId.value === null) {
    focusPreviewCameraId.value = currentCameraIds.value[0] ?? null
  }
}

function setActivityDock(dock: ActivityDock) {
  dashboardLayout.value = { ...dashboardLayout.value, activityDock: dock }
}

function changeActivitySize(delta: number) {
  dashboardLayout.value = {
    ...dashboardLayout.value,
    activitySize: Math.max(72, Math.min(320, dashboardLayout.value.activitySize + delta)),
  }
}

function toggleAutoFocus() {
  if (!dashboardLayout.value.autoFocusEnabled && !focusAllowed.value) {
    layoutMessage.value = 'Adjust the focus layout so it does not overlap activity.'
    return
  }
  dashboardLayout.value = {
    ...dashboardLayout.value,
    autoFocusEnabled: !dashboardLayout.value.autoFocusEnabled,
  }
  if (!dashboardLayout.value.autoFocusEnabled && !manuallyFocused.value) focusedCameraId.value = null
  persistLayout()
}

function toggleFocusEligibility(cameraId: number) {
  const eligible = new Set(dashboardLayout.value.focusEligibleCameraIds)
  if (eligible.has(cameraId)) eligible.delete(cameraId)
  else eligible.add(cameraId)
  dashboardLayout.value = { ...dashboardLayout.value, focusEligibleCameraIds: [...eligible] }
}

function toggleCameraVisibility(cameraId: number) {
  const visible = new Set(dashboardLayout.value.visibleCameraIds)
  if (visible.has(cameraId)) visible.delete(cameraId)
  else visible.add(cameraId)
  dashboardLayout.value = { ...dashboardLayout.value, visibleCameraIds: [...visible] }
}

function focusCamera(cameraId: number) {
  if (!focusAllowed.value) return
  if (focusedCameraId.value === cameraId && manuallyFocused.value) {
    focusedCameraId.value = null
    manuallyFocused.value = false
    updateAutomaticFocus()
    return
  }
  if (focusReleaseTimer) clearTimeout(focusReleaseTimer)
  focusedCameraId.value = cameraId
  manuallyFocused.value = true
}

function cameraFocusPriority(cameraId: number) {
  const rank = dashboardLayout.value.priorityCameraIds.indexOf(cameraId)
  return rank < 0 ? 0 : dashboardLayout.value.priorityCameraIds.length - rank
}

function updateAutomaticFocus() {
  if (!focusAllowed.value) {
    manuallyFocused.value = false
    focusedCameraId.value = null
  }
  if (manuallyFocused.value) return
  if (!dashboardLayout.value.autoFocusEnabled || !focusAllowed.value) {
    if (focusReleaseTimer) { clearTimeout(focusReleaseTimer); focusReleaseTimer = null }
    focusedCameraId.value = null
    return
  }

  const eligible = new Set(dashboardLayout.value.focusEligibleCameraIds)
  const active = cameraStore.cameras
    .filter(camera => currentCameraIds.value.includes(camera.id) && eligible.has(camera.id) && camera.motionActive)
    .sort((a, b) => cameraFocusPriority(b.id) - cameraFocusPriority(a.id))
  const best = active[0]?.id ?? null
  const current = focusedCameraId.value

  if (current === null) {
    focusedCameraId.value = best
    return
  }
  if (active.some(camera => camera.id === current)) {
    if (focusReleaseTimer) { clearTimeout(focusReleaseTimer); focusReleaseTimer = null }
    if (best !== null && cameraFocusPriority(best) > cameraFocusPriority(current)) focusedCameraId.value = best
    return
  }

  if (focusReleaseTimer) return // unrelated camera motion must not extend this hold forever
  focusReleaseTimer = setTimeout(() => {
    focusReleaseTimer = null
    focusedCameraId.value = null
    updateAutomaticFocus()
  }, dashboardLayout.value.focusHoldSeconds * 1000)
}

function toggleRecentActivity() {
  showRecentActivity.value = !showRecentActivity.value
  localStorage.setItem(DASHBOARD_ACTIVITY_KEY, showRecentActivity.value ? '1' : '0')
}

function selectGroup(id: number | null) {
  if (editingLayout.value) cancelLayoutEdit()
  selectedGroupId.value = id
  if (id === null) {
    localStorage.removeItem(DASHBOARD_GROUP_KEY)
  } else {
    localStorage.setItem(DASHBOARD_GROUP_KEY, String(id))
  }
}

const groupCameraIds = computed(() => {
  if (selectedGroupId.value === null) return null
  return new Set(groupStore.camerasInGroup(selectedGroupId.value).map(c => c.id))
})

const hasDashboardActivity = computed(() => {
  const included = (cameraId: number) =>
    !groupCameraIds.value || groupCameraIds.value.has(cameraId)
  return cameraStore.cameras.some(c => c.isRecording && included(c.id)) ||
    clipStore.recentClips.some(c => c.duration >= 2 && included(c.camera))
})

const reserveActivitySpace = computed(() =>
  canvasMode.value && showRecentActivity.value && (hasDashboardActivity.value || editingLayout.value),
)

const fullscreenInsets = computed(() => {
  const insets = { top: 0, right: 0, bottom: 0, left: 0 }
  if (reserveActivitySpace.value && !rectangleActive.value) insets[dashboardLayout.value.activityDock] = dashboardLayout.value.activitySize
  return insets
})

const fullscreenActivityStyle = computed(() => {
  if (rectangleActive.value) {
    const area = dashboardLayout.value.activityRegion
    return { left: `${area.x / 12 * 100}%`, top: `${area.y / 12 * 100}%`,
      width: `${area.width / 12 * 100}%`, height: `${area.height / 12 * 100}%` }
  }
  const size = `${dashboardLayout.value.activitySize}px`
  switch (dashboardLayout.value.activityDock) {
    case 'top': return { top: '0', left: '0', right: '0', height: size }
    case 'right': return { top: '0', right: '0', bottom: '0', width: size }
    case 'left': return { top: '0', left: '0', bottom: '0', width: size }
    default: return { left: '0', right: '0', bottom: '0', height: size }
  }
})

const activityOrientation = computed(() =>
  rectangleActive.value ? dashboardLayout.value.activityDirection :
  dashboardLayout.value.activityDock === 'left' || dashboardLayout.value.activityDock === 'right'
    ? 'vertical' as const
    : 'horizontal' as const,
)

const fullscreenControlsStyle = computed(() => ({
  top: `${fullscreenInsets.value.top + 10}px`,
  right: `${fullscreenInsets.value.right + 56}px`,
}))

watch([selectedGroupId, () => currentCameraIds.value.join(',')], () => {
  if (!editingLayout.value) loadLayout()
}, { immediate: true })

watch(() => settings.fullscreenMode, fullscreen => {
  if (fullscreenControlsTimer) {
    clearTimeout(fullscreenControlsTimer)
    fullscreenControlsTimer = null
  }
  fullscreenControlsVisible.value = true
  if (fullscreen) showFullscreenControls()
})

watch(
  [() => cameraStore.cameras.map(camera => `${camera.id}:${camera.motionActive ? 1 : 0}`).join(','),
    () => dashboardLayout.value.autoFocusEnabled,
    () => dashboardLayout.value.focusEligibleCameraIds.join(','),
    () => dashboardLayout.value.priorityCameraIds.join(','), focusAllowed],
  updateAutomaticFocus,
)

onUnmounted(() => {
  stopActivityDrag()
  if (focusReleaseTimer) clearTimeout(focusReleaseTimer)
  if (fullscreenControlsTimer) clearTimeout(fullscreenControlsTimer)
  window.removeEventListener('pointermove', showFullscreenControls)
  window.removeEventListener('pointerdown', showFullscreenControls)
  window.removeEventListener('keydown', showFullscreenControls)
})

onMounted(async () => {
  window.addEventListener('pointermove', showFullscreenControls, { passive: true })
  window.addEventListener('pointerdown', showFullscreenControls, { passive: true })
  window.addEventListener('keydown', showFullscreenControls)
  if (settings.fullscreenMode) showFullscreenControls()
  await cameraStore.fetchCameras()
  if (groupStore.groups.length === 0) {
    await groupStore.fetchGroups()
  }
  // Clear saved group if it no longer exists
  if (selectedGroupId.value !== null &&
      !groupStore.activeGroups.some(g => g.id === selectedGroupId.value)) {
    selectGroup(null)
  }
})
</script>

<template>
  <AppLayout>
    <template #title>Dashboard</template>
    <template #actions>
      <div class="dashboard-toolbar d-flex align-items-center gap-2">
        <div class="btn-group btn-group-sm" aria-label="Dashboard view">
          <button v-for="mode in (['tiles', 'layout', 'fullscreen'] as DashboardMode[])" :key="mode"
            class="btn" :class="dashboardMode === mode ? 'btn-primary' : 'btn-outline-secondary'"
            :aria-pressed="dashboardMode === mode" @click="selectDashboardMode(mode)">
            {{ mode === 'fullscreen' ? 'Full Screen' : mode === 'layout' ? 'Layout' : 'Tiles' }}
          </button>
        </div>
        <select v-if="canvasMode && !editingLayout" class="form-select form-select-sm dashboard-preset"
          :value="dashboardLayout.preset" aria-label="Layout preset" @change="changePreset">
          <option value="custom">Custom layout</option>
          <option value="hero7">1 large + 2 medium + 4 small</option>
          <option value="balanced8">4 medium + 4 small</option>
          <option value="grid16">16 small</option>
        </select>
        <!-- Group picker -->
        <div v-if="groupStore.activeGroups.length > 0" class="btn-group btn-group-sm me-2">
          <button
            class="btn"
            :class="selectedGroupId === null ? 'btn-primary' : 'btn-outline-secondary'"
            @click="selectGroup(null)"
          >All</button>
          <button
            v-for="group in groupStore.activeGroups"
            :key="group.id"
            class="btn"
            :class="selectedGroupId === group.id ? 'btn-primary' : 'btn-outline-secondary'"
            @click="selectGroup(group.id)"
          >{{ group.displayName }}</button>
        </div>
        <button
          class="btn btn-sm btn-outline-secondary mobile-hide"
          @click="settings.decreaseScale"
          title="Zoom out"
        >−</button>
        <span class="small text-muted-custom mobile-hide" style="min-width: 3rem; text-align: center;">
          {{ settings.cameraPreviewScale }}%
        </span>
        <button
          class="btn btn-sm btn-outline-secondary mobile-hide"
          @click="settings.increaseScale"
          title="Zoom in"
        >+</button>
        <button
          class="btn btn-sm btn-outline-secondary"
          @click="openLayoutEditor"
          title="Edit this group's saved custom layout"
        >Arrange</button>
        <button
          class="btn btn-sm"
          :class="settings.fullscreenMode ? 'btn-primary' : 'btn-outline-secondary'"
          @click="settings.toggleFullscreen"
          title="Fullscreen"
          style="display: inline-flex; align-items: center; gap: 0.35rem;"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/>
            <line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/>
          </svg>
          <span class="mobile-hide">Fullscreen</span>
        </button>
        <div class="vr mx-1" style="border-color: var(--bs-border-color);"></div>
        <button
          class="btn btn-sm"
          :class="settings.streamingMode !== 'jpeg' ? 'btn-outline-primary' : 'btn-outline-secondary'"
          @click="settings.toggleStreamingMode"
          :title="'Playback mode: ' + settings.streamingMode.toUpperCase() + ' (click to change)'"
        >
          {{ settings.streamingMode === 'jpeg' ? 'Snapshots' : 'Video' }} <span class="mode-detail mobile-hide">{{ settings.streamingMode.toUpperCase() }}</span>
        </button>
        <button
          class="btn btn-sm"
          :class="showRecentActivity ? 'btn-outline-primary' : 'btn-outline-secondary'"
          @click="toggleRecentActivity"
          :title="showRecentActivity ? 'Hide recent activity' : 'Show recent activity'"
          :aria-pressed="showRecentActivity"
        >Activity</button>
      </div>
    </template>

    <section v-if="!settings.fullscreenMode" class="view-heading" aria-label="Live overview">
      <div><div class="ui-eyebrow">LIVE OVERVIEW</div><h1>{{ selectedGroupId === null ? 'Your cameras' : groupStore.getGroupById(selectedGroupId)?.displayName ?? 'Your cameras' }}</h1><p>Keep an eye on what matters. Open a camera for a closer look.</p></div>
      <span class="ui-count-pill">{{ currentCameras.length }} cameras · {{ currentCameras.filter(camera => camera.status === 'Connected').length }} connected</span>
    </section>
    <p v-if="layoutMessage || (!editingLayout && regularActivityConflict)" class="layout-notice" role="status">
      {{ layoutMessage || 'Activity is docked because the current cameras occupy its saved rectangle. Use Arrange to adjust it.' }}
    </p>
    <div ref="stageRef" class="dashboard-stage" :class="{ 'canvas-stage': canvasMode }">
    <CameraGrid
      :layout-mode="layoutView"
      :group-camera-ids="groupCameraIds"
      :fullscreen-insets="fullscreenInsets"
      :fullscreen-layout="editorTiles"
      :editing-layout="editingLayout"
      :editing-layout-mode="editingLayoutMode"
      :fullscreen-controls-visible="fullscreenControlsVisible"
      :focused-camera-id="displayedFocusedCameraId"
      :focus-eligible-camera-ids="dashboardLayout.focusEligibleCameraIds"
      :visible-camera-ids="displayedCameraIds"
      :focus-region="dashboardLayout.focusRegion"
      :focus-slots="dashboardLayout.focusSlots"
      :focus-allowed="focusAllowed"
      @update-fullscreen-layout="updateFullscreenLayout"
      @cancel-layout="cancelLayoutEdit"
      @focus-camera="focusCamera"
      @toggle-focus-eligibility="toggleFocusEligibility"
      @toggle-camera-visibility="toggleCameraVisibility"
    />

    <div
      v-if="showRecentActivity"
      class="dashboard-activity-strip"
      :class="{ 'dashboard-activity-strip-fullscreen': settings.fullscreenMode, 'dashboard-activity-strip-layout': layoutView && !settings.fullscreenMode,
        'activity-rectangle-editing': rectangleActive && editingLayout, 'activity-region-invalid': rectangleActive && (editingLayoutMode === 'focus' ? focusActivityConflict : regularActivityConflict) }"
      :style="canvasMode ? fullscreenActivityStyle : undefined"
    >
    <ActivityStrip
      :camera-ids="groupCameraIds"
      :orientation="canvasMode ? activityOrientation : 'horizontal'"
      :force-visible="canvasMode && editingLayout"
      :fill="canvasMode"
      @play="playingClip = $event"
    />
      <template v-if="rectangleActive && editingLayout">
        <button class="activity-move-handle" @pointerdown="startActivityDrag($event, 'move')" aria-label="Drag to move activity panel">Move activity</button>
        <button class="activity-resize-handle" @pointerdown="startActivityDrag($event, 'resize')" aria-label="Drag to resize activity panel">↘</button>
      </template>
    </div>
    </div>

    <div
      v-if="settings.fullscreenMode"
      class="fullscreen-dashboard-controls fullscreen-fading-controls"
      :class="{ visible: fullscreenControlsVisible }"
      :style="fullscreenControlsStyle"
    >
      <button
        v-if="!editingLayout"
        class="fullscreen-focus-toggle"
        :class="{ active: dashboardLayout.autoFocusEnabled }"
        @click="toggleAutoFocus"
        :title="dashboardLayout.autoFocusEnabled ? 'Disable focus on motion' : 'Focus eligible cameras on motion'"
      >Auto focus</button>
      <button
        v-if="!editingLayout"
        class="fullscreen-layout-toggle"
        @click="beginLayoutEdit"
        title="Edit this group's fullscreen layout"
      >Layout</button>
      <button
        class="fullscreen-activity-toggle"
        :class="showRecentActivity ? 'active' : ''"
        @click="toggleRecentActivity"
        :title="showRecentActivity ? 'Hide recent activity' : 'Show recent activity'"
      >Activity</button>
      <select v-if="!editingLayout" :value="dashboardLayout.preset" aria-label="Fullscreen layout preset" @change="changePreset">
        <option value="custom">Custom</option><option value="hero7">1 + 2 + 4</option>
        <option value="balanced8">4 + 4</option><option value="grid16">16 small</option>
      </select>
    </div>

    <div
      v-if="canvasMode && editingLayout"
      class="layout-editor-toolbar fullscreen-fading-controls"
      :class="{ visible: !settings.fullscreenMode || fullscreenControlsVisible }"
    >
      <button :class="{ active: editingLayoutMode === 'regular' }" @click="setLayoutEditorMode('regular')">Regular</button>
      <button :class="{ active: editingLayoutMode === 'focus' }" @click="setLayoutEditorMode('focus')">Auto-focus</button>
      <select
        v-if="editingLayoutMode === 'focus'"
        v-model.number="focusPreviewCameraId"
        title="Camera shown in the focus region while editing"
      >
        <option v-for="camera in currentCameras" :key="camera.id" :value="camera.id">{{ camera.name }}</option>
      </select>
      <span class="layout-editor-separator" />
      <span class="layout-editor-label">Activity</span>
      <select :value="dashboardLayout.activityPlacement" aria-label="Activity placement"
        @change="setActivityPlacement(($event.target as HTMLSelectElement).value as 'dock' | 'rectangle')">
        <option value="dock">Edge dock</option><option value="rectangle">Free rectangle</option>
      </select>
      <template v-if="dashboardLayout.activityPlacement === 'dock'">
      <button
        v-for="dock in (['top', 'right', 'bottom', 'left'] as ActivityDock[])"
        :key="dock"
        :class="{ active: dashboardLayout.activityDock === dock }"
        @click="setActivityDock(dock)"
      >{{ dock }}</button>
      <button title="Make activity panel smaller" @click="changeActivitySize(-16)">−</button>
      <button title="Make activity panel larger" @click="changeActivitySize(16)">+</button>
      </template>
      <template v-else>
        <select v-model="dashboardLayout.activityDirection" aria-label="Activity direction">
          <option value="horizontal">Horizontal</option><option value="vertical">Vertical</option>
        </select>
        <label v-for="field in (['x', 'y', 'width', 'height'] as const)" :key="field" class="activity-coordinate">
          {{ field }}<input type="number" :aria-label="`Activity ${field}`" :min="field === 'x' || field === 'y' ? 0 : 1"
            :max="field === 'x' || field === 'y' ? 11 : 12" :value="dashboardLayout.activityRegion[field]"
            @change="setActivityCoordinate(field, $event)">
        </label>
      </template>
      <span class="layout-editor-separator" />
      <button
        :class="{ active: dashboardLayout.autoFocusEnabled }"
        @click="dashboardLayout.autoFocusEnabled = !dashboardLayout.autoFocusEnabled"
      >Auto focus</button>
      <span class="layout-editor-separator" />
      <button @click="resetLayout">Reset</button>
      <button @click="cancelLayoutEdit">Cancel</button>
      <button class="primary" :disabled="!canSaveLayout" @click="saveLayout">Save</button>
      <span v-if="!canSaveLayout" class="layout-conflict" role="status">{{ regularCameraConflict ? 'Cameras overlap.' : regularActivityConflict ? 'Activity overlaps a regular camera.' : focusCameraConflict ? 'Focus camera regions overlap.' : 'Activity overlaps the enabled focus layout.' }} Move or resize regions before saving.</span>
      <details class="priority-editor"><summary>Camera priority</summary>
        <div v-for="(cameraId, rank) in dashboardLayout.priorityCameraIds" :key="cameraId">
          <span>{{ rank + 1 }}. {{ cameraStore.getCameraById(cameraId)?.name ?? cameraId }}</span>
          <button :disabled="rank === 0" :aria-label="`Raise priority of camera ${cameraId}`" @click="moveCameraPriority(cameraId, -1)">↑</button>
          <button :disabled="rank === dashboardLayout.priorityCameraIds.length - 1" :aria-label="`Lower priority of camera ${cameraId}`" @click="moveCameraPriority(cameraId, 1)">↓</button>
        </div>
      </details>
    </div>

    <ClipPlayer v-if="playingClip" :clip="playingClip" @close="playingClip = null" />
  </AppLayout>
</template>

<style scoped>
.dashboard-toolbar { padding: 0.25rem 0; }
.dashboard-toolbar .btn { white-space: nowrap; }
.mode-detail { margin-left: 0.35rem; font-size: 0.65rem; opacity: 0.75; }
.dashboard-stage { position: relative; }
.dashboard-preset { width: auto; max-width: 260px; }
.dashboard-activity-strip-layout { position: absolute; margin: 0; z-index: 4; }
.layout-notice { color: var(--bs-warning-text-emphasis); background: var(--bs-warning-bg-subtle); padding: .5rem .75rem; border-radius: .5rem; }
.activity-rectangle-editing { outline: 2px solid #4ea5ff; }
.activity-region-invalid { outline-color: #ff6474; }
.activity-move-handle, .activity-resize-handle { position: absolute; z-index: 5; background: #142235; color: #fff; border: 1px solid #4ea5ff; border-radius: .25rem; touch-action: none; }
.activity-move-handle { top: 0; left: 50%; transform: translateX(-50%); cursor: move; font-size: .7rem; }
.activity-resize-handle { right: 0; bottom: 0; cursor: nwse-resize; }
.activity-coordinate { display: flex; align-items: center; gap: 3px; }
.activity-coordinate input { width: 3rem; color: #fff; background: #16161d; border: 1px solid #456; border-radius: .25rem; }
.layout-conflict { color: #ff9ba6; }
.priority-editor { position: relative; }
.priority-editor[open] { max-height: 70vh; overflow: auto; min-width: 240px; }
.priority-editor div { display: flex; align-items: center; gap: 4px; margin-top: 4px; }
.priority-editor div span { flex: 1; }
.fullscreen-dashboard-controls select { max-width: 150px; background: #142235; color: #dceafa; border: 1px solid #456; border-radius: .375rem; }
.dashboard-activity-strip {
  margin-top: 1rem;
  margin-bottom: 0;
}
.dashboard-activity-strip-layout { margin: 0; }

.dashboard-activity-strip-fullscreen {
  position: fixed;
  z-index: 1001;
  margin: 0;
  border-radius: 0;
  border-width: 1px 0 0;
  background: #111c2b;
}

.fullscreen-dashboard-controls {
  position: fixed;
  z-index: 1002;
  display: flex;
  gap: 6px;
  transition: top 0.15s, right 0.15s, opacity 0.2s;
}

.fullscreen-fading-controls {
  opacity: 0;
  pointer-events: none;
}

.fullscreen-fading-controls.visible {
  opacity: 1;
  pointer-events: auto;
}

.fullscreen-dashboard-controls button {
  padding: 6px 10px;
  border: 1px solid rgba(255, 255, 255, 0.3);
  border-radius: 0.375rem;
  background: #142235;
  color: #dceafa;
  font-size: 0.75rem;
  cursor: pointer;
}

.fullscreen-focus-toggle.active { background: rgba(13, 110, 253, 0.8); color: #fff; }

.layout-editor-toolbar {
  position: fixed;
  top: 10px;
  left: 50%;
  transform: translateX(-50%);
  z-index: 1004;
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  max-width: calc(100vw - 32px);
  width: max-content;
  gap: 4px;
  padding: 6px;
  border: 1px solid rgba(255, 255, 255, 0.3);
  border-radius: 0.5rem;
  background: rgba(12, 12, 16, 0.94);
  color: #fff;
  font-size: 0.72rem;
  transition: opacity 0.2s;
}

.layout-editor-toolbar button {
  border: 1px solid rgba(255, 255, 255, 0.25);
  border-radius: 0.3rem;
  background: transparent;
  color: rgba(255, 255, 255, 0.8);
  padding: 3px 7px;
}

.layout-editor-toolbar select {
  max-width: 150px;
  border: 1px solid rgba(255, 255, 255, 0.25);
  border-radius: 0.3rem;
  background: #16161d;
  color: #fff;
  padding: 3px 6px;
  font-size: 0.72rem;
}

.layout-editor-toolbar button:hover,
.layout-editor-toolbar button.active { background: rgba(13, 110, 253, 0.65); color: #fff; }
.layout-editor-toolbar button.primary { background: #0d6efd; color: #fff; }
.layout-editor-toolbar button:disabled { opacity: .4; cursor: not-allowed; }
.layout-editor-label { margin: 0 2px; color: rgba(255, 255, 255, 0.65); }
.layout-editor-separator { width: 1px; height: 18px; margin: 0 3px; background: rgba(255, 255, 255, 0.25); }

.fullscreen-activity-toggle:hover,
.fullscreen-activity-toggle.active {
  background: rgba(13, 110, 253, 0.8);
  color: #fff;
}
</style>
