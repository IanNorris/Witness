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
  type ActivityDock,
  type DashboardLayout,
  type DashboardTileLayout,
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
    : dashboardLayout.value.tiles,
)

const displayedFocusedCameraId = computed(() => {
  if (!settings.fullscreenMode) return null
  if (editingLayout.value) {
    return editingLayoutMode.value === 'focus' ? focusPreviewCameraId.value : null
  }
  return focusedCameraId.value
})

function cloneLayout(layout: DashboardLayout): DashboardLayout {
  return JSON.parse(JSON.stringify(layout)) as DashboardLayout
}

function loadLayout() {
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
  layoutBeforeEditing = cloneLayout(dashboardLayout.value)
  editingLayoutMode.value = 'regular'
  focusPreviewCameraId.value = currentCameraIds.value[0] ?? null
  editingLayout.value = true
}

function openLayoutEditor() {
  if (!settings.fullscreenMode) settings.toggleFullscreen()
  beginLayoutEdit()
}

function saveLayout() {
  persistLayout()
  layoutBeforeEditing = null
  editingLayout.value = false
}

function cancelLayoutEdit() {
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
  const tile = dashboardLayout.value.tiles.find(item => item.cameraId === cameraId)
  return tile ? tile.width * tile.height : 0
}

function updateAutomaticFocus() {
  if (manuallyFocused.value) return
  if (focusReleaseTimer) {
    clearTimeout(focusReleaseTimer)
    focusReleaseTimer = null
  }
  if (!dashboardLayout.value.autoFocusEnabled) {
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
    if (best !== null && cameraFocusPriority(best) > cameraFocusPriority(current)) focusedCameraId.value = best
    return
  }

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
  settings.fullscreenMode && showRecentActivity.value && (hasDashboardActivity.value || editingLayout.value),
)

const fullscreenInsets = computed(() => {
  const insets = { top: 0, right: 0, bottom: 0, left: 0 }
  if (reserveActivitySpace.value) insets[dashboardLayout.value.activityDock] = dashboardLayout.value.activitySize
  return insets
})

const fullscreenActivityStyle = computed(() => {
  const size = `${dashboardLayout.value.activitySize}px`
  switch (dashboardLayout.value.activityDock) {
    case 'top': return { top: '0', left: '0', right: '0', height: size }
    case 'right': return { top: '0', right: '0', bottom: '0', width: size }
    case 'left': return { top: '0', left: '0', bottom: '0', width: size }
    default: return { left: '0', right: '0', bottom: '0', height: size }
  }
})

const activityOrientation = computed(() =>
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
    () => dashboardLayout.value.focusEligibleCameraIds.join(',')],
  updateAutomaticFocus,
)

onUnmounted(() => {
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
      <div class="d-flex align-items-center gap-2">
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
          title="Arrange this group's fullscreen dashboard"
        >Layout</button>
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
          :title="'Mode: ' + settings.streamingMode.toUpperCase()"
        >
          {{ settings.streamingMode.toUpperCase() }}
        </button>
        <button
          class="btn btn-sm"
          :class="showRecentActivity ? 'btn-outline-primary' : 'btn-outline-secondary'"
          @click="toggleRecentActivity"
          :title="showRecentActivity ? 'Hide recent activity' : 'Show recent activity'"
        >Activity</button>
      </div>
    </template>

    <CameraGrid
      :group-camera-ids="groupCameraIds"
      :fullscreen-insets="fullscreenInsets"
      :fullscreen-layout="editorTiles"
      :editing-layout="editingLayout"
      :editing-layout-mode="editingLayoutMode"
      :fullscreen-controls-visible="fullscreenControlsVisible"
      :focused-camera-id="displayedFocusedCameraId"
      :focus-eligible-camera-ids="dashboardLayout.focusEligibleCameraIds"
      :visible-camera-ids="dashboardLayout.visibleCameraIds"
      :focus-region="dashboardLayout.focusRegion"
      :focus-slots="dashboardLayout.focusSlots"
      @update-fullscreen-layout="updateFullscreenLayout"
      @cancel-layout="cancelLayoutEdit"
      @focus-camera="focusCamera"
      @toggle-focus-eligibility="toggleFocusEligibility"
      @toggle-camera-visibility="toggleCameraVisibility"
    />

    <ActivityStrip
      v-if="showRecentActivity"
      class="dashboard-activity-strip"
      :class="{ 'dashboard-activity-strip-fullscreen': settings.fullscreenMode }"
      :style="settings.fullscreenMode ? fullscreenActivityStyle : undefined"
      :camera-ids="groupCameraIds"
      :orientation="settings.fullscreenMode ? activityOrientation : 'horizontal'"
      :force-visible="settings.fullscreenMode && editingLayout"
      :fill="settings.fullscreenMode"
      @play="playingClip = $event"
    />

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
    </div>

    <div
      v-if="settings.fullscreenMode && editingLayout"
      class="layout-editor-toolbar fullscreen-fading-controls"
      :class="{ visible: fullscreenControlsVisible }"
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
      <button
        v-for="dock in (['top', 'right', 'bottom', 'left'] as ActivityDock[])"
        :key="dock"
        :class="{ active: dashboardLayout.activityDock === dock }"
        @click="setActivityDock(dock)"
      >{{ dock }}</button>
      <button title="Make activity panel smaller" @click="changeActivitySize(-16)">−</button>
      <button title="Make activity panel larger" @click="changeActivitySize(16)">+</button>
      <span class="layout-editor-separator" />
      <button
        :class="{ active: dashboardLayout.autoFocusEnabled }"
        @click="dashboardLayout.autoFocusEnabled = !dashboardLayout.autoFocusEnabled"
      >Auto focus</button>
      <span class="layout-editor-separator" />
      <button @click="resetLayout">Reset</button>
      <button @click="cancelLayoutEdit">Cancel</button>
      <button class="primary" @click="saveLayout">Save</button>
    </div>

    <ClipPlayer v-if="playingClip" :clip="playingClip" @close="playingClip = null" />
  </AppLayout>
</template>

<style scoped>
.dashboard-activity-strip {
  margin-top: 1rem;
  margin-bottom: 0;
}

.dashboard-activity-strip-fullscreen {
  position: fixed;
  z-index: 1001;
  margin: 0;
  border-radius: 0;
  border-width: 1px 0 0;
  background: rgba(15, 15, 20, 0.97);
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
  background: rgba(0, 0, 0, 0.6);
  color: rgba(255, 255, 255, 0.75);
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
.layout-editor-label { margin: 0 2px; color: rgba(255, 255, 255, 0.65); }
.layout-editor-separator { width: 1px; height: 18px; margin: 0 3px; background: rgba(255, 255, 255, 0.25); }

.fullscreen-activity-toggle:hover,
.fullscreen-activity-toggle.active {
  background: rgba(13, 110, 253, 0.8);
  color: #fff;
}
</style>
