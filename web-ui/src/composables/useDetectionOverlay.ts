import { ref, onUnmounted, type Ref, computed } from 'vue'
import { useEventStream } from './useEventStream'
import { useSettingsStore } from '../stores/settings'
import { useTrailRenderer } from './useTrailRenderer'

interface DetectionBox {
  id: number
  cls: string
  conf: number
  x: number
  y: number
  w: number
  h: number
  baseline?: boolean
  name?: string
}

interface DetectionFrame {
  cameraId: number
  timestamp: number
  boxes: DetectionBox[]
}

// Interpolation state per tracked object
interface TrackedBox extends DetectionBox {
  ix: number; iy: number; iw: number; ih: number
  tx: number; ty: number; tw: number; th: number
  lastSeen: number
}

export const CLASS_COLORS: Record<string, string> = {
  person: '#00ff88',
  face: '#ff44ff',
  car: '#4488ff',
  truck: '#4488ff',
  bus: '#4488ff',
  bicycle: '#4488ff',
  motorcycle: '#4488ff',
  cat: '#ff8844',
  dog: '#ff8844',
  bird: '#ff8844',
  horse: '#ff8844',
  cow: '#ff8844',
}

function getClassColor(cls: string): string {
  return CLASS_COLORS[cls.toLowerCase()] || '#ffff44'
}

function isKnownClass(cls: string): boolean {
  return cls.toLowerCase() in CLASS_COLORS
}

function boxArea(b: { w: number; h: number }): number {
  return b.w * b.h
}

function isEncapsulated(inner: DetectionBox, outer: DetectionBox): boolean {
  return (
    inner.x >= outer.x &&
    inner.y >= outer.y &&
    inner.x + inner.w <= outer.x + outer.w &&
    inner.y + inner.h <= outer.y + outer.h
  )
}

function boxesOverlap(a: DetectionBox, b: DetectionBox): boolean {
  return !(
    a.x + a.w <= b.x ||
    b.x + b.w <= a.x ||
    a.y + a.h <= b.y ||
    b.y + b.h <= a.y
  )
}

/**
 * Filter redundant detections:
 * 1. Remove any box fully encapsulated by a larger box (except face boxes — they're inside person boxes by design)
 * 2. Remove unknown-class (yellow) boxes that overlap a larger box
 */
function filterRedundantBoxes(boxes: DetectionBox[]): DetectionBox[] {
  const sorted = [...boxes].sort((a, b) => boxArea(b) - boxArea(a)) // largest first
  const keep: DetectionBox[] = []

  for (const box of sorted) {
    let dominated = false

    // Face boxes are never filtered — they're intentionally inside person boxes
    if (box.cls.toLowerCase() === 'face') {
      keep.push(box)
      continue
    }

    for (const kept of keep) {
      // Rule 1: fully encapsulated by a larger box → remove
      if (isEncapsulated(box, kept)) {
        dominated = true
        break
      }
      // Rule 2: unknown-class box overlapping a larger box → remove
      if (!isKnownClass(box.cls) && boxesOverlap(box, kept) && boxArea(kept) > boxArea(box)) {
        dominated = true
        break
      }
    }

    if (!dominated) keep.push(box)
  }

  return keep
}

/**
 * Calculate the actual rendered video area inside a container using object-fit: contain.
 * Returns null if video dimensions aren't available yet.
 */
function getVideoContentRect(video: HTMLVideoElement) {
  if (!video.videoWidth || !video.videoHeight) return null

  // Use parent container for layout rect — video element can report stale dimensions
  const container = video.parentElement
  if (!container) return null
  const rect = container.getBoundingClientRect()
  if (rect.width <= 0 || rect.height <= 0) return null

  const videoAspect = video.videoWidth / video.videoHeight
  const containerAspect = rect.width / rect.height

  let drawW: number, drawH: number, offsetX: number, offsetY: number

  if (videoAspect > containerAspect) {
    // Video is wider than container — letterboxed top/bottom
    drawW = rect.width
    drawH = rect.width / videoAspect
    offsetX = 0
    offsetY = (rect.height - drawH) / 2
  } else {
    // Video is taller than container — pillarboxed left/right
    drawH = rect.height
    drawW = rect.height * videoAspect
    offsetX = (rect.width - drawW) / 2
    offsetY = 0
  }

  return { drawW, drawH, offsetX, offsetY, containerW: rect.width, containerH: rect.height }
}

function syncCanvasSize(canvas: HTMLCanvasElement, _video: HTMLVideoElement): boolean {
  // Use the parent container for sizing — video.getBoundingClientRect() can return
  // stale dimensions during route transitions causing the canvas to flash full-width
  const container = canvas.parentElement
  if (!container) return false
  const rect = container.getBoundingClientRect()
  const w = Math.round(rect.width)
  const h = Math.round(rect.height)
  if (w <= 0 || h <= 0) return false
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w
    canvas.height = h
    return false // dimensions just changed — skip this frame to avoid stretched content
  }
  return true
}

const BASELINE_COLOR = '#6688cc'

function drawBox(
  ctx: CanvasRenderingContext2D,
  box: { x: number; y: number; w: number; h: number; cls: string; conf: number; baseline?: boolean; name?: string },
  vr: { drawW: number; drawH: number; offsetX: number; offsetY: number },
  alpha = 1,
) {
  const px = vr.offsetX + box.x * vr.drawW
  const py = vr.offsetY + box.y * vr.drawH
  const pw = box.w * vr.drawW
  const ph = box.h * vr.drawH
  const isFace = box.cls.toLowerCase() === 'face'
  const isRecognized = isFace && !!box.name
  const color = box.baseline ? BASELINE_COLOR : isRecognized ? '#00ffff' : getClassColor(box.cls)

  ctx.strokeStyle = color
  ctx.globalAlpha = alpha
  ctx.lineWidth = box.baseline ? 1 : isFace ? 1 : 2
  if (box.baseline) ctx.setLineDash([4, 4])
  ctx.strokeRect(px, py, pw, ph)
  ctx.setLineDash([])

  const label = isRecognized
    ? `${box.name} ${Math.round(box.conf * 100)}%`
    : `${box.cls} ${Math.round(box.conf * 100)}%`
  ctx.font = '12px sans-serif'
  const textW = ctx.measureText(label).width
  ctx.fillStyle = color
  ctx.globalAlpha = alpha * 0.7
  ctx.fillRect(px, py, textW + 8, 18)
  ctx.fillStyle = '#000'
  ctx.globalAlpha = alpha
  ctx.fillText(label, px + 4, py + 13)
}

export function useDetectionOverlay(
  cameraId: number,
  canvasRef: Ref<HTMLCanvasElement | null>,
  videoRef: Ref<HTMLVideoElement | null>,
) {
  const enabled = ref(false)
  const tracked = new Map<number, TrackedBox>()
  let animFrame = 0
  let removeListener: (() => void) | null = null
  const settings = useSettingsStore()
  const minConf = computed(() => settings.detectionMinConfidence / 100)

  const LERP_SPEED = 0.3
  const STALE_TIMEOUT_MS = 3000
  let lastDetectionTime = 0
  const trailRenderer = useTrailRenderer()

  function handleDetectionEvent(evt: { event: string; data: Record<string, unknown> }) {
    if (evt.event !== 'detection:frame') return
    const data = evt.data as unknown as DetectionFrame
    if (data.cameraId !== cameraId) return
    if (!enabled.value) return

    const now = performance.now()
    lastDetectionTime = now
    const seen = new Set<number>()
    const filtered = filterRedundantBoxes(data.boxes)

    for (const box of filtered) {
      seen.add(box.id)
      const existing = tracked.get(box.id)
      if (existing) {
        // Update target position
        existing.tx = box.x
        existing.ty = box.y
        existing.tw = box.w
        existing.th = box.h
        existing.conf = box.conf
        existing.cls = box.cls
        existing.name = box.name
        existing.lastSeen = now
      } else {
        // New object — start at target position
        tracked.set(box.id, {
          ...box,
          ix: box.x, iy: box.y, iw: box.w, ih: box.h,
          tx: box.x, ty: box.y, tw: box.w, th: box.h,
          lastSeen: now,
        })
      }
    }

    // Remove boxes not present in this detection pass
    for (const [id] of tracked) {
      if (!seen.has(id)) tracked.delete(id)
    }

    trailRenderer.addDetectionFrame(filtered, data.timestamp)
  }

  function draw() {
    const canvas = canvasRef.value
    const video = videoRef.value
    if (!canvas || !video || !enabled.value) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    if (!syncCanvasSize(canvas, video)) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    const ctx = canvas.getContext('2d')
    if (!ctx) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height)

    // Clear stale boxes when detection events stop (motion ended)
    if (tracked.size > 0 && lastDetectionTime > 0 &&
        performance.now() - lastDetectionTime > STALE_TIMEOUT_MS) {
      tracked.clear()
      trailRenderer.clearTrails()
    }

    const vr = getVideoContentRect(video)
    if (!vr) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    trailRenderer.drawTrails(ctx, vr)

    for (const [, box] of tracked) {
      // Lerp toward target
      box.ix += (box.tx - box.ix) * LERP_SPEED
      box.iy += (box.ty - box.iy) * LERP_SPEED
      box.iw += (box.tw - box.iw) * LERP_SPEED
      box.ih += (box.th - box.ih) * LERP_SPEED

      if (box.conf >= minConf.value) {
        drawBox(ctx, { x: box.ix, y: box.iy, w: box.iw, h: box.ih, cls: box.cls, conf: box.conf, name: box.name }, vr)
      }
    }

    ctx.globalAlpha = 1
    animFrame = requestAnimationFrame(draw)
  }

  function start() {
    if (!removeListener) {
      const { onEvent } = useEventStream()
      removeListener = onEvent(handleDetectionEvent)
    }
    enabled.value = true
    animFrame = requestAnimationFrame(draw)
  }

  function stop() {
    enabled.value = false
    tracked.clear()
    trailRenderer.clearTrails()
    if (animFrame) {
      cancelAnimationFrame(animFrame)
      animFrame = 0
    }
    // Clear canvas
    const canvas = canvasRef.value
    if (canvas) {
      const ctx = canvas.getContext('2d')
      ctx?.clearRect(0, 0, canvas.width, canvas.height)
    }
  }

  function toggle() {
    if (enabled.value) stop()
    else start()
  }

  onUnmounted(() => {
    stop()
    if (removeListener) {
      removeListener()
      removeListener = null
    }
  })

  return { enabled, start, stop, toggle }
}

// Playback overlay: loads detection data from API and syncs to video time
export function useDetectionPlayback(
  cameraId: number,
  canvasRef: Ref<HTMLCanvasElement | null>,
  videoRef: Ref<HTMLVideoElement | null>,
  timeOffset: number | Ref<number> = 0, // epoch seconds of video start — added to video.currentTime
) {
  const enabled = ref(false)
  const frames = ref<Array<{ t: number; boxes: DetectionBox[] }>>([])
  let animFrame = 0
  const settings = useSettingsStore()
  const minConf = computed(() => settings.detectionMinConfidence / 100)
  const trailRenderer = useTrailRenderer()
  let lastPlaybackTime = -1
  let lastFrameIdx = -1

  function getTimeOffset(): number {
    return typeof timeOffset === 'number' ? timeOffset : timeOffset.value
  }

  async function loadDetections(from: number, to: number) {
    try {
      const resp = await fetch(`/api/detection/${cameraId}?from=${from}&to=${to}`)
      if (!resp.ok) return
      const data = await resp.json()
      frames.value = data.frames || []
      trailRenderer.clearTrails()
      lastPlaybackTime = -1
      lastFrameIdx = -1
    } catch {
      frames.value = []
      trailRenderer.clearTrails()
      lastPlaybackTime = -1
      lastFrameIdx = -1
    }
  }

  function findCurrentFrame(time: number): { t: number; boxes: DetectionBox[] } | null {
    const absoluteTime = time + getTimeOffset()
    const arr = frames.value
    if (!arr.length) return null

    // Binary search for the last frame at or before current time
    let lo = 0, hi = arr.length - 1
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1
      if ((arr[mid]?.t ?? 0) <= absoluteTime) lo = mid
      else hi = mid - 1
    }

    const candidate = arr[lo]
    if (!candidate || candidate.t > absoluteTime) return null

    // Find the next frame to determine how long this one should persist
    const next = lo < arr.length - 1 ? arr[lo + 1] : null
    // Show until next frame arrives, or up to 5s max if no next frame
    const maxAge = next ? next.t - candidate.t : 5.0
    if (absoluteTime - candidate.t > maxAge) return null

    return candidate
  }

  function draw() {
    const canvas = canvasRef.value
    const video = videoRef.value
    if (!canvas || !video || !enabled.value) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    if (!syncCanvasSize(canvas, video)) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    const ctx = canvas.getContext('2d')
    if (!ctx) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height)

    const frame = findCurrentFrame(video.currentTime)
    if (!frame) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    const vr = getVideoContentRect(video)
    if (!vr) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    const absoluteTime = video.currentTime + getTimeOffset()

    // If we've seeked backwards, rebuild trails from scratch
    if (absoluteTime < lastPlaybackTime - 0.5) {
      trailRenderer.clearTrails()
      lastFrameIdx = -1
    }
    lastPlaybackTime = absoluteTime

    // Add any new frames since last draw
    const arr = frames.value
    const startIdx = lastFrameIdx + 1
    for (let i = startIdx; i < arr.length; i++) {
      const f = arr[i]
      if (!f || f.t > absoluteTime) break
      trailRenderer.addDetectionFrame(filterRedundantBoxes(f.boxes), f.t)
      lastFrameIdx = i
    }

    trailRenderer.drawTrails(ctx, vr)

    const filtered = filterRedundantBoxes(frame.boxes)
    for (const box of filtered) {
      if (box.conf >= minConf.value) {
        drawBox(ctx, box, vr)
      }
    }

    ctx.globalAlpha = 1
    animFrame = requestAnimationFrame(draw)
  }

  function start() {
    enabled.value = true
    animFrame = requestAnimationFrame(draw)
  }

  function stop() {
    enabled.value = false
    if (animFrame) {
      cancelAnimationFrame(animFrame)
      animFrame = 0
    }
    trailRenderer.clearTrails()
    lastPlaybackTime = -1
    lastFrameIdx = -1
    const canvas = canvasRef.value
    if (canvas) {
      const ctx = canvas.getContext('2d')
      ctx?.clearRect(0, 0, canvas.width, canvas.height)
    }
  }

  function toggle() {
    if (enabled.value) stop()
    else start()
  }

  onUnmounted(() => stop())

  return { enabled, start, stop, toggle, loadDetections, frames }
}
