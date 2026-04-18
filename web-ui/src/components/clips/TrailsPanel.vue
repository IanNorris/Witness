<script setup lang="ts">
import { ref, computed, onMounted, watch, nextTick, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import { useSettingsStore } from '../../stores/settings'
import { useFilterStore } from '../../stores/filters'
import { api } from '../../composables/useApi'
import { CLASS_COLORS } from '../../composables/useDetectionOverlay'

interface TrailPoint {
  x: number
  y: number
  t: number
}

interface Trail {
  id: number
  clipId: number
  clipTs: number
  clipDur: number
  cls: string
  name?: string
  points: TrailPoint[]
  smooth: TrailPoint[]
}

interface RawTrail {
  id: number
  clipId: number
  clipTs: number
  clipDur: number
  cls: string
  name?: string
  pts: number[][]
}

interface TrailsResponse {
  trails: RawTrail[]
  cameraId: number
}

const props = defineProps<{
  cameraId: number | null
  visible: boolean
}>()

const emit = defineEmits<{
  regionClipIds: [ids: number[]]
  hoverClipId: [id: number | null]
}>()

const router = useRouter()
const settings = useSettingsStore()
const filterStore = useFilterStore()

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
  props.cameraId ? `/camera/previewLarge/${props.cameraId}` : '',
)
const activeSnapshotUrl = ref('')
let hoverFrameCache = new Map<string, boolean>()
let snapshotTimeout: ReturnType<typeof setTimeout> | null = null

const allClasses = Object.keys(CLASS_COLORS)

const snapshotUrl = computed(() => activeSnapshotUrl.value || baseSnapshotUrl.value)

const timeRange = computed(() => {
  const range = filterStore.timeRange
  if (range) return range
  const now = Date.now() / 1000
  return { from: now - 86400, to: now }
})

const enabledClasses = computed(() => {
  const tags = filterStore.activeFilters.tags
  if (tags.length === 0) return new Set(Object.keys(CLASS_COLORS))
  return new Set(tags.map(t => t.toLowerCase()))
})

const filteredTrails = computed(() =>
  trails.value.filter(t => enabledClasses.value.has(t.cls.toLowerCase())),
)

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

let canvasPointsCache = new WeakMap<Trail, Pt[]>()

function getCanvasPoints(trail: Trail, vr: ReturnType<typeof getViewRect>): Pt[] {
  const cached = canvasPointsCache.get(trail)
  if (cached) return cached
  const cp = trail.smooth.map(p => ({
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

function distToPolyline(px: number, py: number, cp: Pt[]): { dist: number; segIdx: number; t: number } {
  let minDist = Infinity
  let bestSeg = 0
  let bestT = 0
  for (let i = 0; i < cp.length - 1; i++) {
    const ax = cp[i]!.x, ay = cp[i]!.y
    const bx = cp[i + 1]!.x, by = cp[i + 1]!.y
    const dx = bx - ax, dy = by - ay
    const lenSq = dx * dx + dy * dy
    const t = lenSq === 0 ? 0 : Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / lenSq))
    const cx = ax + t * dx, cy = ay + t * dy
    const dist = Math.sqrt((px - cx) ** 2 + (py - cy) ** 2)
    if (dist < minDist) {
      minDist = dist
      bestSeg = i
      bestT = t
    }
  }
  return { dist: minDist, segIdx: bestSeg, t: bestT }
}

function drawTrails() {
  const canvas = canvasRef.value
  const img = snapshotImg.value
  if (!canvas || !img) return

  const ctx = canvas.getContext('2d')
  if (!ctx) return

  ctx.clearRect(0, 0, canvas.width, canvas.height)

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

function smoothTrailPoints(pts: TrailPoint[], windowRadius = 4): TrailPoint[] {
  if (pts.length <= 3) return pts
  const kernel: number[] = []
  let sum = 0
  for (let i = -windowRadius; i <= windowRadius; i++) {
    const w = Math.exp(-0.5 * (i / (windowRadius * 0.5)) ** 2)
    kernel.push(w)
    sum += w
  }
  for (let i = 0; i < kernel.length; i++) kernel[i]! /= sum

  return pts.map((p, idx) => {
    let sx = 0, sy = 0, wt = 0
    for (let k = -windowRadius; k <= windowRadius; k++) {
      const j = Math.max(0, Math.min(pts.length - 1, idx + k))
      const w = kernel[k + windowRadius]!
      sx += pts[j]!.x * w
      sy += pts[j]!.y * w
      wt += w
    }
    return { x: sx / wt, y: sy / wt, t: p.t }
  })
}

function parseCompactTrails(raw: RawTrail[]): Trail[] {
  return raw.map(r => {
    const points = r.pts.map(p => ({ x: p[0]!, y: p[1]!, t: p[2]! }))
    return {
      id: r.id,
      clipId: r.clipId,
      clipTs: r.clipTs,
      clipDur: r.clipDur,
      cls: r.cls,
      name: r.name,
      points,
      smooth: smoothTrailPoints(points),
    }
  })
}

async function fetchTrails() {
  if (!props.cameraId) return

  loading.value = true
  loadingStatus.value = 'Fetching trails…'
  error.value = ''
  try {
    const { from, to } = timeRange.value
    const url = `/api/trails/${props.cameraId}?from=${Math.floor(from)}&to=${Math.floor(to)}&anchor=${settings.trailAnchor}`
    const data = await api<TrailsResponse>(url)
    loadingStatus.value = `Processing ${data.trails?.length ?? 0} trails…`
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
  if (rect.width < 10 || rect.height < 10 || rect.width > window.innerWidth * 1.5 || rect.height > window.innerHeight * 1.5)
    return

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

let isDraggingRegion = false

function onCanvasMouseMove(e: MouseEvent) {
  if (isDraggingRegion) return

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
  let closestSegIdx = 0
  let closestSegT = 0

  for (const trail of visibleTrails.value) {
    if (trail.points.length < 2) continue
    const cp = getCanvasPoints(trail, vr)
    const hit = distToPolyline(mx, my, cp)
    if (hit.dist < closestDist) {
      closestDist = hit.dist
      closest = trail
      closestSegIdx = hit.segIdx
      closestSegT = hit.t
    }
  }

  if (closest !== hoveredTrail.value || closest) {
    hoveredTrail.value = closest
    tooltipPos.value = { x: e.clientX, y: e.clientY }
    drawTrails()

    // Emit hover event
    emit('hoverClipId', closest ? closest.clipId : null)

    // Show detection frame nearest to mouse position along the trail
    if (closest && props.cameraId) {
      const ptA = closest.points[closestSegIdx]!
      const ptB = closest.points[closestSegIdx + 1]!
      const nearestT = ptA.t + (ptB.t - ptA.t) * closestSegT
      const frameFile = nearestT.toFixed(3) + '.jpg'
      const frameUrl = `/api/detection/frame/${props.cameraId}/${frameFile}`
      if (hoverFrameCache.has(frameUrl)) {
        if (hoverFrameCache.get(frameUrl)) {
          activeSnapshotUrl.value = frameUrl
        }
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
    }
  }
}

function onCanvasMouseLeave() {
  if (hoveredTrail.value) {
    hoveredTrail.value = null
    emit('hoverClipId', null)
    drawTrails()
  }
}

function onCanvasClick(_e: MouseEvent) {
  if (regionMode.value) return

  if (hoveredTrail.value && props.cameraId) {
    const trail = hoveredTrail.value
    router.push({
      path: `/clips/${props.cameraId}`,
      query: { t: String(Math.floor(trail.clipTs)) },
    })
  }
}

function onCanvasMouseDown(e: MouseEvent) {
  if (!regionMode.value) return
  const canvas = canvasRef.value
  if (!canvas) return
  const rect = canvas.getBoundingClientRect()
  regionStart.value = { x: e.clientX - rect.left, y: e.clientY - rect.top }
  regionEnd.value = null
  regionTrails.value = []
  isDraggingRegion = true
  document.addEventListener('mousemove', onDocumentRegionMove)
  document.addEventListener('mouseup', onDocumentRegionUp)
}

function clampToCanvas(clientX: number, clientY: number): { x: number; y: number } | null {
  const canvas = canvasRef.value
  if (!canvas) return null
  const rect = canvas.getBoundingClientRect()
  return {
    x: Math.max(0, Math.min(rect.width, clientX - rect.left)),
    y: Math.max(0, Math.min(rect.height, clientY - rect.top)),
  }
}

function onDocumentRegionMove(e: MouseEvent) {
  if (!isDraggingRegion || !regionStart.value) return
  const pt = clampToCanvas(e.clientX, e.clientY)
  if (pt) {
    regionEnd.value = pt
    drawTrails()
  }
}

function onDocumentRegionUp(e: MouseEvent) {
  document.removeEventListener('mousemove', onDocumentRegionMove)
  document.removeEventListener('mouseup', onDocumentRegionUp)
  isDraggingRegion = false

  if (!regionMode.value || !regionStart.value) return
  const canvas = canvasRef.value
  const img = snapshotImg.value
  if (!canvas || !img) return

  const pt = clampToCanvas(e.clientX, e.clientY)
  if (!pt) return
  regionEnd.value = pt

  const rs = regionStart.value, re = regionEnd.value!
  const rxMin = Math.min(rs.x, re.x), rxMax = Math.max(rs.x, re.x)
  const ryMin = Math.min(rs.y, re.y), ryMax = Math.max(rs.y, re.y)

  if (rxMax - rxMin < 5 || ryMax - ryMin < 5) {
    regionStart.value = null
    regionEnd.value = null
    regionTrails.value = []
    emit('regionClipIds', [])
    drawTrails()
    return
  }

  const vr = getViewRect(canvas, img)
  const matched: Trail[] = []
  for (const trail of filteredTrails.value) {
    const cp = getCanvasPoints(trail, vr)
    for (const p of cp) {
      if (p.x >= rxMin && p.x <= rxMax && p.y >= ryMin && p.y <= ryMax) {
        matched.push(trail)
        break
      }
    }
  }
  regionTrails.value = matched

  // Emit unique clipIds from matched trails
  const clipIds = [...new Set(matched.map(t => t.clipId))]
  emit('regionClipIds', clipIds)

  drawTrails()
}

function onCanvasMouseUp(_e: MouseEvent) {
  // Region mouseup handled by document listener
}

function clearRegion() {
  regionStart.value = null
  regionEnd.value = null
  regionTrails.value = []
  emit('regionClipIds', [])
  drawTrails()
}

function toggleRegionMode() {
  regionMode.value = !regionMode.value
  if (!regionMode.value) clearRegion()
}

function formatTime(ts: number): string {
  return new Date(ts * 1000).toLocaleTimeString()
}

let resizeObserver: ResizeObserver | null = null

onMounted(() => {
  resizeObserver = new ResizeObserver(() => syncCanvasSize())
  if (containerRef.value) resizeObserver.observe(containerRef.value)
})

onUnmounted(() => {
  resizeObserver?.disconnect()
  if (snapshotTimeout) clearTimeout(snapshotTimeout)
  document.removeEventListener('mousemove', onDocumentRegionMove)
  document.removeEventListener('mouseup', onDocumentRegionUp)
})

watch(() => props.cameraId, () => {
  snapshotLoaded.value = false
  snapshotFailed.value = false
  activeSnapshotUrl.value = ''
  hoverFrameCache = new Map()
  trails.value = []
  startSnapshotTimeout()
  fetchTrails()
})

watch(timeRange, () => fetchTrails())

watch(() => props.visible, (visible) => {
  if (visible) {
    nextTick(() => syncCanvasSize())
  }
})

// Redraw when filter tags change (enabledClasses is derived from them)
watch(enabledClasses, () => drawTrails())
</script>

<template>
  <div v-if="visible && cameraId" class="trails-panel">
    <div class="trails-panel-header">
      <div class="d-flex align-items-center gap-2">
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
    </div>
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
      <div v-if="!snapshotLoaded && !snapshotFailed && cameraId" class="trails-empty-overlay">
        <div class="spinner-border spinner-border-sm text-muted-custom" role="status"></div>
      </div>

      <!-- Loading overlay -->
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

    <!-- Legend -->
    <div class="trails-legend">
      <span
        v-for="cls in allClasses"
        :key="cls"
        class="trails-legend-item"
        :class="{ 'trails-legend-disabled': !enabledClasses.has(cls) }"
      >
        <span class="trails-legend-swatch" :style="{ background: CLASS_COLORS[cls] }" />
        {{ cls }}
      </span>
    </div>
  </div>
</template>

<style scoped>
.trails-panel {
  display: flex;
  flex-direction: column;
  flex: 1;
  min-height: 0;
}

.trails-panel-header {
  margin-bottom: 0.5rem;
}

.trails-canvas-container {
  position: relative;
  flex: 1;
  min-height: 350px;
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

@media (max-width: 768px) {
  .trails-canvas-container {
    min-height: 250px;
  }
}
</style>
