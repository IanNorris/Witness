import { ref, onUnmounted, type Ref } from 'vue'
import { useEventStream } from './useEventStream'

interface DetectionBox {
  id: number
  cls: string
  conf: number
  x: number
  y: number
  w: number
  h: number
}

interface DetectionFrame {
  cameraId: number
  timestamp: number
  boxes: DetectionBox[]
}

// Interpolation state per tracked object
interface TrackedBox extends DetectionBox {
  // Current interpolated position
  ix: number
  iy: number
  iw: number
  ih: number
  // Target position
  tx: number
  ty: number
  tw: number
  th: number
  lastSeen: number
}

const CLASS_COLORS: Record<string, string> = {
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

export function useDetectionOverlay(
  cameraId: number,
  canvasRef: Ref<HTMLCanvasElement | null>,
  videoRef: Ref<HTMLVideoElement | null>,
) {
  const enabled = ref(false)
  const tracked = new Map<number, TrackedBox>()
  let animFrame = 0
  let removeListener: (() => void) | null = null

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

    // Sync canvas size to video display
    const rect = video.getBoundingClientRect()
    if (canvas.width !== rect.width || canvas.height !== rect.height) {
      canvas.width = rect.width
      canvas.height = rect.height
    }

    const ctx = canvas.getContext('2d')
    if (!ctx) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height)

    // Calculate video content area (object-fit: contain)
    const videoAspect = video.videoWidth / (video.videoHeight || 1)
    const containerAspect = rect.width / (rect.height || 1)
    let drawW: number, drawH: number, offsetX: number, offsetY: number

    if (videoAspect > containerAspect) {
      drawW = rect.width
      drawH = rect.width / videoAspect
      offsetX = 0
      offsetY = (rect.height - drawH) / 2
    } else {
      drawH = rect.height
      drawW = rect.height * videoAspect
      offsetX = (rect.width - drawW) / 2
      offsetY = 0
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

      // Calculate pixel position within video content area
      const px = offsetX + box.ix * drawW
      const py = offsetY + box.iy * drawH
      const pw = box.iw * drawW
      const ph = box.ih * drawH

      // Fade opacity
      const alpha = age < FADE_MS * 0.5 ? 1 : 1 - (age - FADE_MS * 0.5) / (FADE_MS * 0.5)
      const color = getClassColor(box.cls)

      // Draw bounding box
      ctx.strokeStyle = color
      ctx.globalAlpha = alpha
      ctx.lineWidth = 2
      ctx.strokeRect(px, py, pw, ph)

      // Draw label background
      const label = `${box.cls} ${Math.round(box.conf * 100)}%`
      ctx.font = '12px sans-serif'
      const textW = ctx.measureText(label).width
      ctx.fillStyle = color
      ctx.globalAlpha = alpha * 0.7
      ctx.fillRect(px, py - 18, textW + 8, 18)

      // Draw label text
      ctx.fillStyle = '#000'
      ctx.globalAlpha = alpha
      ctx.fillText(label, px + 4, py - 5)
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
) {
  const enabled = ref(false)
  const frames = ref<Array<{ t: number; boxes: DetectionBox[] }>>([])
  let animFrame = 0

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
    const arr = frames.value
    if (!arr.length) return null

    let lo = 0, hi = arr.length - 1
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      if ((arr[mid]?.t ?? 0) < time) lo = mid + 1
      else hi = mid
    }

    const candidate = arr[lo]
    if (!candidate) return null

    // Check which of lo-1 and lo is closer
    const prev = lo > 0 ? arr[lo - 1] : undefined
    if (prev && Math.abs(prev.t - time) < Math.abs(candidate.t - time)) {
      return Math.abs(prev.t - time) < 1.0 ? prev : null
    }

    // Only return if within 1 second of current time
    return Math.abs(candidate.t - time) < 1.0 ? candidate : null
  }

  function draw() {
    const canvas = canvasRef.value
    const video = videoRef.value
    if (!canvas || !video || !enabled.value) {
      animFrame = requestAnimationFrame(draw)
      return
    }

    const rect = video.getBoundingClientRect()
    if (canvas.width !== rect.width || canvas.height !== rect.height) {
      canvas.width = rect.width
      canvas.height = rect.height
    }

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

    // Calculate video content area
    const videoAspect = video.videoWidth / (video.videoHeight || 1)
    const containerAspect = rect.width / (rect.height || 1)
    let drawW: number, drawH: number, offsetX: number, offsetY: number

    if (videoAspect > containerAspect) {
      drawW = rect.width
      drawH = rect.width / videoAspect
      offsetX = 0
      offsetY = (rect.height - drawH) / 2
    } else {
      drawH = rect.height
      drawW = rect.height * videoAspect
      offsetX = (rect.width - drawW) / 2
      offsetY = 0
    }

    for (const box of frame.boxes) {
      const px = offsetX + box.x * drawW
      const py = offsetY + box.y * drawH
      const pw = box.w * drawW
      const ph = box.h * drawH
      const color = getClassColor(box.cls)

      ctx.strokeStyle = color
      ctx.lineWidth = 2
      ctx.strokeRect(px, py, pw, ph)

      const label = `${box.cls} ${Math.round(box.conf * 100)}%`
      ctx.font = '12px sans-serif'
      const textW = ctx.measureText(label).width
      ctx.fillStyle = color
      ctx.globalAlpha = 0.7
      ctx.fillRect(px, py - 18, textW + 8, 18)
      ctx.fillStyle = '#000'
      ctx.globalAlpha = 1
      ctx.fillText(label, px + 4, py - 5)
    }

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
