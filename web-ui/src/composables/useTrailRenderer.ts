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

function getTrailColor(cls: string): string {
  return CLASS_COLORS[cls.toLowerCase()] || '#ffff44'
}

function hexToRgb(hex: string): { r: number; g: number; b: number } {
  const n = parseInt(hex.replace('#', ''), 16)
  return { r: (n >> 16) & 0xff, g: (n >> 8) & 0xff, b: n & 0xff }
}

export function useTrailRenderer() {
  const trails: Map<number, ObjectTrail> = new Map()

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
  function toCanvas(pt: TrailPoint, vr: ViewRect): { x: number; y: number } {
    return { x: vr.offsetX + pt.x * vr.drawW, y: vr.offsetY + pt.y * vr.drawH }
  }

  /**
   * Compute cubic bezier control points from Catmull-Rom spline through p0..p3.
   * Returns [cp1, cp2] for the segment between p1 and p2.
   */
  function catmullRomToBezier(
    p0: { x: number; y: number },
    p1: { x: number; y: number },
    p2: { x: number; y: number },
    p3: { x: number; y: number },
    alpha = 1 / 6,
  ): [{ x: number; y: number }, { x: number; y: number }] {
    return [
      { x: p1.x + alpha * (p2.x - p0.x), y: p1.y + alpha * (p2.y - p0.y) },
      { x: p2.x - alpha * (p3.x - p1.x), y: p2.y - alpha * (p3.y - p1.y) },
    ]
  }

  function drawArrowHead(
    ctx: CanvasRenderingContext2D,
    tipX: number, tipY: number,
    angle: number,
    size: number,
    fillStyle: string,
  ): void {
    ctx.save()
    ctx.translate(tipX, tipY)
    ctx.rotate(angle)
    ctx.beginPath()
    ctx.moveTo(0, 0)
    ctx.lineTo(-size, -size * 0.45)
    ctx.lineTo(-size, size * 0.45)
    ctx.closePath()
    ctx.fillStyle = fillStyle
    ctx.fill()
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
    ctx.font = '10px sans-serif'
    const metrics = ctx.measureText(text)
    const padX = 4
    const padY = 2
    const tw = metrics.width
    const th = 10
    const pillW = tw + padX * 2
    const pillH = th + padY * 2
    const pillX = x - pillW / 2
    const pillY = y - pillH

    // Semi-transparent background pill
    const pillR = pillH / 2
    ctx.beginPath()
    ctx.moveTo(pillX + pillR, pillY)
    ctx.lineTo(pillX + pillW - pillR, pillY)
    ctx.arc(pillX + pillW - pillR, pillY + pillR, pillR, -Math.PI / 2, Math.PI / 2)
    ctx.lineTo(pillX + pillR, pillY + pillH)
    ctx.arc(pillX + pillR, pillY + pillR, pillR, Math.PI / 2, -Math.PI / 2)
    ctx.closePath()
    ctx.fillStyle = `rgba(${rgb.r},${rgb.g},${rgb.b},${baseOpacity * 0.55})`
    ctx.fill()

    // White text
    ctx.fillStyle = `rgba(255,255,255,${baseOpacity})`
    ctx.textAlign = 'center'
    ctx.textBaseline = 'middle'
    ctx.fillText(text, pillX + pillW / 2, pillY + pillH / 2)
    ctx.restore()
  }

  function drawTrails(ctx: CanvasRenderingContext2D, vr: ViewRect): void {
    const settings = useSettingsStore()
    if (!settings.trailEnabled) return

    const baseOpacity = settings.trailOpacity / 100

    ctx.lineCap = 'round'
    ctx.lineJoin = 'round'
    ctx.lineWidth = 2

    for (const trail of trails.values()) {
      const pts = trail.points
      if (pts.length === 0) continue

      const color = getTrailColor(trail.cls)
      const { r, g, b } = hexToRgb(color)

      // Convert all points to canvas coordinates
      const cp = pts.map(p => toCanvas(p, vr))

      // Draw smooth bezier curves (or straight lines for 2 points) with gradient opacity
      if (pts.length === 2) {
        const avgAlpha = baseOpacity * (0.2 + 0.8 * 0.5)
        ctx.beginPath()
        ctx.strokeStyle = `rgba(${r},${g},${b},${avgAlpha})`
        ctx.moveTo(cp[0]!.x, cp[0]!.y)
        ctx.lineTo(cp[1]!.x, cp[1]!.y)
        ctx.stroke()
      } else if (pts.length >= 3) {
        for (let i = 0; i < cp.length - 1; i++) {
          const t0 = i / (cp.length - 1)
          const t1 = (i + 1) / (cp.length - 1)
          const avgAlpha = baseOpacity * (0.2 + 0.8 * (t0 + t1) / 2)

          // Catmull-Rom needs 4 points; clamp at boundaries
          const p0 = cp[Math.max(0, i - 1)]!
          const p1 = cp[i]!
          const p2 = cp[i + 1]!
          const p3 = cp[Math.min(cp.length - 1, i + 2)]!

          const [c1, c2] = catmullRomToBezier(p0, p1, p2, p3)

          ctx.beginPath()
          ctx.strokeStyle = `rgba(${r},${g},${b},${avgAlpha})`
          ctx.moveTo(p1.x, p1.y)
          ctx.bezierCurveTo(c1.x, c1.y, c2.x, c2.y, p2.x, p2.y)
          ctx.stroke()
        }
      }

      // Draw dots at each point
      for (let i = 0; i < pts.length; i++) {
        const t = pts.length <= 1 ? 1 : i / (pts.length - 1)
        const alpha = baseOpacity * (0.2 + 0.8 * t)
        const isHead = i === pts.length - 1
        const radius = isHead ? 4 : 2

        ctx.beginPath()
        ctx.fillStyle = `rgba(${r},${g},${b},${alpha})`
        ctx.arc(cp[i]!.x, cp[i]!.y, radius, 0, Math.PI * 2)
        ctx.fill()
      }

      // Draw directional arrow and label at the trail head
      if (pts.length >= 2) {
        const head = cp[cp.length - 1]!
        const prev = cp[cp.length - 2]!
        const angle = Math.atan2(head.y - prev.y, head.x - prev.x)

        drawArrowHead(ctx, head.x, head.y, angle, 8, `rgba(${r},${g},${b},${baseOpacity})`)

        const labelText = trail.name || trail.cls
        const labelOffsetY = 14
        drawTrailLabel(ctx, labelText, head.x, head.y - labelOffsetY, { r, g, b }, baseOpacity)
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
