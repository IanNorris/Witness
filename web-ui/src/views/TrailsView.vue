<script setup lang="ts">
import { ref, computed, onMounted, watch, nextTick, onUnmounted } from 'vue'
import AppLayout from '../components/layout/AppLayout.vue'
import { useCameraStore } from '../stores/cameras'
import { useSettingsStore } from '../stores/settings'
import { api } from '../composables/useApi'
import { CLASS_COLORS } from '../composables/useDetectionOverlay'

interface TrailPoint {
  x: number
  y: number
  t: number
}

interface Trail {
  id: number
  cls: string
  name?: string
  points: TrailPoint[]
}

interface TrailsResponse {
  trails: Trail[]
  cameraId: number
}

const cameraStore = useCameraStore()
const settings = useSettingsStore()

const selectedCameraId = ref<number | null>(null)
const timeRangePreset = ref('1h')
const customFrom = ref('')
const customTo = ref('')
const enabledClasses = ref<Set<string>>(new Set(Object.keys(CLASS_COLORS)))
const trails = ref<Trail[]>([])
const loading = ref(false)
const error = ref('')
const hoveredTrail = ref<Trail | null>(null)
const hoveredTimestamp = ref<number | null>(null)
const tooltipPos = ref({ x: 0, y: 0 })

const canvasRef = ref<HTMLCanvasElement | null>(null)
const containerRef = ref<HTMLDivElement | null>(null)
const snapshotImg = ref<HTMLImageElement | null>(null)
const snapshotLoaded = ref(false)
const snapshotFailed = ref(false)
const baseSnapshotUrl = computed(() =>
  selectedCameraId.value ? `/camera/previewLarge/${selectedCameraId.value}` : '',
)
const activeSnapshotUrl = ref('')
let hoverFrameCache = new Map<string, boolean>() // tracks which frame URLs have loaded

const allClasses = Object.keys(CLASS_COLORS)

const snapshotUrl = computed(() => activeSnapshotUrl.value || baseSnapshotUrl.value)

const timeRange = computed(() => {
  if (timeRangePreset.value === 'custom') {
    return {
      from: customFrom.value ? new Date(customFrom.value).getTime() / 1000 : 0,
      to: customTo.value ? new Date(customTo.value).getTime() / 1000 : Date.now() / 1000,
    }
  }
  const now = Date.now() / 1000
  const hours: Record<string, number> = { '1h': 1, '6h': 6, '24h': 24, '7d': 168 }
  const h = hours[timeRangePreset.value] ?? 1
  return { from: now - h * 3600, to: now }
})

const filteredTrails = computed(() =>
  trails.value.filter(t => enabledClasses.value.has(t.cls.toLowerCase())),
)

function hexToRgb(hex: string): { r: number; g: number; b: number } {
  const n = parseInt(hex.replace('#', ''), 16)
  return { r: (n >> 16) & 0xff, g: (n >> 8) & 0xff, b: n & 0xff }
}

function getTrailColor(cls: string): string {
  return CLASS_COLORS[cls.toLowerCase()] || '#ffff44'
}

// Compute the "object-fit: contain" dimensions
function getViewRect(canvas: HTMLCanvasElement, img: HTMLImageElement) {
  const cw = canvas.width
  const ch = canvas.height
  const iw = img.naturalWidth || cw
  const ih = img.naturalHeight || ch
  const scale = Math.min(cw / iw, ch / ih)
  const drawW = iw * scale
  const drawH = ih * scale
  const offsetX = (cw - drawW) / 2
  const offsetY = (ch - drawH) / 2
  return { drawW, drawH, offsetX, offsetY }
}

function drawTrails() {
  const canvas = canvasRef.value
  const img = snapshotImg.value
  if (!canvas || !img) return

  const ctx = canvas.getContext('2d')
  if (!ctx) return

  ctx.clearRect(0, 0, canvas.width, canvas.height)

  if (!snapshotLoaded.value) return

  const vr = getViewRect(canvas, img)
  const baseOpacity = settings.trailOpacity / 100

  ctx.lineCap = 'round'
  ctx.lineJoin = 'round'
  ctx.lineWidth = 2

  for (const trail of filteredTrails.value) {
    const pts = trail.points
    if (pts.length === 0) continue

    const isHovered = hoveredTrail.value?.id === trail.id
    const color = getTrailColor(trail.cls)
    const { r, g, b } = hexToRgb(color)
    const opacity = isHovered ? 1 : baseOpacity

    // Draw line segments with gradient opacity
    for (let i = 1; i < pts.length; i++) {
      const t0 = pts.length <= 1 ? 0 : (i - 1) / (pts.length - 1)
      const t1 = i / (pts.length - 1)
      const a0 = opacity * (0.2 + 0.8 * t0)
      const a1 = opacity * (0.2 + 0.8 * t1)

      const x0 = vr.offsetX + pts[i - 1]!.x * vr.drawW
      const y0 = vr.offsetY + pts[i - 1]!.y * vr.drawH
      const x1 = vr.offsetX + pts[i]!.x * vr.drawW
      const y1 = vr.offsetY + pts[i]!.y * vr.drawH

      const avgAlpha = (a0 + a1) / 2

      ctx.beginPath()
      ctx.strokeStyle = `rgba(${r},${g},${b},${avgAlpha})`
      ctx.lineWidth = isHovered ? 3 : 2
      ctx.moveTo(x0, y0)
      ctx.lineTo(x1, y1)
      ctx.stroke()
    }

    // Draw dots
    for (let i = 0; i < pts.length; i++) {
      const t = pts.length <= 1 ? 1 : i / (pts.length - 1)
      const alpha = opacity * (0.2 + 0.8 * t)
      const isHead = i === pts.length - 1
      const radius = isHovered ? (isHead ? 6 : 3) : (isHead ? 4 : 2)

      const px = vr.offsetX + pts[i]!.x * vr.drawW
      const py = vr.offsetY + pts[i]!.y * vr.drawH

      ctx.beginPath()
      ctx.fillStyle = `rgba(${r},${g},${b},${alpha})`
      ctx.arc(px, py, radius, 0, Math.PI * 2)
      ctx.fill()
    }
  }
}

async function fetchTrails() {
  if (!selectedCameraId.value) return

  loading.value = true
  error.value = ''
  try {
    const { from, to } = timeRange.value
    const url = `/api/trails/${selectedCameraId.value}?from=${Math.floor(from)}&to=${Math.floor(to)}&anchor=${settings.trailAnchor}`
    const data = await api<TrailsResponse>(url)
    trails.value = data.trails || []
    await nextTick()
    drawTrails()
  } catch (e: unknown) {
    error.value = e instanceof Error ? e.message : 'Failed to fetch trails'
    trails.value = []
  } finally {
    loading.value = false
  }
}

function syncCanvasSize() {
  const canvas = canvasRef.value
  const container = containerRef.value
  if (!canvas || !container) return

  const rect = container.getBoundingClientRect()
  canvas.width = rect.width
  canvas.height = rect.height
  drawTrails()
}

function onSnapshotLoad() {
  snapshotLoaded.value = true
  snapshotFailed.value = false
  syncCanvasSize()
}

function onSnapshotError() {
  snapshotLoaded.value = false
  snapshotFailed.value = true
}

// Hit-test trails on mouse move — find closest point and show frame at that timestamp
function onCanvasMouseMove(e: MouseEvent) {
  const canvas = canvasRef.value
  const img = snapshotImg.value
  if (!canvas || !img || !snapshotLoaded.value) return

  const rect = canvas.getBoundingClientRect()
  const mx = e.clientX - rect.left
  const my = e.clientY - rect.top

  const vr = getViewRect(canvas, img)
  const HIT_DIST = 10

  let closest: Trail | null = null
  let closestDist = HIT_DIST
  let closestTimestamp: number | null = null

  for (const trail of filteredTrails.value) {
    for (const pt of trail.points) {
      const px = vr.offsetX + pt.x * vr.drawW
      const py = vr.offsetY + pt.y * vr.drawH
      const dist = Math.hypot(mx - px, my - py)
      if (dist < closestDist) {
        closestDist = dist
        closest = trail
        closestTimestamp = pt.t
      }
    }
  }

  if (closest !== hoveredTrail.value || closestTimestamp !== hoveredTimestamp.value) {
    hoveredTrail.value = closest
    hoveredTimestamp.value = closestTimestamp
    tooltipPos.value = { x: e.clientX, y: e.clientY }
    drawTrails()

    // Update background to show detection frame at hovered timestamp
    if (closest && closestTimestamp && selectedCameraId.value) {
      const frameFile = closestTimestamp.toFixed(3) + '.jpg'
      const frameUrl = `/api/detection/frame/${selectedCameraId.value}/${frameFile}`
      // Preload to avoid flicker — only swap if image loads
      if (hoverFrameCache.has(frameUrl)) {
        activeSnapshotUrl.value = frameUrl
      } else {
        const probe = new Image()
        probe.onload = () => {
          hoverFrameCache.set(frameUrl, true)
          // Only update if still hovering the same point
          if (hoveredTimestamp.value === closestTimestamp) {
            activeSnapshotUrl.value = frameUrl
          }
        }
        probe.onerror = () => { hoverFrameCache.set(frameUrl, false) }
        probe.src = frameUrl
      }
    } else {
      activeSnapshotUrl.value = ''
    }
  } else if (closest) {
    tooltipPos.value = { x: e.clientX, y: e.clientY }
  }
}

function onCanvasMouseLeave() {
  if (hoveredTrail.value) {
    hoveredTrail.value = null
    hoveredTimestamp.value = null
    activeSnapshotUrl.value = ''
    drawTrails()
  }
}

function toggleClass(cls: string) {
  const s = new Set(enabledClasses.value)
  if (s.has(cls)) s.delete(cls)
  else s.add(cls)
  enabledClasses.value = s
  drawTrails()
}

function selectAllClasses() {
  enabledClasses.value = new Set(allClasses)
  drawTrails()
}

function deselectAllClasses() {
  enabledClasses.value = new Set()
  drawTrails()
}

let resizeObserver: ResizeObserver | null = null

onMounted(async () => {
  if (cameraStore.cameras.length === 0) {
    await cameraStore.fetchCameras()
  }
  if (cameraStore.cameras.length > 0 && !selectedCameraId.value) {
    selectedCameraId.value = cameraStore.cameras[0]!.id
  }

  resizeObserver = new ResizeObserver(() => syncCanvasSize())
  if (containerRef.value) resizeObserver.observe(containerRef.value)
})

onUnmounted(() => {
  resizeObserver?.disconnect()
})

watch(selectedCameraId, () => {
  snapshotLoaded.value = false
  snapshotFailed.value = false
  activeSnapshotUrl.value = ''
  hoverFrameCache = new Map()
  trails.value = []
  fetchTrails()
})

watch(timeRange, () => fetchTrails())
</script>

<template>
  <AppLayout>
    <template #title>Trails</template>
    <template #actions>
      <div class="d-flex align-items-center gap-2 flex-wrap">
        <button class="btn btn-sm btn-outline-secondary" @click="fetchTrails" :disabled="loading" title="Refresh">↻</button>
        <span v-if="filteredTrails.length > 0" class="text-muted-custom small">
          {{ filteredTrails.length }} trails
        </span>
      </div>
    </template>

    <!-- Controls panel -->
    <div class="trails-controls">
      <div class="d-flex align-items-center gap-3 flex-wrap">
        <!-- Camera selector -->
        <div class="d-flex align-items-center gap-2">
          <label class="form-label mb-0 small text-muted-custom">Camera</label>
          <select
            class="form-select form-select-sm trails-select"
            v-model="selectedCameraId"
          >
            <option :value="null" disabled>Select camera…</option>
            <option
              v-for="cam in cameraStore.cameras"
              :key="cam.id"
              :value="cam.id"
            >
              {{ cam.name }}
            </option>
          </select>
        </div>

        <!-- Time range presets -->
        <div class="btn-group btn-group-sm">
          <button
            v-for="preset in [
              { label: '1h', value: '1h' },
              { label: '6h', value: '6h' },
              { label: '24h', value: '24h' },
              { label: '7d', value: '7d' },
            ]"
            :key="preset.value"
            class="btn"
            :class="timeRangePreset === preset.value ? 'btn-primary' : 'btn-outline-secondary'"
            @click="timeRangePreset = preset.value"
          >
            {{ preset.label }}
          </button>
          <button
            class="btn"
            :class="timeRangePreset === 'custom' ? 'btn-primary' : 'btn-outline-secondary'"
            @click="timeRangePreset = 'custom'"
          >
            Custom
          </button>
        </div>

        <!-- Custom date inputs -->
        <template v-if="timeRangePreset === 'custom'">
          <input
            type="datetime-local"
            class="form-control form-control-sm trails-date-input"
            v-model="customFrom"
            placeholder="From"
          />
          <input
            type="datetime-local"
            class="form-control form-control-sm trails-date-input"
            v-model="customTo"
            placeholder="To"
          />
        </template>
      </div>

      <!-- Class filters -->
      <div class="trails-class-filters">
        <span class="small text-muted-custom me-2">Filter:</span>
        <button class="btn btn-sm btn-link text-muted-custom p-0 me-2" @click="selectAllClasses">All</button>
        <button class="btn btn-sm btn-link text-muted-custom p-0 me-3" @click="deselectAllClasses">None</button>
        <label
          v-for="cls in allClasses"
          :key="cls"
          class="form-check form-check-inline trails-class-check"
        >
          <input
            class="form-check-input"
            type="checkbox"
            :checked="enabledClasses.has(cls)"
            @change="toggleClass(cls)"
          />
          <span class="form-check-label small" :style="{ color: CLASS_COLORS[cls] }">
            {{ cls }}
          </span>
        </label>
      </div>
    </div>

    <!-- Loading state -->
    <div v-if="loading" class="text-center py-5">
      <div class="spinner-border text-primary" role="status">
        <span class="visually-hidden">Loading…</span>
      </div>
    </div>

    <!-- Error state -->
    <div v-else-if="error" class="text-danger text-center py-4 small">
      {{ error }}
    </div>

    <!-- No camera selected -->
    <div v-else-if="!selectedCameraId" class="text-muted-custom text-center py-5">
      Select a camera to view trails
    </div>

    <!-- Canvas area -->
    <div v-else class="trails-canvas-area">
      <div ref="containerRef" class="trails-canvas-container">
        <img
          v-show="snapshotLoaded"
          ref="snapshotImg"
          :src="snapshotUrl"
          class="trails-snapshot"
          @load="onSnapshotLoad"
          @error="onSnapshotError"
          crossorigin="anonymous"
        />
        <canvas
          ref="canvasRef"
          class="trails-canvas"
          @mousemove="onCanvasMouseMove"
          @mouseleave="onCanvasMouseLeave"
        />

        <!-- Empty state: no trails found -->
        <div v-if="!loading && (snapshotLoaded || snapshotFailed) && filteredTrails.length === 0" class="trails-empty-overlay">
          <div class="text-center">
            <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" class="text-muted-custom mb-2" style="opacity: 0.4">
              <path d="M22 12h-4l-3 9L9 3l-3 9H2"/>
            </svg>
            <div class="text-muted-custom">No trails found for this time range</div>
            <div class="text-muted-custom small mt-1" style="opacity: 0.6">Clips may need to be reprocessed first — use the "Reprocess All" button on the Clips page</div>
          </div>
        </div>

        <!-- Loading snapshot placeholder -->
        <div v-if="!snapshotLoaded && !snapshotFailed && selectedCameraId" class="trails-empty-overlay">
          <div class="spinner-border spinner-border-sm text-muted-custom" role="status"></div>
        </div>

        <!-- Tooltip -->
        <div
          v-if="hoveredTrail"
          class="trails-tooltip"
          :style="{
            left: tooltipPos.x + 12 + 'px',
            top: tooltipPos.y - 8 + 'px',
          }"
        >
          <span
            class="trails-tooltip-dot"
            :style="{ background: getTrailColor(hoveredTrail.cls) }"
          />
          <span>{{ hoveredTrail.name || hoveredTrail.cls }}</span>
          <span v-if="hoveredTrail.name" class="text-muted-custom small ms-1">({{ hoveredTrail.cls }})</span>
        </div>
      </div>

      <!-- Legend -->
      <div class="trails-legend">
        <span
          v-for="cls in allClasses"
          :key="cls"
          class="trails-legend-item"
          :class="{ 'trails-legend-disabled': !enabledClasses.has(cls) }"
          @click="toggleClass(cls)"
        >
          <span class="trails-legend-swatch" :style="{ background: CLASS_COLORS[cls] }" />
          {{ cls }}
        </span>
      </div>
    </div>
  </AppLayout>
</template>

<style scoped>
.trails-controls {
  margin-bottom: 1rem;
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
}

.trails-select {
  width: auto;
  min-width: 160px;
  background-color: var(--bs-dark, #1e1e2e);
  color: var(--bs-body-color, #e1e4e8);
  border-color: var(--bs-border-color, #333);
}

.trails-date-input {
  width: auto;
  min-width: 180px;
  background-color: var(--bs-dark, #1e1e2e);
  color: var(--bs-body-color, #e1e4e8);
  border-color: var(--bs-border-color, #333);
}

.trails-class-filters {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 0.25rem;
}

.trails-class-check {
  margin-right: 0.5rem;
}

.trails-canvas-area {
  display: flex;
  flex-direction: column;
  flex: 1;
  min-height: 0;
}

.trails-canvas-container {
  position: relative;
  flex: 1;
  min-height: 400px;
  background: #000;
  border-radius: 0.5rem;
  overflow: hidden;
}

.trails-snapshot {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: block;
  position: absolute;
  top: 0;
  left: 0;
}

.trails-canvas {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  cursor: crosshair;
}

.trails-empty-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  pointer-events: none;
}

.trails-tooltip {
  position: fixed;
  z-index: 9999;
  background: rgba(0, 0, 0, 0.85);
  color: #e1e4e8;
  padding: 0.25rem 0.5rem;
  border-radius: 0.25rem;
  font-size: 0.8rem;
  pointer-events: none;
  white-space: nowrap;
  display: flex;
  align-items: center;
  gap: 0.35rem;
}

.trails-tooltip-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  display: inline-block;
  flex-shrink: 0;
}

.trails-legend {
  display: flex;
  flex-wrap: wrap;
  gap: 0.75rem;
  padding: 0.5rem 0;
}

.trails-legend-item {
  display: flex;
  align-items: center;
  gap: 0.3rem;
  font-size: 0.8rem;
  cursor: pointer;
  user-select: none;
  color: var(--bs-body-color, #e1e4e8);
}

.trails-legend-disabled {
  opacity: 0.35;
}

.trails-legend-swatch {
  width: 10px;
  height: 10px;
  border-radius: 2px;
  display: inline-block;
  flex-shrink: 0;
}

@media (max-width: 768px) {
  .trails-canvas-container {
    min-height: 250px;
  }
}
</style>
