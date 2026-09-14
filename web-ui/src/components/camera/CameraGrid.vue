<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { useCameraStore } from '../../stores/cameras'
import { useSettingsStore } from '../../stores/settings'
import CameraCard from './CameraCard.vue'
import type { DashboardGridArea, DashboardTileLayout } from '../../types/dashboardLayout'

const props = defineProps<{
  groupCameraIds?: Set<number> | null
  fullscreenInsets?: { top: number; right: number; bottom: number; left: number }
  fullscreenLayout?: DashboardTileLayout[]
  editingLayout?: boolean
  focusedCameraId?: number | null
  focusEligibleCameraIds?: number[]
  visibleCameraIds?: number[]
  focusRegion?: DashboardGridArea
  focusSlots?: DashboardGridArea[]
  editingLayoutMode?: 'regular' | 'focus'
}>()

const emit = defineEmits<{
  updateFullscreenLayout: [tiles: DashboardTileLayout[]]
  cancelLayout: []
  focusCamera: [cameraId: number]
  toggleFocusEligibility: [cameraId: number]
  toggleCameraVisibility: [cameraId: number]
}>()

const cameraStore = useCameraStore()
const settings = useSettingsStore()
const router = useRouter()

const filteredCameras = computed(() => {
  if (!props.groupCameraIds) return cameraStore.cameras
  return cameraStore.cameras.filter(c => props.groupCameraIds!.has(c.id))
})

const gridStyle = computed(() => {
  if (settings.fullscreenMode) {
    return {
      '--fs-top-inset': `${props.fullscreenInsets?.top ?? 0}px`,
      '--fs-right-inset': `${props.fullscreenInsets?.right ?? 0}px`,
      '--fs-bottom-inset': `${props.fullscreenInsets?.bottom ?? 0}px`,
      '--fs-left-inset': `${props.fullscreenInsets?.left ?? 0}px`,
    }
  }
  const minWidth = Math.max(200, (settings.cameraPreviewScale / 100) * 600)
  return {
    gridTemplateColumns: `repeat(auto-fill, minmax(${minWidth}px, 1fr))`,
  }
})

const gridRef = ref<HTMLElement | null>(null)
const activeCameraId = ref<number | null>(null)
const invalidPlacement = ref(false)
let pointerAction: 'move' | 'resize' | null = null
let pointerStartX = 0
let pointerStartY = 0
let originalTile: DashboardTileLayout | null = null
let originalTiles: DashboardTileLayout[] = []

function tileFor(cameraId: number) {
  return props.fullscreenLayout?.find(tile => tile.cameraId === cameraId)
}

function isCameraVisible(cameraId: number) {
  if (!settings.fullscreenMode || props.editingLayout) return true
  if (cameraId === props.focusedCameraId) return true
  return props.visibleCameraIds?.includes(cameraId) ?? true
}

function tileStyle(cameraId: number) {
  if (!settings.fullscreenMode) return undefined
  if (!isCameraVisible(cameraId)) return undefined
  if (props.focusedCameraId !== null && props.focusedCameraId !== undefined) {
    const others = filteredCameras.value.filter(camera =>
      camera.id !== props.focusedCameraId && isCameraVisible(camera.id),
    )
    if (cameraId === props.focusedCameraId) {
      const area = props.focusRegion ?? { x: 0, y: 0, width: others.length > 0 ? 8 : 12, height: 12 }
      return areaStyle(area)
    }
    const index = others.findIndex(camera => camera.id === cameraId)
    const configuredSlot = props.focusSlots?.[index]
    if (configuredSlot) return areaStyle(configuredSlot)
    const railColumns = others.length > 6 ? 2 : 1
    const railRows = Math.max(1, Math.ceil(others.length / railColumns))
    const cellWidth = Math.floor(4 / railColumns)
    const cellHeight = Math.floor(12 / railRows)
    const column = index % railColumns
    const row = Math.floor(index / railColumns)
    return {
      gridColumn: `${9 + column * cellWidth} / span ${column === railColumns - 1 ? 4 - column * cellWidth : cellWidth}`,
      gridRow: `${1 + row * cellHeight} / span ${row === railRows - 1 ? 12 - row * cellHeight : cellHeight}`,
    }
  }
  const tile = tileFor(cameraId)
  if (!tile) return undefined
  return {
    gridColumn: `${tile.x + 1} / span ${tile.width}`,
    gridRow: `${tile.y + 1} / span ${tile.height}`,
  }
}

function areaStyle(area: DashboardGridArea) {
  return {
    gridColumn: `${area.x + 1} / span ${area.width}`,
    gridRow: `${area.y + 1} / span ${area.height}`,
  }
}

function overlaps(candidate: DashboardTileLayout, other: DashboardTileLayout) {
  return candidate.x < other.x + other.width && candidate.x + candidate.width > other.x &&
    candidate.y < other.y + other.height && candidate.y + candidate.height > other.y
}

function startPointer(event: PointerEvent, cameraId: number, action: 'move' | 'resize') {
  if (!props.editingLayout || !gridRef.value) return
  const tile = tileFor(cameraId)
  if (!tile) return
  event.preventDefault()
  event.stopPropagation()
  activeCameraId.value = cameraId
  pointerAction = action
  pointerStartX = event.clientX
  pointerStartY = event.clientY
  originalTile = { ...tile }
  originalTiles = (props.fullscreenLayout ?? []).map(item => ({ ...item }))
  window.addEventListener('pointermove', onPointerMove)
  window.addEventListener('pointerup', finishPointer, { once: true })
}

function onPointerMove(event: PointerEvent) {
  if (!gridRef.value || !originalTile || !pointerAction) return
  const bounds = gridRef.value.getBoundingClientRect()
  const dx = Math.round((event.clientX - pointerStartX) / (bounds.width / 12))
  const dy = Math.round((event.clientY - pointerStartY) / (bounds.height / 12))
  const next = { ...originalTile }
  if (pointerAction === 'move') {
    next.x = Math.max(0, Math.min(12 - next.width, originalTile.x + dx))
    next.y = Math.max(0, Math.min(12 - next.height, originalTile.y + dy))
  } else {
    next.width = Math.max(1, Math.min(12 - next.x, originalTile.width + dx))
    next.height = Math.max(1, Math.min(12 - next.y, originalTile.height + dy))
  }
  invalidPlacement.value = originalTiles.some(tile => tile.cameraId !== next.cameraId && overlaps(next, tile))
  emit('updateFullscreenLayout', originalTiles.map(tile => tile.cameraId === next.cameraId ? next : tile))
}

function finishPointer() {
  window.removeEventListener('pointermove', onPointerMove)
  if (invalidPlacement.value) emit('updateFullscreenLayout', originalTiles)
  activeCameraId.value = null
  invalidPlacement.value = false
  pointerAction = null
  originalTile = null
}

function openStream(cameraId: number) {
  router.push(`/stream/${cameraId}`)
}

function openClips(cameraId: number) {
  router.push(`/clips/${cameraId}`)
}

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape' && settings.fullscreenMode) {
    if (props.editingLayout) emit('cancelLayout')
    else settings.toggleFullscreen()
  }
}

onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => {
  window.removeEventListener('keydown', onKeydown)
  window.removeEventListener('pointermove', onPointerMove)
})
</script>

<template>
  <div v-if="cameraStore.isLoading" class="d-flex justify-content-center py-5">
    <div class="spinner-border text-primary" role="status">
      <span class="visually-hidden">Loading...</span>
    </div>
  </div>

  <div v-else-if="filteredCameras.length === 0" class="text-center py-5 text-muted-custom">
    <p>No cameras configured</p>
  </div>

  <div
    v-else
    ref="gridRef"
    class="camera-grid"
    :class="{ fullscreen: settings.fullscreenMode, 'layout-editing': editingLayout }"
    :style="gridStyle"
  >
    <!-- Fullscreen exit button -->
    <button
      v-if="settings.fullscreenMode"
      class="fullscreen-exit-btn"
      @click="settings.toggleFullscreen"
      title="Exit fullscreen (Esc)"
    >
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
        <polyline points="4 14 10 14 10 20"/><polyline points="20 10 14 10 14 4"/>
        <line x1="14" y1="10" x2="21" y2="3"/><line x1="3" y1="21" x2="10" y2="14"/>
      </svg>
    </button>

    <div
      v-for="camera in filteredCameras"
      :key="camera.id"
      class="camera-grid-item"
      :class="{
        active: activeCameraId === camera.id,
        invalid: activeCameraId === camera.id && invalidPlacement,
        focused: focusedCameraId === camera.id,
        'dashboard-camera-hidden': !isCameraVisible(camera.id),
      }"
      :style="tileStyle(camera.id)"
    >
      <CameraCard
        :camera="camera"
        :dashboard-hidden="!isCameraVisible(camera.id)"
        @open-stream="openStream"
        @open-clips="openClips"
      />
      <div
        v-if="settings.fullscreenMode && editingLayout"
        class="layout-move-shield"
        title="Drag to move"
        @pointerdown="startPointer($event, camera.id, 'move')"
      />
      <button
        v-if="settings.fullscreenMode && editingLayout"
        class="layout-resize-handle"
        title="Drag to resize"
        @pointerdown="startPointer($event, camera.id, 'resize')"
      >↘</button>
      <button
        v-if="settings.fullscreenMode && editingLayout && editingLayoutMode !== 'focus'"
        class="layout-camera-visibility"
        :class="{ enabled: visibleCameraIds?.includes(camera.id) }"
        @pointerdown.stop
        @click.stop="emit('toggleCameraVisibility', camera.id)"
        :title="visibleCameraIds?.includes(camera.id) ? 'Hide from the regular fullscreen layout' : 'Show in the regular fullscreen layout'"
      >{{ visibleCameraIds?.includes(camera.id) ? 'Shown' : 'Hidden' }}</button>
      <button
        v-if="settings.fullscreenMode && editingLayout && editingLayoutMode !== 'focus'"
        class="layout-focus-eligibility"
        :class="{ enabled: focusEligibleCameraIds?.includes(camera.id) }"
        @pointerdown.stop
        @click.stop="emit('toggleFocusEligibility', camera.id)"
        :title="focusEligibleCameraIds?.includes(camera.id) ? 'Exclude from automatic focus' : 'Allow automatic focus'"
      >Focus {{ focusEligibleCameraIds?.includes(camera.id) ? 'on' : 'off' }}</button>
      <button
        v-if="settings.fullscreenMode && !editingLayout"
        class="camera-focus-button"
        :class="{ active: focusedCameraId === camera.id }"
        @click.stop="emit('focusCamera', camera.id)"
        :title="focusedCameraId === camera.id ? 'Return to regular layout' : `Focus ${camera.name}`"
      >{{ focusedCameraId === camera.id ? '×' : '⌗' }}</button>
    </div>
  </div>
</template>
