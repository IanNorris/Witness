<script setup lang="ts">
import { ref, computed, onMounted, watch, nextTick, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
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
  clipId: number
  cls: string
  name?: string
  points: TrailPoint[]
}

interface RawTrail {
  id: number
  clipId: number
  cls: string
  name?: string
  pts: number[][]  // compact format [[x,y,t],...]
}

interface TrailsResponse {
  trails: RawTrail[]
  cameraId: number
}

const router = useRouter()
const cameraStore = useCameraStore()
const settings = useSettingsStore()

const selectedCameraId = ref<number | null>(null)
const timeRangePreset = ref('1h')
const customFrom = ref('')
const customTo = ref('')
const enabledClasses = ref<Set<string>>(new Set(Object.keys(CLASS_COLORS)))
const trails = ref<Trail[]>([])
const loading = ref(false)
const loadingStatus = ref('')
const error = ref('')
const hoveredTrail = ref<Trail | null>(null)
const tooltipPos = ref({ x: 0, y: 0 })

// Region selection state
const regionMode = ref(false)
const regionStart = ref<{ x: number; y: number } | null>(null)
const regionEnd = ref<{ x: number; y: number } | null>(null)
const regionTrails = ref<Trail[]>([])

const canvasRef = ref<HTMLCanvasElement | null>(null)
const containerRef = ref<HTMLDivElement | null>(null)
const snapshotImg = ref<HTMLImageElement | null>(null)
const snapshotLoaded = ref(false)
const snapshotFailed = ref(false)
const baseSnapshotUrl = computed(() =>
  selectedCameraId.value ? `/camera/previewLarge/${selectedCameraId.value}` : '',
)
const activeSnapshotUrl = ref('')
let hoverFrameCache = new Map<string, boolean>()
let snapshotTimeout: ReturnType<typeof setTimeout> | null = null

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

// Trails matched by region selection, or all filtered trails if no region
const visibleTrails = computed(() => {
  if (regionTrails.value.length > 0) return regionTrails.value
  return filteredTrails.value
})

function hexToRgb(hex: string): { r: number; g: number; b: number } {
  const n = parseInt(hex.replace('#', ''), 16)
  return { r: (n >> 16) & 0xff, g: (n >> 8) & 0xff, b: n & 0xff }
}

function getTrailColor(cls: string): string {
  return CLASS_COLORS[cls.toLowerCase()] || '#ffff44'
}

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

type Pt = { x: number; y: number }

function catmullRomToBezier(p0: Pt, p1: Pt, p2: Pt, p3: Pt, alpha = 1 / 6): [Pt, Pt] {
  return [
    { x: p1.x + alpha * (p2.x - p0.x), y: p1.y + alpha * (p2.y - p0.y) },
    { x: p2.x - alpha * (p3.x - p1.x), y: p2.y - alpha * (p3.y - p1.y) },
  ]
}

function buildSmoothPath(cp: Pt[]): Path2D {
  const path = new Path2D()
  if (cp.length < 2) return path
  path.moveTo(cp[0]!.x, cp[0]!.y)
  if (cp.length === 2) { path.lineTo(cp[1]!.x, cp[1]!.y); return path }
  for (let i = 0; i < cp.length - 1; i++) {
    const p0 = cp[Math.max(0, i - 1)]!, p1 = cp[i]!, p2 = cp[i + 1]!, p3 = cp[Math.min(cp.length - 1, i + 2)]!
    const [c1, c2] = catmullRomToBezier(p0, p1, p2, p3)
    path.bezierCurveTo(c1.x, c1.y, c2.x, c2.y, p2.x, p2.y)
  }
  return path
}

function samplePathPoints(cp: Pt[], spacingPx: number): { x: number; y: number; angle: number }[] {
  if (cp.length < 2) return []
  const samples: { x: number; y: number; angle: number }[] = []
  let accDist = spacingPx * 0.5
  for (let i = 0; i < cp.length - 1; i++) {
    const p0 = cp[Math.max(0, i - 1)]!, p1 = cp[i]!, p2 = cp[i + 1]!, p3 = cp[Math.min(cp.length - 1, i + 2)]!
    const [c1, c2] = catmullRomToBezier(p0, p1, p2, p3)
    const steps = 20
    let prevX = p1.x, prevY = p1.y
    for (let s = 1; s <= steps; s++) {
      const t = s / steps, it = 1 - t
      const x = it * it * it * p1.x + 3 * it * it * t * c1.x + 3 * it * t * t * c2.x + t * t * t * p2.x
      const y = it * it * it * p1.y + 3 * it * it * t * c1.y + 3 * it * t * t * c2.y + t * t * t * p2.y
      accDist += Math.sqrt((x - prevX) ** 2 + (y - prevY) ** 2)
      if (accDist >= spacingPx) { accDist -= spacingPx; samples.push({ x, y, angle: Math.atan2(y - prevY, x - prevX) }) }
      prevX = x; prevY = y
    }
  }
  return samples
}

// Cache converted canvas points per trail to avoid recomputation during hover/click
let canvasPointsCache = new WeakMap<Trail, Pt[]>()

function getCanvasPoints(trail: Trail, vr: ReturnType<typeof getViewRect>): Pt[] {
  const cached = canvasPointsCache.get(trail)
  if (cached) return cached
  const cp = trail.points.map(p => ({
    x: vr.offsetX + p.x * vr.drawW,
    y: vr.offsetY + p.y * vr.drawH,
  }))
  canvasPointsCache.set(trail, cp)
  return cp
}

const CHEVRON_SPACING = 50
const CHEVRON_SIZE = 5
const RIBBON_WIDTH = 6

function lightenColor(r: number, g: number, b: number, factor: number) {
  return {
    r: Math.round(r + (255 - r) * factor),
    g: Math.round(g + (255 - g) * factor),
    b: Math.round(b + (255 - b) * factor),
  }
}

/** Distance from point (px,py) to the nearest segment of a polyline */
function distToPolyline(px: number, py: number, cp: Pt[]): number {
  let minDist = Infinity
  for (let i = 0; i < cp.length - 1; i++) {
    const ax = cp[i]!.x, ay = cp[i]!.y
    const bx = cp[i + 1]!.x, by = cp[i + 1]!.y
    const dx = bx - ax, dy = by - ay
    const lenSq = dx * dx + dy * dy
    let t = lenSq === 0 ? 0 : Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / lenSq))
    const cx = ax + t * dx, cy = ay + t * dy
    const dist = Math.sqrt((px - cx) ** 2 + (py - cy) ** 2)
    if (dist < minDist) minDist = dist
  }
  return minDist
}

function drawTrails() {
  const canvas = canvasRef.value
  const img = snapshotImg.value
  if (!canvas || !img) return

  const ctx = canvas.getContext('2d')
  if (!ctx) return

  ctx.clearRect(0, 0, canvas.width, canvas.height)

  // Draw dark background if snapshot hasn't loaded
  if (!snapshotLoaded.value) {
    ctx.fillStyle = '#111'
    ctx.fillRect(0, 0, canvas.width, canvas.height)
  }

  const vr = getViewRect(canvas, img)
  const baseOpacity = settings.trailOpacity / 100
  const trailsToDraw = visibleTrails.value
  const highlightedIds = new Set(regionTrails.value.map(t => t.id))

  for (const trail of trailsToDraw) {
    const pts = trail.points
    if (pts.length < 2) continue

    const isHovered = hoveredTrail.value?.id === trail.id
    const isRegionMatch = highlightedIds.has(trail.id) && regionTrails.value.length > 0
    const color = getTrailColor(trail.cls)
    const { r, g, b } = hexToRgb(color)
    const light = lightenColor(r, g, b, 0.4)
    const opacity = isHovered ? 1 : isRegionMatch ? Math.max(baseOpacity, 0.9) : baseOpacity

    // Points are pre-simplified server-side — use directly
    const cp = getCanvasPoints(trail, vr)
    if (cp.length < 2) continue

    const path = buildSmoothPath(cp)
    const first = cp[0]!, last = cp[cp.length - 1]!

    // Solid ribbon with color → lighter gradient
    ctx.save()
    ctx.lineCap = 'round'
    ctx.lineJoin = 'round'
    ctx.lineWidth = isHovered ? RIBBON_WIDTH + 2 : RIBBON_WIDTH
    const grad = ctx.createLinearGradient(first.x, first.y, last.x, last.y)
    grad.addColorStop(0, `rgba(${r},${g},${b},${opacity * 0.6})`)
    grad.addColorStop(0.5, `rgba(${r},${g},${b},${opacity * 0.95})`)
    grad.addColorStop(1, `rgba(${light.r},${light.g},${light.b},${opacity})`)
    ctx.strokeStyle = grad
    ctx.stroke(path)
    ctx.restore()

    // Thin bright edge highlight
    ctx.save()
    ctx.lineCap = 'round'
    ctx.lineJoin = 'round'
    ctx.lineWidth = 1.5
    const edgeGrad = ctx.createLinearGradient(first.x, first.y, last.x, last.y)
    edgeGrad.addColorStop(0, `rgba(255,255,255,${opacity * 0.05})`)
    edgeGrad.addColorStop(1, `rgba(255,255,255,${opacity * 0.25})`)
    ctx.strokeStyle = edgeGrad
    ctx.stroke(path)
    ctx.restore()

    // Chevrons
    const samples = samplePathPoints(cp, CHEVRON_SPACING)
    for (let i = 0; i < samples.length; i++) {
      const s = samples[i]!
      const t = samples.length <= 1 ? 1 : i / (samples.length - 1)
      const chevAlpha = opacity * (0.3 + 0.6 * t)
      ctx.save()
      ctx.translate(s.x, s.y)
      ctx.rotate(s.angle)
      ctx.beginPath()
      ctx.moveTo(-CHEVRON_SIZE, -CHEVRON_SIZE * 0.6)
      ctx.lineTo(0, 0)
      ctx.lineTo(-CHEVRON_SIZE, CHEVRON_SIZE * 0.6)
      ctx.strokeStyle = `rgb(255,255,255)`
      ctx.globalAlpha = chevAlpha
      ctx.lineWidth = 1.5
      ctx.lineCap = 'round'
      ctx.stroke()
      ctx.restore()
    }
  }

  // Draw region selection rectangle
  if (regionStart.value && regionEnd.value) {
    const rs = regionStart.value, re = regionEnd.value
    const rx = Math.min(rs.x, re.x), ry = Math.min(rs.y, re.y)
    const rw = Math.abs(re.x - rs.x), rh = Math.abs(re.y - rs.y)
    ctx.save()
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.8)'
    ctx.lineWidth = 1.5
    ctx.setLineDash([6, 4])
    ctx.strokeRect(rx, ry, rw, rh)
    ctx.fillStyle = 'rgba(100, 150, 255, 0.1)'
    ctx.fillRect(rx, ry, rw, rh)
    ctx.restore()
  }
}

function parseCompactTrails(raw: RawTrail[]): Trail[] {
  return raw.map(r => ({
    id: r.id,
    clipId: r.clipId,
    cls: r.cls,
    name: r.name,
    points: r.pts.map(p => ({ x: p[0]!, y: p[1]!, t: p[2]! })),
  }))
}

async function fetchTrails() {
  if (!selectedCameraId.value) return

  loading.value = true
  loadingStatus.value = 'Fetching trails…'
  error.value = ''
  try {
    const { from, to } = timeRange.value
    const url = `/api/trails/${selectedCameraId.value}?from=${Math.floor(from)}&to=${Math.floor(to)}&anchor=${settings.trailAnchor}`
    const data = await api<TrailsResponse>(url)
    loadingStatus.value = `Processing ${data.trails?.length ?? 0} trails…`
    // Let the UI update before heavy parsing
    await nextTick()
    trails.value = parseCompactTrails(data.trails || [])
    canvasPointsCache = new WeakMap()
    await nextTick()
    drawTrails()
  } catch (e: unknown) {
    error.value = e instanceof Error ? e.message : 'Failed to fetch trails'
    trails.value = []
  } finally {
    loading.value = false
    loadingStatus.value = ''
  }
}

function syncCanvasSize() {
  const canvas = canvasRef.value
  const container = containerRef.value
  if (!canvas || !container) return

  const rect = container.getBoundingClientRect()
  canvas.width = rect.width
  canvas.height = rect.height
  canvasPointsCache = new WeakMap()
  drawTrails()
}

function onSnapshotLoad() {
  snapshotLoaded.value = true
  snapshotFailed.value = false
  if (snapshotTimeout) { clearTimeout(snapshotTimeout); snapshotTimeout = null }
  syncCanvasSize()
}

function onSnapshotError() {
  snapshotLoaded.value = false
  snapshotFailed.value = true
  if (snapshotTimeout) { clearTimeout(snapshotTimeout); snapshotTimeout = null }
  // Still render trails on dark background
  syncCanvasSize()
}

function startSnapshotTimeout() {
  if (snapshotTimeout) clearTimeout(snapshotTimeout)
  snapshotTimeout = setTimeout(() => {
    if (!snapshotLoaded.value && !snapshotFailed.value) {
      snapshotFailed.value = true
      syncCanvasSize()
    }
  }, 5000)
}

// Hit-test trails using distance to polyline path
function onCanvasMouseMove(e: MouseEvent) {
  if (regionMode.value && regionStart.value) {
    // Dragging region selection
    const canvas = canvasRef.value
    if (!canvas) return
    const rect = canvas.getBoundingClientRect()
    regionEnd.value = { x: e.clientX - rect.left, y: e.clientY - rect.top }
    drawTrails()
    return
  }

  const canvas = canvasRef.value
  const img = snapshotImg.value
  if (!canvas || !img) return

  const rect = canvas.getBoundingClientRect()
  const mx = e.clientX - rect.left
  const my = e.clientY - rect.top

  const vr = getViewRect(canvas, img)
  const HIT_DIST = 12

  let closest: Trail | null = null
  let closestDist = HIT_DIST

  for (const trail of visibleTrails.value) {
    if (trail.points.length < 2) continue
    const cp = getCanvasPoints(trail, vr)
    const dist = distToPolyline(mx, my, cp)
    if (dist < closestDist) {
      closestDist = dist
      closest = trail
    }
  }

  if (closest !== hoveredTrail.value) {
    hoveredTrail.value = closest
    tooltipPos.value = { x: e.clientX, y: e.clientY }
    drawTrails()

    // Update background to show detection frame near hovered trail midpoint
    if (closest && selectedCameraId.value) {
      const midPt = closest.points[Math.floor(closest.points.length / 2)]!
      const frameFile = midPt.t.toFixed(3) + '.jpg'
      const frameUrl = `/api/detection/frame/${selectedCameraId.value}/${frameFile}`
      if (hoverFrameCache.has(frameUrl)) {
        activeSnapshotUrl.value = frameUrl
      } else {
        const probe = new Image()
        probe.onload = () => {
          hoverFrameCache.set(frameUrl, true)
          if (hoveredTrail.value?.id === closest!.id) {
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
    activeSnapshotUrl.value = ''
    drawTrails()
  }
}

function onCanvasClick(_e: MouseEvent) {
  if (regionMode.value) return // region selection handles its own click

  // Navigate to clip if a trail is hovered
  if (hoveredTrail.value && selectedCameraId.value) {
    const trail = hoveredTrail.value
    const midTime = trail.points[Math.floor(trail.points.length / 2)]?.t
    router.push({
      path: `/clips/${selectedCameraId.value}`,
      query: midTime ? { t: String(Math.floor(midTime)) } : undefined,
    })
  }
}

// Region selection handlers
function onCanvasMouseDown(e: MouseEvent) {
  if (!regionMode.value) return
  const canvas = canvasRef.value
  if (!canvas) return
  const rect = canvas.getBoundingClientRect()
  regionStart.value = { x: e.clientX - rect.left, y: e.clientY - rect.top }
  regionEnd.value = null
  regionTrails.value = []
}

function onCanvasMouseUp(e: MouseEvent) {
  if (!regionMode.value || !regionStart.value) return
  const canvas = canvasRef.value
  const img = snapshotImg.value
  if (!canvas || !img) return

  const rect = canvas.getBoundingClientRect()
  regionEnd.value = { x: e.clientX - rect.left, y: e.clientY - rect.top }

  const rs = regionStart.value, re = regionEnd.value
  const rxMin = Math.min(rs.x, re.x), rxMax = Math.max(rs.x, re.x)
  const ryMin = Math.min(rs.y, re.y), ryMax = Math.max(rs.y, re.y)

  // Min drag size to count as a region
  if (rxMax - rxMin < 5 || ryMax - ryMin < 5) {
    regionStart.value = null
    regionEnd.value = null
    regionTrails.value = []
    drawTrails()
    return
  }

  const vr = getViewRect(canvas, img)
  const matched: Trail[] = []
  for (const trail of filteredTrails.value) {
    const cp = getCanvasPoints(trail, vr)
    for (const pt of cp) {
      if (pt.x >= rxMin && pt.x <= rxMax && pt.y >= ryMin && pt.y <= ryMax) {
        matched.push(trail)
        break
      }
    }
  }
  regionTrails.value = matched
  drawTrails()
}

function clearRegion() {
  regionStart.value = null
  regionEnd.value = null
  regionTrails.value = []
  drawTrails()
}

function toggleRegionMode() {
  regionMode.value = !regionMode.value
  if (!regionMode.value) clearRegion()
}

function navigateToClip(trail: Trail) {
  if (!selectedCameraId.value) return
  const midTime = trail.points[Math.floor(trail.points.length / 2)]?.t
  router.push({
    path: `/clips/${selectedCameraId.value}`,
    query: midTime ? { t: String(Math.floor(midTime)) } : undefined,
  })
}

function formatTime(ts: number): string {
  return new Date(ts * 1000).toLocaleTimeString()
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
  if (snapshotTimeout) clearTimeout(snapshotTimeout)
})

watch(selectedCameraId, () => {
  snapshotLoaded.value = false
  snapshotFailed.value = false
  activeSnapshotUrl.value = ''
  hoverFrameCache = new Map()
  trails.value = []
  startSnapshotTimeout()
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
        <button
          class="btn btn-sm"
          :class="regionMode ? 'btn-primary' : 'btn-outline-secondary'"
          @click="toggleRegionMode"
          title="Draw a region to find matching clips"
        >
          ⬚ Region
        </button>
        <button
          v-if="regionTrails.length > 0"
          class="btn btn-sm btn-outline-warning"
          @click="clearRegion"
        >
          ✕ Clear
        </button>
        <span v-if="filteredTrails.length > 0" class="text-muted-custom small">
          {{ filteredTrails.length }} trails
          <template v-if="regionTrails.length > 0">({{ regionTrails.length }} matched)</template>
        </span>
        <span v-if="loadingStatus" class="text-muted-custom small">{{ loadingStatus }}</span>
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

    <!-- No camera selected -->
    <div v-if="!selectedCameraId" class="text-muted-custom text-center py-5">
      Select a camera to view trails
    </div>

    <!-- Canvas area (always mounted to preserve DOM refs) -->
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
          :class="{ 'region-cursor': regionMode }"
          @mousemove="onCanvasMouseMove"
          @mouseleave="onCanvasMouseLeave"
          @click="onCanvasClick"
          @mousedown="onCanvasMouseDown"
          @mouseup="onCanvasMouseUp"
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

        <!-- Loading overlay (overlays the canvas, doesn't destroy it) -->
        <div v-if="loading" class="trails-loading-overlay">
          <div class="spinner-border text-primary" role="status">
            <span class="visually-hidden">Loading…</span>
          </div>
          <span v-if="loadingStatus" class="small">{{ loadingStatus }}</span>
        </div>

        <!-- Error overlay -->
        <div v-if="error && !loading" class="trails-loading-overlay">
          <div class="text-danger small">{{ error }}</div>
        </div>

        <!-- Tooltip -->
        <div
          v-if="hoveredTrail && !regionMode"
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
          <span class="text-muted-custom small ms-1">{{ formatTime(hoveredTrail.points[0]?.t ?? 0) }}</span>
          <span class="trails-tooltip-click small ms-1">click to view</span>
        </div>
      </div>

      <!-- Region matched clips panel -->
      <div v-if="regionTrails.length > 0" class="trails-region-panel">
        <div class="trails-region-header">
          <strong class="small">{{ regionTrails.length }} matched trails</strong>
          <button class="btn btn-sm btn-link text-muted-custom p-0" @click="clearRegion">✕</button>
        </div>
        <div class="trails-region-list">
          <div
            v-for="trail in regionTrails.slice(0, 50)"
            :key="trail.id"
            class="trails-region-item"
            @click="navigateToClip(trail)"
            @mouseenter="hoveredTrail = trail; drawTrails()"
            @mouseleave="hoveredTrail = null; drawTrails()"
          >
            <span
              class="trails-tooltip-dot"
              :style="{ background: getTrailColor(trail.cls) }"
            />
            <span class="small">{{ trail.name || trail.cls }}</span>
            <span class="text-muted-custom small ms-auto">{{ formatTime(trail.points[0]?.t ?? 0) }}</span>
          </div>
          <div v-if="regionTrails.length > 50" class="text-muted-custom small px-2 py-1">
            … and {{ regionTrails.length - 50 }} more
          </div>
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
  cursor: pointer;
}

.trails-canvas.region-cursor {
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

.trails-loading-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 0.5rem;
  background: rgba(0, 0, 0, 0.5);
  z-index: 10;
  pointer-events: none;
}

.trails-loading-overlay .small {
  color: var(--bs-body-color, #e1e4e8);
  opacity: 0.7;
}

.trails-tooltip-click {
  color: rgba(255, 255, 255, 0.4);
}

.trails-region-panel {
  margin-top: 0.5rem;
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.375rem;
  background: var(--bs-dark, #1e1e2e);
  max-height: 200px;
  overflow: hidden;
  display: flex;
  flex-direction: column;
}

.trails-region-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.375rem 0.5rem;
  border-bottom: 1px solid var(--bs-border-color, #333);
}

.trails-region-list {
  overflow-y: auto;
  flex: 1;
}

.trails-region-item {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.25rem 0.5rem;
  cursor: pointer;
  transition: background 0.1s;
}

.trails-region-item:hover {
  background: rgba(255, 255, 255, 0.08);
}

@media (max-width: 768px) {
  .trails-canvas-container {
    min-height: 250px;
  }
}
</style>
