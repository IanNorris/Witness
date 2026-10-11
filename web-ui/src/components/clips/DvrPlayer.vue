<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch } from 'vue'
import { api } from '../../composables/useApi'
import { useDetectionPlayback, CLASS_COLORS } from '../../composables/useDetectionOverlay'
import { useSettingsStore } from '../../stores/settings'
import { format } from 'date-fns'
import { createPlaybackGuard } from '../../composables/playbackGuard'
import { waitForDvrSeek } from '../../utils/dvrSeek'
import { usePlaybackResources } from '../../composables/usePlaybackResources'
import { readDvrStreamMetadata } from '../../utils/dvrStreamMetadata'
import type { StreamInfo } from '../../utils/streamChoice'
import { recordingPlaybackError } from '../../utils/streamChoice'

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
  hideHeader?: boolean
  pendingSave?: boolean
  externalTimeline?: boolean
  embedded?: boolean
  initialPaused?: boolean
  muted?: boolean
}>(), {
  compact: false,
})

const emit = defineEmits<{
  close: []
  time: [timestamp: number]
  gap: [timestamp: number]
  live: []
}>()

const videoA = ref<HTMLVideoElement | null>(null)
const videoB = ref<HTMLVideoElement | null>(null)
const canvasRef = ref<HTMLCanvasElement | null>(null)
const frozenFrameRef = ref<HTMLCanvasElement | null>(null)
const hasFrozenFrame = ref(false)
const activeSlot = ref<'a' | 'b'>('a')
const segments = ref<DvrSegment[]>([])
const currentSegIdx = ref(0)
const currentTime = ref(0)
const playing = ref(false)
const wantsPlayback = ref(!props.initialPaused)
const playbackRate = ref(1)
const error = ref<string | null>(null)
const mediaErrorCode = ref<number | null>(null)
watch(error, value => { if (value === null) mediaErrorCode.value = null }, { flush: 'sync' })
const displayedError = computed(() => mediaErrorCode.value === null ? error.value :
  recordingPlaybackError(mediaErrorCode.value, replayStreamInfo.value.codec))
const loading = ref(true)
const seeking = ref(false)
const busy = computed(() => loading.value || seeking.value)
let cancelSeek: (() => void) | undefined
let requestedTimestamp = props.startAt ?? props.from
let targetSegmentId: number | undefined
const diagnosticId = `${props.cameraId}_dvr_${crypto.randomUUID()}`
const diagnosticEvents: Record<string, unknown>[] = []
let lastDiagnosticError: Record<string, unknown> | undefined
let diagnosticErrors = 0
let diagnosticStalls = 0
function mediaState(video = getActive()) {
  const ranges = (value?: TimeRanges) => Array.from({ length: Math.min(value?.length ?? 0, 4) }, (_, i) => [value!.start(i), value!.end(i)])
  return { readyState: video?.readyState, networkState: video?.networkState,
    currentTime: video?.currentTime, duration: video?.duration, seeking: video?.seeking,
    paused: video?.paused, buffered: ranges(video?.buffered), seekable: ranges(video?.seekable),
    code: video?.error?.code, message: video?.error?.message?.slice(0, 512) }
}
function recordDvr(type: string, video = getActive(), extra: Record<string, unknown> = {}) {
  const event = { t: new Date().toISOString(), type, generation: loadGeneration,
    segmentId: targetSegmentId, requestedTimestamp, ...mediaState(video), ...extra }
  diagnosticEvents.push(event)
  if (diagnosticEvents.length > 100) diagnosticEvents.shift()
  if (['mediaError', 'seekTimeout', 'seekFailed', 'playRejected', 'mutedPlayRejected', 'segmentListError'].includes(type)) {
    lastDiagnosticError = event; diagnosticErrors++
    console.warn(`[DVR] Camera ${props.cameraId}: ${type}`, event)
  }
  if (type === 'waiting' || type === 'stalled') diagnosticStalls++
}
const diagnosticRoot = window as unknown as Record<string, unknown>
const diagnosticRegistry = (diagnosticRoot._witnessDiag ??= {}) as Record<string, unknown>
diagnosticRegistry[diagnosticId] = { snapshot: () => ({
  liveState: { selectedStream: 'dvr', ...replayStreamInfo.value },
  currentState: { ...mediaState(), playbackQuality: getActive()?.getVideoPlaybackQuality?.() },
  stats: { errorCount: diagnosticErrors, stallCount: diagnosticStalls },
  events: diagnosticEvents, lastError: lastDiagnosticError,
}) }
let swapPending = false
// Every asynchronous media load captures this value. A newer seek invalidates
// older canplay/timeout callbacks so they cannot swap a stale segment onscreen.
let loadGeneration = 0
let fetchGeneration = 0
let fetchController: AbortController | undefined
let metadataController: AbortController | undefined
let metadataSegmentId: number | undefined

const rates = [1, 2, 4, 8]

// Detection overlay — use a computed ref that follows the active video
const activeVideoRef = computed(() => activeSlot.value === 'a' ? videoA.value : videoB.value)
const replayStreamInfo = ref<StreamInfo>({ tier: 'dvr' })
usePlaybackResources(props.cameraId, videoA, 'dvr', () => replayStreamInfo.value, (type, data) => recordDvr(type, videoA.value, data))
// Standalone DVR prefetch also consumes a decoder; dashboard replay has no B slot.
usePlaybackResources(props.cameraId, videoB, 'dvr-prefetch', () => ({ tier: 'dvr' }))
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
  if (props.embedded) return videoA.value
  return activeSlot.value === 'a' ? videoA.value : videoB.value
}
function getBack(): HTMLVideoElement | null {
  if (props.embedded) return null
  return activeSlot.value === 'a' ? videoB.value : videoA.value
}

const currentSeg = computed(() => segments.value[currentSegIdx.value] ?? null)
const embeddedPlaybackGuard = createPlaybackGuard(props.cameraId, getActive, (type, data) => {
  recordDvr(type, getActive(), data)
  if (data.name === 'NotSupportedError') { mediaErrorCode.value = 4; error.value = 'Recording playback unsupported' }
})
function playActive() {
  if (props.embedded) void embeddedPlaybackGuard.play('dashboardReplay')
  else getActive()?.play().catch((e: Error) => recordDvr('playRejected', getActive(), { name: e.name, message: e.message }))
}

function freezePreviousFrame() {
  // Keep the original still across repeated seeks; never capture an unfinished
  // replacement. A small canvas needs no decoder and avoids retaining a 4K bitmap.
  if (!props.embedded || busy.value) return
  const video = getActive(), canvas = frozenFrameRef.value
  if (!video || !canvas || video.readyState < 2 || !video.videoWidth) return
  const scale = Math.min(1, 960 / video.videoWidth)
  canvas.width = Math.max(1, Math.round(video.videoWidth * scale))
  canvas.height = Math.max(1, Math.round(video.videoHeight * scale))
  try {
    const context = canvas.getContext('2d')
    if (!context) return
    context.drawImage(video, 0, 0, canvas.width, canvas.height)
    hasFrozenFrame.value = true
  } catch { hasFrozenFrame.value = false }
}

function clearFrozenFrame() {
  hasFrozenFrame.value = false
  if (frozenFrameRef.value) frozenFrameRef.value.width = frozenFrameRef.value.height = 0
}
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
  // The old active video can continue firing while the requested segment loads
  // in the back slot. It must not overwrite the newly selected timeline time.
  if (source !== getActive() || busy.value || swapPending || source.seeking || error.value) return
  currentTime.value = source.currentTime
  playing.value = !source.paused
  emit('time', currentTimestamp.value)

  // Wait for `ended`: switching early discards the last frames of every segment.
}

function handleEnded(source: HTMLVideoElement) {
  if (source !== getActive()) return
  if (!swapPending && currentSegIdx.value < segments.value.length - 1) {
    const current = segments.value[currentSegIdx.value]!
    const next = segments.value[currentSegIdx.value + 1]!
    if (next.from - current.to > 2) {
      wantsPlayback.value = false
      playing.value = false
      error.value = 'Recording gap — seek to resume'
      emit('gap', current.to)
    } else swapToNext()
  } else if (currentSegIdx.value >= segments.value.length - 1) {
    playing.value = false
    if (currentSeg.value) emit('gap', currentSeg.value.to)
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
  // A dashboard may have many historical tiles. Do not allocate another
  // decoder per tile merely to prefetch the next file.
  if (props.embedded) return
  const nextIdx = idx + 1
  const back = getBack()
  if (nextIdx >= segments.value.length || !back) return
  back.pause()
  back.src = `/dvr/segment/${segments.value[nextIdx]!.id}`
  back.playbackRate = playbackRate.value
  back.load()
}

function swapToNext() {
  if (!swapPending) loadSegment(currentSegIdx.value + 1)
}

async function fetchSegments(startAt = props.startAt) {
  freezePreviousFrame()
  const generation = ++fetchGeneration
  fetchController?.abort()
  const controller = new AbortController()
  fetchController = controller
  const fetchTimeout = setTimeout(() => controller.abort(), 10000)
  cancelSeek?.()
  seeking.value = false
  ++loadGeneration
  swapPending = false
  getActive()?.pause()
  getBack()?.pause()
  loading.value = true
  error.value = null
  try {
    const data = await api<{ segments: DvrSegment[] }>(
      `/dvr/segments/${props.cameraId}/${props.from}/${props.to}`, { signal: controller.signal, redirectOnFail: false }
    )
    if (generation !== fetchGeneration) return
    segments.value = data.segments ?? []
    if (segments.value.length === 0) {
      error.value = 'No DVR segments found for this range'
      return
    }

    let startIdx = 0
    let seekOffset = 0
    if (startAt !== undefined) {
      let found = false
      for (let i = 0; i < segments.value.length; i++) {
        const seg = segments.value[i]!
        if (startAt >= seg.from && startAt < seg.to) {
          startIdx = i
          seekOffset = startAt - seg.from
          found = true
          break
        }
      }
      if (!found) { error.value = 'No recording at the selected time'; return }
    }

    loadSegment(startIdx, seekOffset)
  } catch (e) {
    if (generation === fetchGeneration) {
      recordDvr('segmentListError', getActive(), { message: e instanceof Error ? e.message : 'Segment list request failed' })
      error.value = 'Failed to load DVR segments'
    }
  } finally {
    clearTimeout(fetchTimeout)
    if (fetchController === controller) fetchController = undefined
    if (generation === fetchGeneration) loading.value = false
  }
}

function prepareSeek(video: HTMLVideoElement, idx: number, offset: number, swap: boolean) {
  freezePreviousFrame()
  cancelSeek?.()
  const generation = ++loadGeneration
  const seg = segments.value[idx]!
  targetSegmentId = seg.id
  requestedTimestamp = seg.from + offset
  seeking.value = true
  error.value = null
  swapPending = swap
  getActive()?.pause()
  video.pause()
  recordDvr('seekRequested', video)
  cancelSeek = waitForDvrSeek(video, offset, () => {
    if (generation !== loadGeneration) return
    currentSegIdx.value = idx
    currentTime.value = video.currentTime
    if (swap) activeSlot.value = activeSlot.value === 'a' ? 'b' : 'a'
    swapPending = false
    seeking.value = false
    clearFrozenFrame()
    recordDvr('seekReady', video)
    emit('time', currentTimestamp.value)
    if (wantsPlayback.value) playActive()
    preloadNext(idx)
  }, reason => {
    if (generation !== loadGeneration) return
    seeking.value = false
    swapPending = false
    recordDvr(reason, video)
    mediaErrorCode.value = reason === 'mediaError' ? video.error?.code ?? 0 : null
    error.value = reason === 'seekTimeout' ? 'Recording took too long to load' : 'Could not load or seek this recording'
  })
}

function loadSegment(idx: number, seekTo = 0) {
  if (idx < 0 || idx >= segments.value.length) return
  const active = getActive()
  const swap = !props.embedded && !!active?.src && active.readyState > 0
  const video = swap ? getBack() : active
  if (!video) return
  prepareSeek(video, idx, seekTo, swap)
  const segmentId = segments.value[idx]!.id
  metadataController?.abort()
  const controller = new AbortController()
  metadataController = controller
  metadataSegmentId = segmentId
  replayStreamInfo.value = { tier: 'dvr' }
  const metadataTimeout = setTimeout(() => controller.abort(), 5000)
  void readDvrStreamMetadata(segmentId, controller.signal).then(info => {
    if (metadataController === controller && !controller.signal.aborted && info) replayStreamInfo.value = info
  }).catch(() => { /* Missing metadata must never block playback or guess a codec. */ })
    .finally(() => clearTimeout(metadataTimeout))
  if (props.embedded) {
    // Release the old resource rather than keeping a paused HEVC decoder alive
    // while another video element opens the replacement segment.
    video.removeAttribute('src')
    video.load()
  }
  video.src = `/dvr/segment/${segments.value[idx]!.id}`
  video.playbackRate = playbackRate.value
  video.load()
}

function seekActive(offset: number) {
  const active = getActive()
  if (!active) return
  // A failed initial resource needs reloading, not another currentTime assignment.
  if (active.error || active.readyState === 0) loadSegment(currentSegIdx.value, offset)
  else prepareSeek(active, currentSegIdx.value, offset, false)
}

function retrySeek() {
  recordDvr('retry')
  void fetchSegments(requestedTimestamp)
}

function togglePlay() {
  const vid = getActive()
  if (!vid) return
  setPlaying(vid.paused)
}

function setPlaying(value: boolean) {
  wantsPlayback.value = value
  const vid = getActive()
  if (!vid) return
  if (value && !busy.value && !error.value) playActive()
  else vid.pause()
}

function setRate(rate: number) {
  playbackRate.value = rate
  const active = getActive()
  if (active) active.playbackRate = rate
  const back = getBack()
  if (back) back.playbackRate = rate
}

function seekToTimestamp(ts: number) {
  requestedTimestamp = ts
  for (let i = 0; i < segments.value.length; i++) {
    const seg = segments.value[i]!
    if (ts >= seg.from && ts < seg.to) {
      error.value = null
      const offset = Math.max(0, Math.min(ts - seg.from, seg.duration - 0.1))
      // In single-video mode src changes before the new frame is ready, while
      // currentSegIdx still describes the last presented frame. A rapid seek
      // back must reload that resource, not seek within the pending replacement.
      if (i !== currentSegIdx.value || !getActive()?.src.endsWith(`/dvr/segment/${seg.id}`)) {
        loadSegment(i, offset)
      } else {
        seekActive(offset)
      }
      return
    }
  }
  ++loadGeneration
  cancelSeek?.()
  seeking.value = false
  swapPending = false
  getActive()?.pause()
  error.value = 'No recording at the selected time'
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
        seekActive(offset)
      }
      break
    }
    accumulated += seg.duration
  }
}

function attachHandlers(el: HTMLVideoElement) {
  for (const type of ['loadedmetadata', 'loadeddata', 'canplay', 'seeking', 'seeked', 'waiting', 'stalled', 'error']) {
    el.addEventListener(type, () => {
      if (type === 'loadedmetadata' && metadataSegmentId === targetSegmentId) replayStreamInfo.value = { ...replayStreamInfo.value, tier: 'dvr', width: el.videoWidth || undefined, height: el.videoHeight || undefined }
      recordDvr(type === 'error' ? 'mediaError' : type, el, { slot: el === videoA.value ? 'a' : 'b' })
      if (type === 'error' && el === getActive() && !busy.value) {
        mediaErrorCode.value = el.error?.code ?? 0
        error.value = 'Recording playback failed'
      }
    })
  }
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
    seekActive(Math.max(0, newTime))
  }
}

onMounted(() => {
  window.addEventListener('keydown', onKeyDown)
})

onUnmounted(() => {
  cancelSeek?.()
  clearFrozenFrame()
  fetchController?.abort()
  metadataController?.abort()
  recordDvr('replayClosed')
  // Preserve the last causal snapshot when navigating to Health, without
  // retaining video elements, decoders or this component's reactive state.
  const finalSnapshot = (diagnosticRegistry[diagnosticId] as { snapshot: () => Record<string, unknown> }).snapshot()
  finalSnapshot.liveState = { selectedStream: 'dvr-ended' }
  const retired = { retiredDvr: true, snapshot: () => finalSnapshot }
  diagnosticRegistry[diagnosticId] = retired
  const retiredIds = Object.keys(diagnosticRegistry).filter(id => (diagnosticRegistry[id] as { retiredDvr?: boolean })?.retiredDvr)
  retiredIds.slice(0, Math.max(0, retiredIds.length - 8)).forEach(id => delete diagnosticRegistry[id])
  setTimeout(() => { if (diagnosticRegistry[diagnosticId] === retired) delete diagnosticRegistry[diagnosticId] }, 10 * 60 * 1000)
  ++fetchGeneration
  ++loadGeneration
  swapPending = false
  for (const video of [videoA.value, videoB.value]) {
    if (!video) continue
    video.pause()
    video.removeAttribute('src')
    video.load()
  }
  window.removeEventListener('keydown', onKeyDown)
})

defineExpose({
  loading: busy,
  streamInfo: replayStreamInfo,
  isSwapping: () => swapPending,
  refreshSegments: fetchSegments,
  currentTimestamp,
  elapsedTime,
  totalDuration,
  nudgeDetection,
  toggleDetectionWithSave,
  overlayEnabled,
  seekToTimestamp,
  togglePlay,
  setPlaying,
  setRate,
  playing,
  error,
})
</script>

<template>
  <div class="dvr-player" :class="{ compact, embedded }">
    <div v-if="!compact && !hideHeader" class="dvr-header">
      <span class="dvr-title">📹 {{ cameraName }} — DVR Playback</span>
      <button class="dvr-close" @click="emit('close')" title="Close">✕</button>
    </div>
    <div class="dvr-video-wrap">
      <video ref="videoA" class="dvr-video" :class="{ active: activeSlot === 'a', 'seeking-hidden': embedded && busy }" :muted="muted" playsinline />
      <video v-if="!embedded" ref="videoB" class="dvr-video" :class="{ active: activeSlot === 'b' }" :muted="muted" playsinline />
      <canvas v-if="embedded" ref="frozenFrameRef" class="dvr-frozen-frame" v-show="busy && hasFrozenFrame" aria-hidden="true" />
      <canvas ref="canvasRef" class="detection-overlay" v-show="overlayEnabled && !busy" />
      <span v-if="compact && !embedded" class="dvr-cam-label">{{ cameraName }}</span>
      <div v-if="busy" class="dvr-seeking" role="status" aria-live="polite"><div class="spinner-border spinner-border-sm" /><span>Seeking recording…</span></div>
      <div v-else-if="error" class="dvr-error dvr-error-compact" role="alert">
        <span>{{ pendingSave ? 'Currently saving — try again when the file finishes.' : displayedError }}</span>
        <div class="dvr-error-actions"><button type="button" class="btn btn-sm btn-outline-light" @click="retrySeek">Retry</button><button v-if="embedded" type="button" class="btn btn-sm btn-outline-light" @click="emit('live')">Go Live</button></div>
      </div>
    </div>
    <div v-if="!compact" class="dvr-controls">
      <button class="dvr-btn" @click="togglePlay" :title="playing ? 'Pause' : 'Play'">
        {{ playing ? '⏸' : '▶' }}
      </button>
      <div v-if="!externalTimeline" class="dvr-progress" @click="seek" @contextmenu.prevent="pinScrub" @mousemove="onScrubMove" @mouseleave="onScrubLeave">
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
      <span class="dvr-time">{{ formattedTimestamp }}<template v-if="!externalTimeline"> · {{ formattedElapsed }} / {{ formattedTotal }}</template></span>
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
.dvr-player.embedded, .embedded .dvr-video-wrap { height: 100%; width: 100%; }
.embedded .dvr-video { height: 100%; max-height: none; }
.embedded .dvr-error { background: #000; }

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
  flex-direction: column;
  gap: 0.6rem;
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #f87171;
  font-size: 0.9rem;
  background: rgba(0,0,0,0.7);
}
.dvr-video.active.seeking-hidden { opacity: 0; }
.dvr-frozen-frame { position: absolute; inset: 0; width: 100%; height: 100%; object-fit: contain; filter: blur(8px); opacity: 0.65; pointer-events: none; z-index: 2; }
.dvr-seeking { position: absolute; inset: 0; z-index: 3; display: flex; align-items: center; justify-content: center; gap: 0.6rem; background: rgba(0,0,0,0.5); color: #e2e8f0; font-size: 0.8rem; }
.dvr-error-actions { display: flex; gap: 0.5rem; }

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
