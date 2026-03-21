import { useSettingsStore } from '../stores/settings'
import { CLASS_COLORS } from './useDetectionOverlay'

export interface TrailPoint {
  x: number
  y: number
  timestamp: number
}

export interface ObjectTrail {
  trackingId: number
  cls: string
  points: TrailPoint[]
  name?: string
}

interface DetectionBox {
  id: number
  cls: string
  x: number
  y: number
  w: number
  h: number
  name?: string
}

interface ViewRect {
  drawW: number
  drawH: number
  offsetX: number
  offsetY: number
}

// Discontinuity thresholds (normalized coordinates)
const MAX_SPATIAL_JUMP = 0.15
const MAX_TIME_GAP = 2.0 // seconds

// Chevron spacing in pixels along the path
const CHEVRON_SPACING_PX = 50
const CHEVRON_SIZE = 5

// Ribbon width
const RIBBON_WIDTH = 6

function getTrailColor(cls: string): string {
  return CLASS_COLORS[cls.toLowerCase()] || '#ffff44'
}

function hexToRgb(hex: string): { r: number; g: number; b: number } {
  const n = parseInt(hex.replace('#', ''), 16)
  return { r: (n >> 16) & 0xff, g: (n >> 8) & 0xff, b: n & 0xff }
}

/** Lighten an RGB color toward white by a factor (0=unchanged, 1=white) */
function lighten(r: number, g: number, b: number, factor: number): { r: number; g: number; b: number } {
  return {
    r: Math.round(r + (255 - r) * factor),
    g: Math.round(g + (255 - g) * factor),
    b: Math.round(b + (255 - b) * factor),
  }
}

type Pt = { x: number; y: number }

/**
 * Ramer-Douglas-Peucker simplification on normalized points.
 * Reduces noisy detections to a smooth path while preserving shape.
 */
function simplifyRDP(points: TrailPoint[], epsilon: number): TrailPoint[] {
  if (points.length <= 2) return points

  // Find point with max distance from line between first and last
  let maxDist = 0
  let maxIdx = 0
  const a = points[0]!
  const b = points[points.length - 1]!
  const dx = b.x - a.x
  const dy = b.y - a.y
  const lenSq = dx * dx + dy * dy

  for (let i = 1; i < points.length - 1; i++) {
    const p = points[i]!
    let dist: number
    if (lenSq === 0) {
      dist = Math.sqrt((p.x - a.x) ** 2 + (p.y - a.y) ** 2)
    } else {
      const t = Math.max(0, Math.min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq))
      const projX = a.x + t * dx
      const projY = a.y + t * dy
      dist = Math.sqrt((p.x - projX) ** 2 + (p.y - projY) ** 2)
    }
    if (dist > maxDist) {
      maxDist = dist
      maxIdx = i
    }
  }

  if (maxDist > epsilon) {
    const left = simplifyRDP(points.slice(0, maxIdx + 1), epsilon)
    const right = simplifyRDP(points.slice(maxIdx), epsilon)
    return [...left.slice(0, -1), ...right]
  }
  return [a, b]
}

/**
 * Split a trail's points into segments wherever there's a temporal gap
 * or a large spatial jump — prevents connecting unrelated detections.
 */
function splitOnDiscontinuities(points: TrailPoint[]): TrailPoint[][] {
  if (points.length < 2) return [points]
  const segments: TrailPoint[][] = []
  let current: TrailPoint[] = [points[0]!]

  for (let i = 1; i < points.length; i++) {
    const prev = points[i - 1]!
    const curr = points[i]!
    const dt = curr.timestamp - prev.timestamp
    const dist = Math.sqrt((curr.x - prev.x) ** 2 + (curr.y - prev.y) ** 2)

    if (dt > MAX_TIME_GAP || dist > MAX_SPATIAL_JUMP) {
      if (current.length >= 2) segments.push(current)
      current = []
    }
    current.push(curr)
  }
  if (current.length >= 2) segments.push(current)
  return segments
}

/**
 * Compute cubic bezier control points from Catmull-Rom spline through p0..p3.
 */
function catmullRomToBezier(
  p0: Pt, p1: Pt, p2: Pt, p3: Pt, alpha = 1 / 6,
): [Pt, Pt] {
  return [
    { x: p1.x + alpha * (p2.x - p0.x), y: p1.y + alpha * (p2.y - p0.y) },
    { x: p2.x - alpha * (p3.x - p1.x), y: p2.y - alpha * (p3.y - p1.y) },
  ]
}

/**
 * Build a full Catmull-Rom bezier path from canvas-space points.
 * Returns a Path2D that can be stroked as a smooth ribbon.
 */
function buildSmoothPath(cp: Pt[]): Path2D {
  const path = new Path2D()
  if (cp.length < 2) return path

  path.moveTo(cp[0]!.x, cp[0]!.y)

  if (cp.length === 2) {
    path.lineTo(cp[1]!.x, cp[1]!.y)
    return path
  }

  for (let i = 0; i < cp.length - 1; i++) {
    const p0 = cp[Math.max(0, i - 1)]!
    const p1 = cp[i]!
    const p2 = cp[i + 1]!
    const p3 = cp[Math.min(cp.length - 1, i + 2)]!
    const [c1, c2] = catmullRomToBezier(p0, p1, p2, p3)
    path.bezierCurveTo(c1.x, c1.y, c2.x, c2.y, p2.x, p2.y)
  }
  return path
}

/**
 * Sample points along a Catmull-Rom path at regular pixel intervals.
 * Used for placing chevrons evenly along the path.
 */
function samplePathPoints(cp: Pt[], spacingPx: number): { x: number; y: number; angle: number }[] {
  if (cp.length < 2) return []

  // Approximate the path with small line segments and accumulate distance
  const samples: { x: number; y: number; angle: number }[] = []
  let accDist = spacingPx * 0.5 // start half a spacing in

  for (let i = 0; i < cp.length - 1; i++) {
    const p0 = cp[Math.max(0, i - 1)]!
    const p1 = cp[i]!
    const p2 = cp[i + 1]!
    const p3 = cp[Math.min(cp.length - 1, i + 2)]!
    const [c1, c2] = catmullRomToBezier(p0, p1, p2, p3)

    // Subdivide bezier into small steps
    const steps = 20
    let prevX = p1.x, prevY = p1.y
    for (let s = 1; s <= steps; s++) {
      const t = s / steps
      const it = 1 - t
      const x = it * it * it * p1.x + 3 * it * it * t * c1.x + 3 * it * t * t * c2.x + t * t * t * p2.x
      const y = it * it * it * p1.y + 3 * it * it * t * c1.y + 3 * it * t * t * c2.y + t * t * t * p2.y
      const segLen = Math.sqrt((x - prevX) ** 2 + (y - prevY) ** 2)
      accDist += segLen

      if (accDist >= spacingPx) {
        accDist -= spacingPx
        const angle = Math.atan2(y - prevY, x - prevX)
        samples.push({ x, y, angle })
      }
      prevX = x
      prevY = y
    }
  }
  return samples
}

export function useTrailRenderer() {
  const trails: Map<number, ObjectTrail> = new Map()
  let nextSplitId = 1_000_000 // for client-side discontinuity splitting

  function addDetectionFrame(boxes: DetectionBox[], timestamp: number): void {
    const settings = useSettingsStore()
    const maxPoints = settings.trailMaxPoints

    for (const box of boxes) {
      let ax: number
      let ay: number

      switch (settings.trailAnchor) {
        case 'center':
          ax = box.x + box.w / 2
          ay = box.y + box.h / 2
          break
        case 'top-center':
          ax = box.x + box.w / 2
          ay = box.y
          break
        case 'bottom-center':
        default:
          ax = box.x + box.w / 2
          ay = box.y + box.h
          break
      }

      let trail = trails.get(box.id)

      // Break trail on spatial discontinuity (live mode — same tracking ID teleports)
      if (trail && trail.points.length > 0) {
        const last = trail.points[trail.points.length - 1]!
        const dist = Math.sqrt((ax - last.x) ** 2 + (ay - last.y) ** 2)
        const dt = timestamp - last.timestamp
        if (dist > MAX_SPATIAL_JUMP || dt > MAX_TIME_GAP) {
          // Freeze the old trail under a new ID and start fresh
          const frozenId = nextSplitId++
          trails.set(frozenId, { ...trail, trackingId: frozenId })
          trail = undefined
          trails.delete(box.id)
        }
      }

      if (!trail) {
        trail = { trackingId: box.id, cls: box.cls, points: [], name: box.name }
        trails.set(box.id, trail)
      }

      trail.cls = box.cls
      if (box.name !== undefined) trail.name = box.name
      trail.points.push({ x: ax, y: ay, timestamp })

      if (trail.points.length > maxPoints) {
        trail.points.splice(0, trail.points.length - maxPoints)
      }
    }
  }

  /** Convert normalized trail point to canvas pixel coordinates */
  function toCanvas(pt: TrailPoint, vr: ViewRect): Pt {
    return { x: vr.offsetX + pt.x * vr.drawW, y: vr.offsetY + pt.y * vr.drawH }
  }

  function drawChevron(
    ctx: CanvasRenderingContext2D,
    x: number, y: number, angle: number,
    size: number, color: string, opacity: number,
  ): void {
    ctx.save()
    ctx.translate(x, y)
    ctx.rotate(angle)
    ctx.beginPath()
    ctx.moveTo(-size, -size * 0.6)
    ctx.lineTo(0, 0)
    ctx.lineTo(-size, size * 0.6)
    ctx.strokeStyle = color
    ctx.globalAlpha = opacity
    ctx.lineWidth = 1.5
    ctx.lineCap = 'round'
    ctx.stroke()
    ctx.restore()
  }

  function drawTrailLabel(
    ctx: CanvasRenderingContext2D,
    text: string,
    x: number, y: number,
    rgb: { r: number; g: number; b: number },
    baseOpacity: number,
  ): void {
    ctx.save()
    ctx.font = 'bold 11px sans-serif'
    const metrics = ctx.measureText(text)
    const padX = 6
    const padY = 3
    const tw = metrics.width
    const th = 11
    const pillW = tw + padX * 2
    const pillH = th + padY * 2
    const pillX = x - pillW / 2
    const pillY = y - pillH

    const pillR = pillH / 2
    ctx.beginPath()
    ctx.moveTo(pillX + pillR, pillY)
    ctx.lineTo(pillX + pillW - pillR, pillY)
    ctx.arc(pillX + pillW - pillR, pillY + pillR, pillR, -Math.PI / 2, Math.PI / 2)
    ctx.lineTo(pillX + pillR, pillY + pillH)
    ctx.arc(pillX + pillR, pillY + pillR, pillR, Math.PI / 2, -Math.PI / 2)
    ctx.closePath()
    ctx.fillStyle = `rgba(${rgb.r},${rgb.g},${rgb.b},${baseOpacity * 0.6})`
    ctx.fill()
    ctx.strokeStyle = `rgba(255,255,255,${baseOpacity * 0.3})`
    ctx.lineWidth = 1
    ctx.stroke()

    ctx.fillStyle = `rgba(255,255,255,${baseOpacity})`
    ctx.textAlign = 'center'
    ctx.textBaseline = 'middle'
    ctx.fillText(text, pillX + pillW / 2, pillY + pillH / 2)
    ctx.restore()
  }

  /**
   * Draw a single trail segment as a solid vibrant ribbon.
   * Color blends from base → lighter toward the trail head.
   */
  function drawSegment(
    ctx: CanvasRenderingContext2D,
    points: TrailPoint[],
    vr: ViewRect,
    r: number, g: number, b: number,
    baseOpacity: number,
    isLastSegment: boolean,
    trailLabel?: string,
  ): void {
    if (points.length < 2) return

    // More aggressive simplification — trails are pre-simplified server-side too
    const simplified = simplifyRDP(points, 0.008)
    const cp = simplified.map(p => toCanvas(p, vr))
    if (cp.length < 2) return

    const path = buildSmoothPath(cp)
    const first = cp[0]!
    const last = cp[cp.length - 1]!

    // Solid ribbon with color → lighter gradient
    const light = lighten(r, g, b, 0.4)
    ctx.save()
    ctx.lineCap = 'round'
    ctx.lineJoin = 'round'
    ctx.lineWidth = RIBBON_WIDTH

    const grad = ctx.createLinearGradient(first.x, first.y, last.x, last.y)
    grad.addColorStop(0, `rgba(${r},${g},${b},${baseOpacity * 0.6})`)
    grad.addColorStop(0.5, `rgba(${r},${g},${b},${baseOpacity * 0.95})`)
    grad.addColorStop(1, `rgba(${light.r},${light.g},${light.b},${baseOpacity})`)
    ctx.strokeStyle = grad
    ctx.stroke(path)
    ctx.restore()

    // Thin bright edge highlight (inner stroke for depth)
    ctx.save()
    ctx.lineCap = 'round'
    ctx.lineJoin = 'round'
    ctx.lineWidth = 1.5
    const edgeGrad = ctx.createLinearGradient(first.x, first.y, last.x, last.y)
    edgeGrad.addColorStop(0, `rgba(255,255,255,${baseOpacity * 0.05})`)
    edgeGrad.addColorStop(1, `rgba(255,255,255,${baseOpacity * 0.25})`)
    ctx.strokeStyle = edgeGrad
    ctx.stroke(path)
    ctx.restore()

    // Chevrons along the path for direction
    const samples = samplePathPoints(cp, CHEVRON_SPACING_PX)
    for (let i = 0; i < samples.length; i++) {
      const s = samples[i]!
      const t = samples.length <= 1 ? 1 : i / (samples.length - 1)
      const chevronOpacity = baseOpacity * (0.3 + 0.6 * t)
      drawChevron(ctx, s.x, s.y, s.angle, CHEVRON_SIZE, `rgb(255,255,255)`, chevronOpacity)
    }

    // Trail head indicator
    if (isLastSegment) {
      // Filled head dot
      ctx.beginPath()
      ctx.fillStyle = `rgba(${light.r},${light.g},${light.b},${baseOpacity})`
      ctx.arc(last.x, last.y, 5, 0, Math.PI * 2)
      ctx.fill()
      ctx.beginPath()
      ctx.strokeStyle = `rgba(255,255,255,${baseOpacity * 0.8})`
      ctx.lineWidth = 1.5
      ctx.arc(last.x, last.y, 5, 0, Math.PI * 2)
      ctx.stroke()

      // Arrow at head
      const prev = cp[cp.length - 2]!
      const angle = Math.atan2(last.y - prev.y, last.x - prev.x)
      ctx.save()
      ctx.translate(last.x, last.y)
      ctx.rotate(angle)
      ctx.beginPath()
      ctx.moveTo(9, 0)
      ctx.lineTo(-2, -5)
      ctx.lineTo(-2, 5)
      ctx.closePath()
      ctx.fillStyle = `rgba(${light.r},${light.g},${light.b},${baseOpacity})`
      ctx.fill()
      ctx.restore()

      // Label
      if (trailLabel) {
        drawTrailLabel(ctx, trailLabel, last.x, last.y - 16, { r, g, b }, baseOpacity)
      }
    }
  }

  function drawTrails(ctx: CanvasRenderingContext2D, vr: ViewRect): void {
    const settings = useSettingsStore()
    if (!settings.trailEnabled) return

    const baseOpacity = settings.trailOpacity / 100

    for (const trail of trails.values()) {
      const pts = trail.points
      if (pts.length < 2) continue

      const color = getTrailColor(trail.cls)
      const { r, g, b } = hexToRgb(color)
      const label = trail.name || trail.cls

      // Split on discontinuities (handles recycled IDs and teleporting objects)
      const segments = splitOnDiscontinuities(pts)

      for (let s = 0; s < segments.length; s++) {
        const isLast = s === segments.length - 1
        drawSegment(ctx, segments[s]!, vr, r, g, b, baseOpacity, isLast, isLast ? label : undefined)
      }
    }
  }

  function clearTrails(): void {
    trails.clear()
  }

  function pruneStaleTrails(staleThresholdSec: number, currentTimestamp: number): void {
    for (const [id, trail] of trails) {
      const last = trail.points[trail.points.length - 1]
      if (!last || (currentTimestamp - last.timestamp) > staleThresholdSec) {
        trails.delete(id)
      }
    }
  }

  function getTrailsUpToTime(absoluteTime: number): Map<number, ObjectTrail> {
    const result = new Map<number, ObjectTrail>()
    for (const [id, trail] of trails) {
      const filtered = trail.points.filter(p => p.timestamp <= absoluteTime)
      if (filtered.length > 0) {
        result.set(id, {
          trackingId: trail.trackingId,
          cls: trail.cls,
          points: filtered,
          name: trail.name,
        })
      }
    }
    return result
  }

  return {
    trails,
    addDetectionFrame,
    drawTrails,
    clearTrails,
    pruneStaleTrails,
    getTrailsUpToTime,
  }
}
