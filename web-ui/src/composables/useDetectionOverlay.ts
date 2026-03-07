import { ref, onUnmounted, type Ref, computed } from 'vue'
import { useEventStream } from './useEventStream'
import { useSettingsStore } from '../stores/settings'

interface DetectionBox {
  id: number
  cls: string
  conf: number
  x: number
  y: number
  w: number
  h: number
  baseline?: boolean
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

/**
 * Calculate the actual rendered video area inside a container using object-fit: contain.
 * Returns null if video dimensions aren't available yet.
 */
function getVideoContentRect(video: HTMLVideoElement) {
  if (!video.videoWidth || !video.videoHeight) return null

  const rect = video.getBoundingClientRect()
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

function syncCanvasSize(canvas: HTMLCanvasElement, video: HTMLVideoElement) {
  const rect = video.getBoundingClientRect()
  const w = Math.round(rect.width)
  const h = Math.round(rect.height)
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w
    canvas.height = h
  }
}

const BASELINE_COLOR = '#6688cc'

function drawBox(
  ctx: CanvasRenderingContext2D,
  box: { x: number; y: number; w: number; h: number; cls: string; conf: number; baseline?: boolean },
  vr: { drawW: number; drawH: number; offsetX: number; offsetY: number },
  alpha = 1,
) {
  const px = vr.offsetX + box.x * vr.drawW
  const py = vr.offsetY + box.y * vr.drawH
  const pw = box.w * vr.drawW
  const ph = box.h * vr.drawH
  const color = box.baseline ? BASELINE_COLOR : getClassColor(box.cls)

  ctx.strokeStyle = color
  ctx.globalAlpha = alpha
  ctx.lineWidth = box.baseline ? 1 : 2
  if (box.baseline) ctx.setLineDash([4, 4])
  ctx.strokeRect(px, py, pw, ph)
  ctx.setLineDash([])

  const label = `${box.cls} ${Math.round(box.conf * 100)}%`
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

  const FADE_MS = 500
  const LERP_SPEED = 0.3

  function handleDetectionEvent(evt: { event: string; data: Record<string, unknown> }) {
    if (evt.event !== 'detection:frame') return
    const data = evt.data as unknown as DetectionFrame
    if (data.cameraId !== cameraId) return
    if (!enabled.value) return

    const now = performance.now()
    const seen = new Set<number>()

    for (const box of data.boxes) {
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
  }

  function draw() {
    const canvas = canvasRef.value
    const video = videoRef.value
    if (!canvas || !video || !enabled.value) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    syncCanvasSize(canvas, video)

    const ctx = canvas.getContext('2d')
    if (!ctx) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height)

    const vr = getVideoContentRect(video)
    if (!vr) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    const now = performance.now()

    for (const [id, box] of tracked) {
      const age = now - box.lastSeen
      if (age > FADE_MS) {
        tracked.delete(id)
        continue
      }

      // Lerp toward target
      box.ix += (box.tx - box.ix) * LERP_SPEED
      box.iy += (box.ty - box.iy) * LERP_SPEED
      box.iw += (box.tw - box.iw) * LERP_SPEED
      box.ih += (box.th - box.ih) * LERP_SPEED

      const alpha = age < FADE_MS * 0.5 ? 1 : 1 - (age - FADE_MS * 0.5) / (FADE_MS * 0.5)
      if (box.conf >= minConf.value) {
        drawBox(ctx, { x: box.ix, y: box.iy, w: box.iw, h: box.ih, cls: box.cls, conf: box.conf }, vr, alpha)
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

  function getTimeOffset(): number {
    return typeof timeOffset === 'number' ? timeOffset : timeOffset.value
  }

  async function loadDetections(from: number, to: number) {
    try {
      const resp = await fetch(`/api/detection/${cameraId}?from=${from}&to=${to}`)
      if (!resp.ok) return
      const data = await resp.json()
      frames.value = data.frames || []
    } catch {
      frames.value = []
    }
  }

  function findNearestFrame(time: number): { t: number; boxes: DetectionBox[] } | null {
    const absoluteTime = time + getTimeOffset()
    const arr = frames.value
    if (!arr.length) return null

    let lo = 0, hi = arr.length - 1
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      if ((arr[mid]?.t ?? 0) < absoluteTime) lo = mid + 1
      else hi = mid
    }

    const candidate = arr[lo]
    if (!candidate) return null

    const prev = lo > 0 ? arr[lo - 1] : undefined
    if (prev && Math.abs(prev.t - absoluteTime) < Math.abs(candidate.t - absoluteTime)) {
      return Math.abs(prev.t - absoluteTime) < 1.0 ? prev : null
    }

    return Math.abs(candidate.t - absoluteTime) < 1.0 ? candidate : null
  }

  function draw() {
    const canvas = canvasRef.value
    const video = videoRef.value
    if (!canvas || !video || !enabled.value) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    syncCanvasSize(canvas, video)

    const ctx = canvas.getContext('2d')
    if (!ctx) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height)

    const frame = findNearestFrame(video.currentTime)
    if (!frame) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    const vr = getVideoContentRect(video)
    if (!vr) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    for (const box of frame.boxes) {
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
