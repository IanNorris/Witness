<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch } from 'vue'
import { api } from '../../composables/useApi'
import { useDetectionPlayback, CLASS_COLORS } from '../../composables/useDetectionOverlay'
import { useSettingsStore } from '../../stores/settings'
import { format } from 'date-fns'

interface DvrSegment {
  id: number
  from: number
  to: number
  duration: number
}

const props = withDefaults(defineProps<{
  cameraId: number
  from: number
  to: number
  startAt?: number
  cameraName: string
  compact?: boolean
}>(), {
  compact: false,
})

const emit = defineEmits<{
  close: []
}>()

const videoA = ref<HTMLVideoElement | null>(null)
const videoB = ref<HTMLVideoElement | null>(null)
const canvasRef = ref<HTMLCanvasElement | null>(null)
const activeSlot = ref<'a' | 'b'>('a')
const segments = ref<DvrSegment[]>([])
const currentSegIdx = ref(0)
const currentTime = ref(0)
const playing = ref(false)
const playbackRate = ref(1)
const error = ref<string | null>(null)
const loading = ref(true)
let swapPending = false

const rates = [1, 2, 4, 8]

// Detection overlay — use a computed ref that follows the active video
const activeVideoRef = computed(() => activeSlot.value === 'a' ? videoA.value : videoB.value)
const detectionVideoRef = ref<HTMLVideoElement | null>(null)

// Keep detectionVideoRef in sync with active slot
watch(activeVideoRef, (v) => { detectionVideoRef.value = v }, { immediate: true })

// Reactive time offset: current segment's start timestamp
const segmentTimeOffset = computed(() => currentSeg.value?.from ?? props.from)

const { enabled: overlayEnabled, toggle: toggleOverlay, loadDetections, frames: detectionFrames } = useDetectionPlayback(
  props.cameraId,
  canvasRef,
  detectionVideoRef,
  segmentTimeOffset,
)

function toggleDetectionWithSave() {
  toggleOverlay()
  localStorage.setItem(`witness-detection-overlay-${props.cameraId}`, overlayEnabled.value ? '1' : '0')
}
async function loadDvrDetections() {
  await loadDetections(props.from, props.to)
}

// Nudge: find next/prev detection frame relative to currentTimestamp
function nudgeDetection(direction: 'next' | 'prev') {
  const arr = detectionFrames.value
  if (!arr.length) return
  const now = currentTimestamp.value

  if (direction === 'next') {
    const frame = arr.find(f => f.t > now + 0.5)
    if (frame) seekToTimestamp(frame.t)
  } else {
    // Find last frame before current time
    let frame = null
    for (let i = arr.length - 1; i >= 0; i--) {
      if ((arr[i]?.t ?? 0) < now - 0.5) {
        frame = arr[i]!
        break
      }
    }
    if (frame) seekToTimestamp(frame.t)
  }
}

// Seek bar scrub preview
const scrubThumbUrl = ref<string | null>(null)
const scrubThumbX = ref(0)
const scrubThumbY = ref(0)
const scrubThumbTime = ref<string | null>(null)
let scrubTimer: ReturnType<typeof setTimeout> | null = null

// Pinned scrub thumbnail for detection nudge
const scrubPinned = ref(false)
const scrubPinnedTs = ref(0)
const scrubThumbCanvasRef = ref<HTMLCanvasElement | null>(null)

function findDetectionFrameAt(ts: number): { t: number; boxes: { cls: string; conf: number; x: number; y: number; w: number; h: number; baseline?: boolean }[] } | null {
  const arr = detectionFrames.value
  if (!arr.length) return null
  let lo = 0, hi = arr.length - 1
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if ((arr[mid]?.t ?? 0) < ts) lo = mid + 1
    else hi = mid
  }
  const candidate = arr[lo]
  if (!candidate) return null
  const prev = lo > 0 ? arr[lo - 1] : undefined
  if (prev && Math.abs(prev.t - ts) < Math.abs(candidate.t - ts)) {
    return Math.abs(prev.t - ts) < 2.0 ? prev : null
  }
  return Math.abs(candidate.t - ts) < 2.0 ? candidate : null
}

function drawScrubDetections() {
  const canvas = scrubThumbCanvasRef.value
  if (!canvas) return

  // Size canvas to match the image
  const img = canvas.previousElementSibling?.previousElementSibling as HTMLImageElement | null
  if (img && img.naturalWidth > 0) {
    canvas.width = img.clientWidth || img.naturalWidth
    canvas.height = img.clientHeight || img.naturalHeight
  }

  const ctx = canvas.getContext('2d')
  if (!ctx) return
  ctx.clearRect(0, 0, canvas.width, canvas.height)

  const ts = scrubPinned.value ? scrubPinnedTs.value : 0
  if (!ts) return

  const frame = findDetectionFrameAt(ts)
  if (!frame) return

  const settings = useSettingsStore()
  const minConf = settings.detectionMinConfidence / 100

  for (const box of frame.boxes) {
    if (box.conf < minConf) continue
    const px = box.x * canvas.width
    const py = box.y * canvas.height
    const pw = box.w * canvas.width
    const ph = box.h * canvas.height
    const color = box.baseline ? '#6688cc' : (CLASS_COLORS[box.cls.toLowerCase()] || '#ffff44')

    ctx.strokeStyle = color
    ctx.lineWidth = 1.5
    if (box.baseline) ctx.setLineDash([3, 3])
    ctx.strokeRect(px, py, pw, ph)
    ctx.setLineDash([])

    ctx.font = '10px sans-serif'
    const label = `${box.cls} ${Math.round(box.conf * 100)}%`
    const textW = ctx.measureText(label).width
    ctx.fillStyle = color
    ctx.globalAlpha = 0.7
    ctx.fillRect(px, py, textW + 6, 14)
    ctx.fillStyle = '#000'
    ctx.globalAlpha = 1
    ctx.fillText(label, px + 3, py + 11)
  }
}

function updateScrubThumb(ts: number) {
  scrubPinnedTs.value = ts
  scrubThumbTime.value = format(new Date(ts * 1000), 'HH:mm:ss')
  scrubThumbUrl.value = `/dvr/thumbnail/${props.cameraId}/${Math.floor(ts)}`
}

function pinScrub(event: MouseEvent) {
  if (totalDuration.value <= 0) return
  const bar = event.currentTarget as HTMLElement
  const rect = bar.getBoundingClientRect()
  const pct = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width))
  scrubThumbX.value = event.clientX
  scrubThumbY.value = rect.top

  const targetTime = pct * totalDuration.value
  let accumulated = 0
  let ts = props.from
  for (const seg of segments.value) {
    if (accumulated + seg.duration >= targetTime) {
      ts = seg.from + (targetTime - accumulated)
      break
    }
    accumulated += seg.duration
  }

  scrubPinned.value = true
  updateScrubThumb(ts)
}

function nudgeScrubDetection(direction: 'next' | 'prev') {
  if (!scrubPinned.value) return
  const arr = detectionFrames.value
  if (!arr.length) return
  const now = scrubPinnedTs.value

  let target: number | null = null
  if (direction === 'next') {
    const frame = arr.find(f => f.t > now + 0.5)
    if (frame) target = frame.t
  } else {
    for (let i = arr.length - 1; i >= 0; i--) {
      if ((arr[i]?.t ?? 0) < now - 0.5) {
        target = arr[i]!.t
        break
      }
    }
  }

  if (target !== null) {
    updateScrubThumb(target)
  }
}

function unpinScrub() {
  scrubPinned.value = false
  scrubThumbUrl.value = null
  scrubThumbTime.value = null
}

function getActive(): HTMLVideoElement | null {
  return activeSlot.value === 'a' ? videoA.value : videoB.value
}
function getBack(): HTMLVideoElement | null {
  return activeSlot.value === 'a' ? videoB.value : videoA.value
}

const currentSeg = computed(() => segments.value[currentSegIdx.value] ?? null)
const totalDuration = computed(() => segments.value.reduce((sum, s) => sum + s.duration, 0))

const elapsedTime = computed(() => {
  let elapsed = 0
  for (let i = 0; i < currentSegIdx.value && i < segments.value.length; i++) {
    elapsed += segments.value[i]!.duration
  }
  return elapsed + currentTime.value
})

const formattedElapsed = computed(() => formatSec(elapsedTime.value))
const formattedTotal = computed(() => formatSec(totalDuration.value))

const currentTimestamp = computed(() => {
  if (!currentSeg.value) return props.from
  return currentSeg.value.from + currentTime.value
})

const formattedTimestamp = computed(() => {
  return format(new Date(currentTimestamp.value * 1000), 'HH:mm:ss')
})

const progressPct = computed(() => {
  if (totalDuration.value <= 0) return 0
  return Math.min(100, (elapsedTime.value / totalDuration.value) * 100)
})

function formatSec(s: number): string {
  const sec = Math.max(0, Math.round(s))
  const m = Math.floor(sec / 60)
  const ss = sec % 60
  return `${m}:${ss.toString().padStart(2, '0')}`
}

// Event handlers — only respond if the firing element is the active one
function handleTimeUpdate(source: HTMLVideoElement) {
  if (source !== getActive()) return
  currentTime.value = source.currentTime
  playing.value = !source.paused

  const dur = source.duration
  if (!dur || !isFinite(dur)) return
  const remaining = dur - source.currentTime
  // Scale threshold with playback rate so we don't miss the window at high speeds
  const threshold = Math.max(0.5, playbackRate.value * 0.4)
  if (remaining < threshold && remaining > 0 && !swapPending && currentSegIdx.value < segments.value.length - 1) {
    swapToNext()
  }
}

function handleEnded(source: HTMLVideoElement) {
  if (source !== getActive()) return
  if (!swapPending && currentSegIdx.value < segments.value.length - 1) {
    swapToNext()
  } else if (currentSegIdx.value >= segments.value.length - 1) {
    playing.value = false
  }
}

function handlePlay(source: HTMLVideoElement) {
  if (source === getActive()) playing.value = true
}

function handlePause(source: HTMLVideoElement) {
  if (source === getActive()) playing.value = false
}

// Preload the next segment into the back video
function preloadNext(idx: number) {
  const nextIdx = idx + 1
  const back = getBack()
  if (nextIdx >= segments.value.length || !back) return
  back.src = `/dvr/segment/${segments.value[nextIdx]!.id}`
  back.playbackRate = playbackRate.value
  back.load()
}

function swapToNext() {
  if (swapPending) return
  swapPending = true
  const nextIdx = currentSegIdx.value + 1
  if (nextIdx >= segments.value.length) {
    playing.value = false
    swapPending = false
    return
  }
  const back = getBack()
  if (!back) { swapPending = false; return }

  const expectedSrc = `/dvr/segment/${segments.value[nextIdx]!.id}`

  const doSwap = () => {
    currentSegIdx.value = nextIdx
    back.playbackRate = playbackRate.value
    back.play().catch(() => {})
    activeSlot.value = activeSlot.value === 'a' ? 'b' : 'a'
    // Pause old video (now the back)
    const oldActive = getBack()
    if (oldActive) oldActive.pause()
    swapPending = false
    preloadNext(nextIdx)
  }

  // If back doesn't have the right segment loaded, load it now
  if (!back.src.endsWith(expectedSrc)) {
    back.src = expectedSrc
    back.load()
  }

  if (back.readyState >= 3) {
    doSwap()
  } else {
    const handler = () => {
      back.removeEventListener('canplay', handler)
      doSwap()
    }
    back.addEventListener('canplay', handler)
    // Safety: force swap after 2s if canplay never fires
    setTimeout(() => {
      back.removeEventListener('canplay', handler)
      if (swapPending) doSwap()
    }, 2000)
  }
}

async function fetchSegments() {
  loading.value = true
  error.value = null
  try {
    const data = await api<{ segments: DvrSegment[] }>(
      `/dvr/segments/${props.cameraId}/${props.from}/${props.to}`
    )
    segments.value = data.segments ?? []
    if (segments.value.length === 0) {
      error.value = 'No DVR segments found for this range'
      return
    }

    let startIdx = 0
    let seekOffset = 0
    if (props.startAt && props.startAt > props.from) {
      for (let i = 0; i < segments.value.length; i++) {
        const seg = segments.value[i]!
        if (props.startAt >= seg.from && props.startAt <= seg.to) {
          startIdx = i
          seekOffset = props.startAt - seg.from
          break
        }
        if (props.startAt < seg.from) {
          startIdx = i
          break
        }
      }
    }

    currentSegIdx.value = startIdx
    loadSegment(startIdx, seekOffset)
  } catch {
    error.value = 'Failed to load DVR segments'
  } finally {
    loading.value = false
  }
}

function loadSegment(idx: number, seekTo = 0) {
  if (idx < 0 || idx >= segments.value.length) return
  const seg = segments.value[idx]!
  currentSegIdx.value = idx
  swapPending = false

  const active = getActive()
  const isInitialLoad = !active || !active.src || active.readyState === 0

  if (isInitialLoad) {
    if (!active) return
    active.src = `/dvr/segment/${seg.id}`
    active.playbackRate = playbackRate.value
    if (seekTo > 0) {
      const onLoaded = () => {
        active.currentTime = seekTo
        active.removeEventListener('loadeddata', onLoaded)
      }
      active.addEventListener('loadeddata', onLoaded)
    }
    active.load()
    active.play().catch(() => {})
  } else {
    // Load into back video, swap when ready to avoid flash
    const back = getBack()
    if (!back) return
    back.src = `/dvr/segment/${seg.id}`
    back.playbackRate = playbackRate.value
    const onReady = () => {
      back.removeEventListener('canplay', onReady)
      if (seekTo > 0) back.currentTime = seekTo
      back.play().catch(() => {})
      activeSlot.value = activeSlot.value === 'a' ? 'b' : 'a'
      // Pause old active (now back)
      const oldActive = getBack()
      if (oldActive) oldActive.pause()
    }
    back.addEventListener('canplay', onReady)
    back.load()
  }

  preloadNext(idx)
}

function togglePlay() {
  const vid = getActive()
  if (!vid) return
  if (vid.paused) {
    vid.play().catch(() => {})
  } else {
    vid.pause()
  }
}

function setRate(rate: number) {
  playbackRate.value = rate
  const active = getActive()
  if (active) active.playbackRate = rate
}

function seekToTimestamp(ts: number) {
  for (let i = 0; i < segments.value.length; i++) {
    const seg = segments.value[i]!
    if ((ts >= seg.from && ts <= seg.to) || i === segments.value.length - 1) {
      const offset = Math.max(0, ts - seg.from)
      if (i !== currentSegIdx.value) {
        loadSegment(i, offset)
      } else {
        const vid = getActive()
        if (vid) vid.currentTime = offset
      }
      return
    }
  }
}

function seek(event: MouseEvent) {
  if (totalDuration.value <= 0) return
  const bar = event.currentTarget as HTMLElement
  const rect = bar.getBoundingClientRect()
  const pct = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width))
  const targetTime = pct * totalDuration.value

  let accumulated = 0
  for (let i = 0; i < segments.value.length; i++) {
    const seg = segments.value[i]!
    if (accumulated + seg.duration >= targetTime || i === segments.value.length - 1) {
      // Clamp offset to avoid seeking past actual video duration
      const offset = Math.max(0, Math.min(targetTime - accumulated, seg.duration - 0.1))
      if (i !== currentSegIdx.value) {
        loadSegment(i, offset)
      } else {
        const vid = getActive()
        if (vid) vid.currentTime = offset
      }
      break
    }
    accumulated += seg.duration
  }
}

function attachHandlers(el: HTMLVideoElement) {
  el.addEventListener('timeupdate', () => handleTimeUpdate(el))
  el.addEventListener('ended', () => handleEnded(el))
  el.addEventListener('play', () => handlePlay(el))
  el.addEventListener('pause', () => handlePause(el))
}

onMounted(() => {
  if (videoA.value) attachHandlers(videoA.value)
  if (videoB.value) attachHandlers(videoB.value)
  fetchSegments()

  // Auto-enable detection overlay based on per-camera preference
  const saved = localStorage.getItem(`witness-detection-overlay-${props.cameraId}`)
  if (saved === null || saved === '1') {
    toggleOverlay()
  }
  loadDvrDetections()
})

watch(() => [props.cameraId, props.from, props.to], () => {
  fetchSegments()
  loadDvrDetections()
})

watch(() => props.startAt, (newTs) => {
  if (!newTs || segments.value.length === 0) return
  seekToTimestamp(newTs)
})

// Seek bar scrub preview
function onScrubMove(event: MouseEvent) {
  if (totalDuration.value <= 0 || scrubPinned.value) return
  const bar = event.currentTarget as HTMLElement
  const rect = bar.getBoundingClientRect()
  const pct = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width))
  scrubThumbX.value = event.clientX
  scrubThumbY.value = rect.top

  // Calculate timestamp at scrub position
  const targetTime = pct * totalDuration.value
  let accumulated = 0
  let ts = props.from
  for (const seg of segments.value) {
    if (accumulated + seg.duration >= targetTime) {
      ts = seg.from + (targetTime - accumulated)
      break
    }
    accumulated += seg.duration
  }
  scrubThumbTime.value = format(new Date(ts * 1000), 'HH:mm:ss')

  if (scrubTimer) clearTimeout(scrubTimer)
  scrubTimer = setTimeout(() => {
    scrubThumbUrl.value = `/dvr/thumbnail/${props.cameraId}/${Math.floor(ts)}`
  }, 100)
}

function onScrubLeave() {
  if (scrubPinned.value) return
  if (scrubTimer) { clearTimeout(scrubTimer); scrubTimer = null }
  scrubThumbUrl.value = null
  scrubThumbTime.value = null
}

// Download current segment
function downloadSegment() {
  const seg = currentSeg.value
  if (!seg) return
  const a = document.createElement('a')
  a.href = `/dvr/segment/${seg.id}`
  a.download = `dvr_cam${props.cameraId}_${seg.from}.mp4`
  a.click()
}

// Keyboard shortcuts
function onKeyDown(e: KeyboardEvent) {
  if (props.compact) return
  // Ignore if user is typing in an input
  if ((e.target as HTMLElement)?.tagName === 'INPUT' || (e.target as HTMLElement)?.tagName === 'TEXTAREA') return

  switch (e.key) {
    case ' ':
      e.preventDefault()
      togglePlay()
      break
    case 'ArrowLeft':
      e.preventDefault()
      if (scrubPinned.value) {
        nudgeScrubDetection('prev')
      } else {
        seekRelative(-5)
      }
      break
    case 'ArrowRight':
      e.preventDefault()
      if (scrubPinned.value) {
        nudgeScrubDetection('next')
      } else {
        seekRelative(5)
      }
      break
    case 'Escape':
      if (scrubPinned.value) {
        e.preventDefault()
        unpinScrub()
      }
      break
    case '1': setRate(1); break
    case '2': setRate(2); break
    case '4': setRate(4); break
    case '8': setRate(8); break
    case 'd':
      toggleDetectionWithSave()
      break
    case 'n':
      e.preventDefault()
      nudgeDetection('next')
      break
    case 'p':
      e.preventDefault()
      nudgeDetection('prev')
      break
  }
}

function seekRelative(delta: number) {
  const vid = getActive()
  if (!vid) return
  const newTime = vid.currentTime + delta
  if (newTime < 0 && currentSegIdx.value > 0) {
    // Seek into previous segment
    const prevSeg = segments.value[currentSegIdx.value - 1]!
    loadSegment(currentSegIdx.value - 1, Math.max(0, prevSeg.duration + newTime))
  } else if (newTime >= (currentSeg.value?.duration ?? 0) && currentSegIdx.value < segments.value.length - 1) {
    const overflow = newTime - (currentSeg.value?.duration ?? 0)
    loadSegment(currentSegIdx.value + 1, overflow)
  } else {
    vid.currentTime = Math.max(0, newTime)
  }
}

onMounted(() => {
  window.addEventListener('keydown', onKeyDown)
})

onUnmounted(() => {
  window.removeEventListener('keydown', onKeyDown)
})

defineExpose({
  currentTimestamp,
  elapsedTime,
  totalDuration,
  nudgeDetection,
  toggleDetectionWithSave,
  overlayEnabled,
})
</script>

<template>
  <div class="dvr-player" :class="{ compact }">
    <div v-if="!compact" class="dvr-header">
      <span class="dvr-title">📹 {{ cameraName }} — DVR Playback</span>
      <button class="dvr-close" @click="emit('close')" title="Close">✕</button>
    </div>
    <div class="dvr-video-wrap">
      <video ref="videoA" class="dvr-video" :class="{ active: activeSlot === 'a' }" />
      <video ref="videoB" class="dvr-video" :class="{ active: activeSlot === 'b' }" />
      <canvas ref="canvasRef" class="detection-overlay" v-show="overlayEnabled" />
      <span v-if="compact" class="dvr-cam-label">{{ cameraName }}</span>
      <div v-if="loading" class="dvr-error"><div class="spinner-border spinner-border-sm" /></div>
      <div v-else-if="error" class="dvr-error dvr-error-compact">{{ error }}</div>
    </div>
    <div v-if="!compact" class="dvr-controls">
      <button class="dvr-btn" @click="togglePlay" :title="playing ? 'Pause' : 'Play'">
        {{ playing ? '⏸' : '▶' }}
      </button>
      <div class="dvr-progress" @click="seek" @contextmenu.prevent="pinScrub" @mousemove="onScrubMove" @mouseleave="onScrubLeave">
        <div class="dvr-progress-fill" :style="{ width: `${progressPct}%` }"></div>
        <!-- Scrub thumbnail preview -->
        <div v-if="scrubThumbTime" class="dvr-scrub-tooltip" :class="{ pinned: scrubPinned }" :style="{ left: `${scrubThumbX}px`, top: `${scrubThumbY}px` }">
          <div class="dvr-scrub-thumb-wrap">
            <img v-if="scrubThumbUrl" :src="scrubThumbUrl" class="dvr-scrub-img" alt=""
              @load="drawScrubDetections"
              @error="($event.target as HTMLImageElement).style.display='none';
                (($event.target as HTMLElement).nextElementSibling as HTMLElement).style.display='flex'" />
            <div class="dvr-scrub-offline" style="display:none">Offline</div>
            <canvas ref="scrubThumbCanvasRef" class="dvr-scrub-canvas" />
          </div>
          <div class="dvr-scrub-time">
            {{ scrubThumbTime }}
            <span v-if="scrubPinned" class="dvr-scrub-hint">← → nudge · Esc close</span>
          </div>
        </div>
      </div>
      <span class="dvr-time">{{ formattedTimestamp }} · {{ formattedElapsed }} / {{ formattedTotal }}</span>
      <span v-if="segments.length > 1" class="dvr-seg-info">seg {{ currentSegIdx + 1 }}/{{ segments.length }}</span>
      <div class="dvr-rates">
        <button
          v-for="r in rates"
          :key="r"
          class="dvr-rate-btn"
          :class="{ active: playbackRate === r }"
          @click="setRate(r)"
        >{{ r }}x</button>
      </div>
      <button class="dvr-btn dvr-download" @click="downloadSegment" title="Download current segment">⬇</button>
      <button
        class="dvr-btn"
        :class="overlayEnabled ? 'dvr-btn-active' : ''"
        @click="toggleDetectionWithSave"
        title="Toggle detection overlay (D)"
      >🔲</button>
      <button class="dvr-btn" @click="nudgeDetection('prev')" title="Previous detection (P)">⏮</button>
      <button class="dvr-btn" @click="nudgeDetection('next')" title="Next detection (N)">⏭</button>
    </div>
  </div>
</template>

<style scoped>
.dvr-player {
  margin-top: 0.5rem;
  border: 1px solid var(--bs-border-color, #444);
  border-radius: 0.5rem;
  background: rgba(0,0,0,0.6);
  overflow: hidden;
}
.dvr-player.compact {
  margin-top: 0;
  border-radius: 0;
  border: none;
}

.dvr-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.4rem 0.75rem;
  background: rgba(255,255,255,0.05);
  border-bottom: 1px solid var(--bs-border-color, #333);
}

.dvr-title {
  font-size: 0.85rem;
  font-weight: 500;
  color: rgba(255,255,255,0.85);
}

.dvr-close {
  background: none;
  border: none;
  color: rgba(255,255,255,0.5);
  font-size: 1rem;
  cursor: pointer;
  padding: 0 0.25rem;
}
.dvr-close:hover {
  color: rgba(255,255,255,0.9);
}

.dvr-video-wrap {
  position: relative;
  background: #000;
}

.dvr-video {
  display: block;
  width: 100%;
  max-height: 400px;
  object-fit: contain;
  position: absolute;
  top: 0;
  left: 0;
  opacity: 0;
}
.dvr-video.active {
  position: relative;
  opacity: 1;
}

.detection-overlay {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
  z-index: 1;
}

.dvr-btn-active {
  color: #22c55e !important;
}

.dvr-cam-label {
  position: absolute;
  top: 4px;
  left: 6px;
  font-size: 0.65rem;
  color: rgba(255,255,255,0.8);
  background: rgba(0,0,0,0.5);
  padding: 1px 5px;
  border-radius: 3px;
  z-index: 2;
  pointer-events: none;
}

.dvr-error-compact {
  font-size: 0.7rem;
}

.dvr-error {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #f87171;
  font-size: 0.9rem;
  background: rgba(0,0,0,0.7);
}

.dvr-controls {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.4rem 0.75rem;
}

.dvr-btn {
  background: none;
  border: none;
  color: rgba(255,255,255,0.8);
  font-size: 1.1rem;
  cursor: pointer;
  padding: 0;
  width: 28px;
  text-align: center;
}
.dvr-btn:hover {
  color: #fff;
}

.dvr-progress {
  flex: 1;
  height: 6px;
  background: rgba(255,255,255,0.15);
  border-radius: 3px;
  cursor: pointer;
  position: relative;
}

.dvr-progress-fill {
  height: 100%;
  background: #3b82f6;
  border-radius: 3px;
  transition: width 0.1s linear;
}

.dvr-time {
  font-size: 0.75rem;
  color: rgba(255,255,255,0.5);
  white-space: nowrap;
}

.dvr-seg-info {
  font-size: 0.65rem;
  color: rgba(255,255,255,0.3);
  white-space: nowrap;
}

.dvr-rates {
  display: flex;
  gap: 2px;
}

.dvr-rate-btn {
  background: rgba(255,255,255,0.08);
  border: 1px solid rgba(255,255,255,0.15);
  color: rgba(255,255,255,0.5);
  font-size: 0.65rem;
  padding: 1px 5px;
  border-radius: 3px;
  cursor: pointer;
}
.dvr-rate-btn.active {
  background: rgba(59, 130, 246, 0.3);
  border-color: #3b82f6;
  color: #93c5fd;
}
.dvr-rate-btn:hover {
  color: rgba(255,255,255,0.8);
}

/* Download button */
.dvr-download {
  margin-left: 0.3rem;
  font-size: 0.75rem;
}

/* Scrub thumbnail preview */
.dvr-scrub-tooltip {
  position: fixed;
  transform: translate(-50%, -100%);
  margin-top: -8px;
  z-index: 1001;
  background: rgba(0, 0, 0, 0.92);
  border: 1px solid #555;
  border-radius: 6px;
  padding: 4px;
  pointer-events: none;
  text-align: center;
}
.dvr-scrub-tooltip.pinned {
  pointer-events: auto;
  border-color: #4488ff;
  box-shadow: 0 0 8px rgba(68, 136, 255, 0.4);
}
.dvr-scrub-thumb-wrap {
  position: relative;
}
.dvr-scrub-img {
  display: block;
  width: 200px;
  height: auto;
  border-radius: 3px;
}
.dvr-scrub-canvas {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
}
.dvr-scrub-offline {
  width: 200px;
  height: 112px;
  border-radius: 3px;
  background: #1a1a2e;
  display: flex;
  align-items: center;
  justify-content: center;
  color: rgba(255, 255, 255, 0.4);
  font-size: 0.75rem;
  font-weight: 500;
  letter-spacing: 0.05em;
}
.dvr-scrub-time {
  font-size: 0.7rem;
  color: rgba(255, 255, 255, 0.85);
  margin-top: 3px;
}
.dvr-scrub-hint {
  font-size: 0.6rem;
  color: rgba(255, 255, 255, 0.5);
  margin-left: 0.3rem;
}
</style>
