<script setup lang="ts">
import { computed, inject, onMounted, onUnmounted, ref, watch } from 'vue'
import { format } from 'date-fns'
import { api } from '../../composables/useApi'
import { dashboardPlaybackKey } from '../../composables/useDashboardPlayback'
import { isSavingAt } from '../../utils/dashboardPlayback'
import { withTimelineBudget } from '../../utils/dashboardTimelineQueue'

const props = defineProps<{ cameraId: number; hidden?: boolean }>()
const playback = inject(dashboardPlaybackKey)
interface Range { from: number; to: number }
const ranges = ref<Range[]>([]), pending = ref<Range[]>([]), clips = ref<Range[]>([])
const now = ref(Math.floor(Date.now() / 1000))
const from = computed(() => Math.min(now.value - 300, (position.value?.requestedAt ?? now.value) - 120))
const position = computed(() => playback?.get(props.cameraId))
const menu = ref<'back' | 'forward' | null>(null)
const error = ref(''), partial = ref(false), busy = ref(false)
const hoverTime = ref<number | null>(null)
const panel = ref<HTMLElement | null>(null)
let resizeObserver: ResizeObserver | undefined
const clock = computed(() => position.value?.viewedAt ?? now.value)
let disposed = false, controller: AbortController | undefined
let timer: ReturnType<typeof setInterval> | undefined
let startup: ReturnType<typeof setTimeout> | undefined
let clockTimer: ReturnType<typeof setInterval> | undefined
const valid = (range: Range) => Number.isFinite(range.from) && Number.isFinite(range.to) && range.to > range.from
const missing = computed(() => {
  if (error.value || !ranges.value.length && busy.value) return []
  const available = [...ranges.value, ...pending.value].sort((a, b) => a.from - b.from)
  const gaps: Range[] = []
  let cursor = from.value
  for (const range of available) {
    if (range.from > cursor) gaps.push({ from: cursor, to: Math.min(now.value, range.from) })
    cursor = Math.max(cursor, range.to)
  }
  if (cursor < now.value) gaps.push({ from: cursor, to: now.value })
  return gaps.filter(range => range.to > range.from)
})
function style(range: Range) {
  const left = Math.max(from.value, range.from), right = Math.min(now.value, range.to)
  return { left: `${(left - from.value) / (now.value - from.value) * 100}%`, width: `${Math.max(0, right - left) / (now.value - from.value) * 100}%` }
}
async function refresh() {
  if (disposed || props.hidden || document.hidden || busy.value) return
  busy.value = true
  const start = Math.floor(from.value), end = now.value
  controller = new AbortController()
  const abortTimer = setTimeout(() => controller?.abort(), 12_000)
  try {
    // Staggered, sequential requests avoid a burst of twice the camera count.
    const signal = controller.signal
    const coverage = await withTimelineBudget(() => api<{ ranges: Range[]; pending?: Range[] }>(`/dvr/coverage/${props.cameraId}/${start}/${end}`, { signal, redirectOnFail: false }))
    const events = await withTimelineBudget(() => api<{ clips: Range[]; clipsTruncated?: boolean }>(`/dvr/events/${props.cameraId}/${start}/${end}`, { signal, redirectOnFail: false }))
    if (disposed || props.hidden) return
    ranges.value = (coverage.ranges ?? []).filter(valid)
    pending.value = (coverage.pending ?? []).filter(valid)
    clips.value = (events.clips ?? []).filter(valid)
    partial.value = !!events.clipsTruncated
    error.value = ''
  } catch {
    if (!disposed && !props.hidden) error.value = 'DVR availability unavailable — retrying'
  } finally { clearTimeout(abortTimer); busy.value = false }
}
function seek(timestamp: number) {
  menu.value = null
  if (!playback || !Number.isFinite(timestamp)) return
  // An open file is not missing footage: selecting it explicitly means live.
  if (timestamp >= now.value - 1 || isSavingAt(timestamp, pending.value)) playback.live(props.cameraId)
  else playback.seek(props.cameraId, timestamp)
}
function skip(direction: 'back' | 'forward', amount: number) { seek(clock.value + (direction === 'back' ? -amount : amount)) }
function preview(event: PointerEvent) {
  const bounds = (event.currentTarget as HTMLElement).getBoundingClientRect()
  hoverTime.value = from.value + Math.max(0, Math.min(1, (event.clientX - bounds.left) / bounds.width)) * (now.value - from.value)
}
const previewLabel = computed(() => {
  const time = hoverTime.value
  if (time === null) return ''
  if (error.value) return 'Availability unknown'
  if (isSavingAt(time, pending.value)) return 'Currently saving · Go live'
  const available = ranges.value.some(range => time >= range.from && time < range.to)
  return `${format(new Date(time * 1000), 'HH:mm:ss')} · ${available ? clips.value.some(range => time >= range.from && time < range.to) ? 'Clip / activity' : 'DVR footage' : 'No DVR data'}`
})
watch(menu, value => playback?.hold(props.cameraId, value !== null))
watch(() => props.hidden, hidden => { if (hidden) controller?.abort(); else refresh() })
watch(() => position.value?.requestedAt, () => { if (!busy.value) refresh() })
onMounted(() => {
  if (panel.value) {
    resizeObserver = new ResizeObserver(() => {
      panel.value?.closest<HTMLElement>('.camera-card')?.style.setProperty('--camera-playback-height', `${panel.value.offsetHeight}px`)
    })
    resizeObserver.observe(panel.value)
  }
  startup = setTimeout(refresh, (props.cameraId % 11) * 350)
  timer = setInterval(refresh, 15_000)
  clockTimer = setInterval(() => { now.value = Math.floor(Date.now() / 1000) }, 1000)
})
onUnmounted(() => { disposed = true; controller?.abort(); resizeObserver?.disconnect(); clearTimeout(startup); clearInterval(timer); clearInterval(clockTimer); playback?.hold(props.cameraId, false) })
</script>

<template>
  <section v-if="playback && position" ref="panel" class="camera-playback-bar" aria-label="Camera playback" @click.stop @dblclick.stop
    @pointerdown.stop="playback.touch(cameraId)" @keydown.stop="playback.touch(cameraId)" @keydown.esc="menu = null">
    <div class="playback-track" :class="{ unknown: !!error }" @pointermove="preview" @pointerleave="hoverTime = null">
      <span v-for="(range, i) in ranges" :key="`r${i}`" class="playback-range coverage" :style="style(range)" />
      <span v-for="(clip, i) in clips" :key="`c${i}`" class="playback-range activity" :style="style(clip)" />
      <span v-for="(range, i) in missing" :key="`g${i}`" class="playback-range missing" :style="style(range)" />
      <span v-for="(range, i) in pending" :key="`p${i}`" class="playback-range saving" :style="style(range)" />
      <input type="range" :min="Math.floor(from)" :max="now" :value="clock" step="1" :disabled="!!error || busy && !ranges.length"
        :aria-label="`Camera ${cameraId} viewing time`" :aria-valuetext="position.requestedAt === null ? 'Live' : format(new Date(clock * 1000), 'HH:mm:ss')"
        @change="seek(Number(($event.target as HTMLInputElement).value))" />
    </div>
    <div class="playback-scale"><span>{{ format(new Date(from * 1000), 'HH:mm') }}</span><span>{{ previewLabel || (position.requestedAt === null ? '' : format(new Date(clock * 1000), 'HH:mm:ss')) }}</span><span>Live</span></div>
    <div class="playback-buttons">
      <div v-for="direction in (['back', 'forward'] as const)" :key="direction" class="seek-split" :class="direction">
        <button :disabled="direction === 'forward' && position.requestedAt === null" :aria-label="`${direction} 30 seconds`" @click="skip(direction, 30)">{{ direction === 'back' ? '↶ 30s' : '30s ↷' }}</button>
        <button :aria-label="`Other ${direction} amounts`" :aria-expanded="menu === direction" @click="menu = menu === direction ? null : direction">⋯</button>
        <div v-if="menu === direction" class="seek-menu">
          <button v-for="seconds in [10, 30, 60, 120, 300]" :key="seconds" @click="skip(direction, seconds)">{{ direction === 'back' ? 'Back' : 'Forward' }} {{ seconds < 60 ? `${seconds}s` : `${seconds / 60} min` }}</button>
        </div>
      </div>
      <button class="play-button" :disabled="position.requestedAt === null" :aria-label="position.paused ? 'Play replay' : 'Pause replay'" @click="playback.pause(cameraId)">{{ position.paused ? '▶' : 'Ⅱ' }}</button>
      <button class="go-live" :aria-pressed="position.requestedAt === null" @click="playback.live(cameraId)">Go Live</button>
      <button class="save-button" aria-label="Save recent footage" title="Save recent footage" @click="playback.save(cameraId, clock)">Save</button>
    </div>
    <span v-if="error || partial" class="playback-warning" role="status">{{ error || 'Activity markers are partial' }}</span>
  </section>
</template>

<style scoped>
.camera-playback-bar { position: relative; background: var(--bs-body-bg); color: var(--bs-body-color); padding: 5px 8px; }
.playback-track { position: relative; height: 22px; background: repeating-linear-gradient(90deg, var(--bs-body-bg) 0 4px, var(--bs-secondary-color) 4px 5px, var(--bs-body-bg) 5px 10px); border-radius: 3px; }
.playback-track.unknown { background: var(--bs-tertiary-bg); }
.playback-range { position: absolute; top: 0; height: 100%; pointer-events: none; }
.coverage { background: #354b64; }
.activity { background: var(--bs-success); opacity: .85; }
.missing { background: repeating-linear-gradient(90deg, var(--bs-body-bg) 0 4px, var(--bs-secondary-color) 4px 5px, var(--bs-body-bg) 5px 10px); }
.saving { background: repeating-linear-gradient(135deg, var(--bs-secondary-color) 0 2px, var(--bs-body-bg) 2px 6px); }
.playback-track input { position: absolute; inset: 0; margin: 0; width: 100%; height: 22px; appearance: none; background: transparent; }
.playback-track input::-webkit-slider-runnable-track { height: 22px; background: transparent; }
.playback-track input::-webkit-slider-thumb { appearance: none; width: 6px; height: 26px; margin-top: -2px; border: 1px solid var(--bs-body-bg); border-radius: 3px; background: var(--bs-body-color); }
.playback-track input::-moz-range-track { background: transparent; }
.playback-track input::-moz-range-thumb { width: 6px; height: 26px; background: var(--bs-body-color); border-color: var(--bs-body-bg); }
.playback-scale { display: flex; justify-content: space-between; gap: 5px; font-size: 11px; font-variant-numeric: tabular-nums; color: var(--bs-secondary-color); min-height: 16px; }
.playback-scale span:nth-child(2) { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.playback-buttons { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 3px; }
.playback-buttons button { background: #1b2a3d; border: 1px solid #46566c; color: var(--witness-ink); font-size: 11px; padding: 3px 5px; border-radius: 4px; }
.seek-split.back { order: 1; }
.play-button { order: 2; }
.seek-split.forward { order: 3; }
.go-live { order: 4; }
.save-button { order: 5; }
.playback-buttons button:disabled { opacity: .45; }
.playback-buttons .go-live { color: #91d8b4; }
.seek-split { display: flex; position: relative; }
.seek-split > button:first-child { border-radius: 4px 0 0 4px; border-right: 0; }
.seek-split > button:nth-child(2) { border-radius: 0 4px 4px 0; }
.seek-menu { position: absolute; bottom: 100%; left: 0; z-index: 45; min-width: 125px; background: var(--bs-body-bg); border: 1px solid var(--bs-border-color); padding: 4px; border-radius: 5px; }
.seek-menu button { display: block; width: 100%; text-align: left; }
.playback-warning { display: block; font-size: 11px; color: var(--bs-warning-text-emphasis); }
@media(pointer:coarse) { .playback-buttons button { min-height: 44px; min-width: 32px; } }
</style>
