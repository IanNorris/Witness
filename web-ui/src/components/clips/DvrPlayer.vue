<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
import { api } from '../../composables/useApi'
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
})

watch(() => [props.cameraId, props.from, props.to], () => {
  fetchSegments()
})

watch(() => props.startAt, (newTs) => {
  if (!newTs || segments.value.length === 0) return
  seekToTimestamp(newTs)
})

defineExpose({
  currentTimestamp,
  elapsedTime,
  totalDuration,
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
      <span v-if="compact" class="dvr-cam-label">{{ cameraName }}</span>
      <div v-if="loading" class="dvr-error"><div class="spinner-border spinner-border-sm" /></div>
      <div v-else-if="error" class="dvr-error dvr-error-compact">{{ error }}</div>
    </div>
    <div v-if="!compact" class="dvr-controls">
      <button class="dvr-btn" @click="togglePlay" :title="playing ? 'Pause' : 'Play'">
        {{ playing ? '⏸' : '▶' }}
      </button>
      <div class="dvr-progress" @click="seek">
        <div class="dvr-progress-fill" :style="{ width: `${progressPct}%` }"></div>
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
</style>
