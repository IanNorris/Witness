<script setup lang="ts">
import { ref, computed, onMounted, watch, onUnmounted } from 'vue'
import { api } from '../../composables/useApi'
import { useTagStore } from '../../stores/tags'
import { format, startOfDay, addDays, subDays, isToday as isTodayFn } from 'date-fns'

interface TimelineClip {
  clipUID: number
  timestamp: number
  duration: number
  cameraID: number
  cameraName: string
  tags: string
  recordMode: number
  lighting: number
  saved: number
  reviewed: number
}

interface CalendarDay {
  date: string
  count: number
}

const emit = defineEmits<{
  play: [clip: { uid: number; camera: number; timestamp: number; duration: number; tags: string }]
  rangeSelect: [from: number, to: number]
  rangeClear: []
}>()

const tagStore = useTagStore()

// Current date
const currentDate = ref(startOfDay(new Date()))
const clips = ref<TimelineClip[]>([])
const loading = ref(false)

// Calendar
const calendarDays = ref<CalendarDay[]>([])
const showCalendar = ref(false)
const calendarMonth = ref(new Date().getMonth() + 1)
const calendarYear = ref(new Date().getFullYear())

// Mouse interaction
const timelineEl = ref<HTMLElement | null>(null)
const scrubTime = ref<string | null>(null)
const scrubX = ref(0)
const isDragging = ref(false)
const dragStartX = ref(0)
const dragEndX = ref(0)
const selectionFrom = ref<number | null>(null)
const selectionTo = ref<number | null>(null)

// Tooltip
const tooltipClip = ref<TimelineClip | null>(null)
const tooltipX = ref(0)
const tooltipY = ref(0)

const HOURS = Array.from({ length: 24 }, (_, i) => i)
const SECONDS_PER_DAY = 86400

// Camera color from ID
function cameraColor(id: number): string {
  const hue = (id * 137) % 360
  return `hsl(${hue}, 65%, 55%)`
}

const dateLabel = computed(() => {
  if (isTodayFn(currentDate.value)) return 'Today'
  return format(currentDate.value, 'EEE, dd MMM yyyy')
})

const isToday = computed(() => isTodayFn(currentDate.value))

function prevDay() {
  currentDate.value = subDays(currentDate.value, 1)
}

function nextDay() {
  if (!isToday.value) {
    currentDate.value = addDays(currentDate.value, 1)
  }
}

function goToday() {
  currentDate.value = startOfDay(new Date())
}

async function fetchTimeline() {
  loading.value = true
  clearSelection()
  try {
    const d = currentDate.value
    const data = await api<{ clips: TimelineClip[] }>(
      `/clip/timeline/${d.getFullYear()}/${d.getMonth() + 1}/${d.getDate()}`
    )
    clips.value = data.clips ?? []
  } catch {
    clips.value = []
  } finally {
    loading.value = false
  }
}

async function fetchCalendar() {
  try {
    const data = await api<{ days: CalendarDay[] }>(
      `/clip/calendar/${calendarYear.value}/${calendarMonth.value}`
    )
    calendarDays.value = data.days ?? []
  } catch {
    calendarDays.value = []
  }
}

function toggleCalendar() {
  showCalendar.value = !showCalendar.value
  if (showCalendar.value) {
    calendarYear.value = currentDate.value.getFullYear()
    calendarMonth.value = currentDate.value.getMonth() + 1
    fetchCalendar()
  }
}

function calendarPrevMonth() {
  calendarMonth.value--
  if (calendarMonth.value < 1) {
    calendarMonth.value = 12
    calendarYear.value--
  }
  fetchCalendar()
}

function calendarNextMonth() {
  calendarMonth.value++
  if (calendarMonth.value > 12) {
    calendarMonth.value = 1
    calendarYear.value++
  }
  fetchCalendar()
}

function selectCalendarDay(dateStr: string) {
  currentDate.value = new Date(dateStr + 'T00:00:00')
  showCalendar.value = false
}

// Calendar grid
const calendarGrid = computed(() => {
  const firstDay = new Date(calendarYear.value, calendarMonth.value - 1, 1)
  const daysInMonth = new Date(calendarYear.value, calendarMonth.value, 0).getDate()
  const startDow = firstDay.getDay() // 0=Sun
  const countMap = new Map<string, number>()
  for (const d of calendarDays.value) countMap.set(d.date, d.count)
  const maxCount = Math.max(1, ...calendarDays.value.map(d => d.count))

  const cells: { day: number; date: string; count: number; intensity: number; empty: boolean }[] = []
  // Leading blanks
  for (let i = 0; i < startDow; i++) cells.push({ day: 0, date: '', count: 0, intensity: 0, empty: true })
  for (let d = 1; d <= daysInMonth; d++) {
    const dateStr = `${calendarYear.value}-${String(calendarMonth.value).padStart(2, '0')}-${String(d).padStart(2, '0')}`
    const count = countMap.get(dateStr) ?? 0
    cells.push({ day: d, date: dateStr, count, intensity: count / maxCount, empty: false })
  }
  return cells
})

const calendarMonthLabel = computed(() =>
  format(new Date(calendarYear.value, calendarMonth.value - 1), 'MMMM yyyy')
)

// Timeline position helpers
function timeToPercent(timestamp: number): number {
  const dayStart = currentDate.value.getTime() / 1000
  const offset = timestamp - dayStart
  return Math.max(0, Math.min(100, (offset / SECONDS_PER_DAY) * 100))
}

function durationToPercent(duration: number): number {
  return Math.max(0.3, (duration / SECONDS_PER_DAY) * 100)
}

function xToSeconds(x: number): number {
  if (!timelineEl.value) return 0
  const rect = timelineEl.value.getBoundingClientRect()
  const pct = Math.max(0, Math.min(1, (x - rect.left) / rect.width))
  return pct * SECONDS_PER_DAY
}

function secondsToTime(s: number): string {
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`
}

// Mouse handlers
function onMouseMove(e: MouseEvent) {
  if (!timelineEl.value) return
  const rect = timelineEl.value.getBoundingClientRect()
  scrubX.value = e.clientX - rect.left
  const secs = xToSeconds(e.clientX)
  scrubTime.value = secondsToTime(secs)

  if (isDragging.value) {
    dragEndX.value = e.clientX
  }
}

function onMouseDown(e: MouseEvent) {
  if (e.button !== 0) return
  isDragging.value = true
  dragStartX.value = e.clientX
  dragEndX.value = e.clientX
  selectionFrom.value = null
  selectionTo.value = null
  emit('rangeClear')
}

function onMouseUp(_e: MouseEvent) {
  if (!isDragging.value) return
  isDragging.value = false

  const startSecs = xToSeconds(Math.min(dragStartX.value, dragEndX.value))
  const endSecs = xToSeconds(Math.max(dragStartX.value, dragEndX.value))

  // Only register if dragged more than ~5 min
  if (endSecs - startSecs > 300) {
    const dayStart = currentDate.value.getTime() / 1000
    selectionFrom.value = dayStart + startSecs
    selectionTo.value = dayStart + endSecs
    emit('rangeSelect', selectionFrom.value, selectionTo.value)
  }
}

function onMouseLeave() {
  scrubTime.value = null
  if (isDragging.value) {
    isDragging.value = false
  }
}

function clearSelection() {
  selectionFrom.value = null
  selectionTo.value = null
  emit('rangeClear')
}

// Selection overlay position
const selectionStyle = computed(() => {
  if (!selectionFrom.value || !selectionTo.value) return null
  const left = timeToPercent(selectionFrom.value)
  const right = timeToPercent(selectionTo.value)
  return { left: `${left}%`, width: `${right - left}%` }
})

const dragStyle = computed(() => {
  if (!isDragging.value || !timelineEl.value) return null
  const rect = timelineEl.value.getBoundingClientRect()
  const left = Math.min(dragStartX.value, dragEndX.value) - rect.left
  const width = Math.abs(dragEndX.value - dragStartX.value)
  return { left: `${left}px`, width: `${width}px` }
})

// Clip tooltip
function getClipEmojis(clip: TimelineClip): string {
  if (!clip.tags) return ''
  const tags = clip.tags.split(/[;,]/).map(t => t.trim()).filter(Boolean)
  const emojis: string[] = []
  for (const t of tags) {
    const info = tagStore.getTagDisplay(t)
    if (info.icon) emojis.push(info.icon)
  }
  return emojis.join('')
}

function onClipHover(clip: TimelineClip, e: MouseEvent) {
  tooltipClip.value = clip
  tooltipX.value = e.clientX
  tooltipY.value = e.clientY
}

function onClipLeave() {
  tooltipClip.value = null
}

function onClipClick(clip: TimelineClip) {
  emit('play', {
    uid: clip.clipUID,
    camera: clip.cameraID,
    timestamp: clip.timestamp,
    duration: clip.duration,
    tags: clip.tags,
  })
}

// Keyboard: left/right for day nav
function onKeyDown(e: KeyboardEvent) {
  if (e.key === 'ArrowLeft') prevDay()
  else if (e.key === 'ArrowRight') nextDay()
}

watch(currentDate, fetchTimeline)

onMounted(() => {
  if (tagStore.tags.length === 0) tagStore.fetchTags()
  fetchTimeline()
  window.addEventListener('keydown', onKeyDown)
})

onUnmounted(() => {
  window.removeEventListener('keydown', onKeyDown)
})
</script>

<template>
  <div class="timeline-wrapper">
    <!-- Day navigation -->
    <div class="timeline-nav">
      <button class="timeline-nav-btn" @click="prevDay" title="Previous day">←</button>
      <button class="timeline-date-label" @click="toggleCalendar">{{ dateLabel }}</button>
      <button class="timeline-nav-btn" @click="nextDay" :disabled="isToday" title="Next day">→</button>
      <button v-if="!isToday" class="timeline-nav-btn timeline-today-btn" @click="goToday">Today</button>
      <button
        v-if="selectionFrom"
        class="timeline-nav-btn timeline-clear-btn"
        @click="clearSelection"
      >✕ Clear range</button>
      <span v-if="selectionFrom && selectionTo" class="timeline-range-label">
        {{ secondsToTime((selectionFrom - currentDate.getTime() / 1000)) }}
        — {{ secondsToTime((selectionTo - currentDate.getTime() / 1000)) }}
      </span>
    </div>

    <!-- Calendar popup -->
    <div v-if="showCalendar" class="timeline-calendar">
      <div class="cal-header">
        <button class="cal-nav" @click="calendarPrevMonth">←</button>
        <span class="cal-month">{{ calendarMonthLabel }}</span>
        <button class="cal-nav" @click="calendarNextMonth">→</button>
      </div>
      <div class="cal-grid">
        <div v-for="dow in ['S','M','T','W','T','F','S']" :key="dow" class="cal-dow">{{ dow }}</div>
        <div
          v-for="(cell, i) in calendarGrid"
          :key="i"
          class="cal-cell"
          :class="{ 'cal-empty': cell.empty, 'cal-has-events': cell.count > 0 }"
          :style="cell.count > 0 ? { backgroundColor: `rgba(37, 99, 235, ${0.2 + cell.intensity * 0.8})` } : {}"
          @click="cell.date && selectCalendarDay(cell.date)"
          :title="cell.count ? `${cell.count} clips` : ''"
        >
          {{ cell.empty ? '' : cell.day }}
        </div>
      </div>
    </div>

    <!-- Timeline bar -->
    <div
      ref="timelineEl"
      class="timeline-bar"
      @mousemove="onMouseMove"
      @mousedown="onMouseDown"
      @mouseup="onMouseUp"
      @mouseleave="onMouseLeave"
    >
      <!-- Hour markers -->
      <div class="timeline-hours">
        <div
          v-for="h in HOURS"
          :key="h"
          class="timeline-hour"
          :style="{ left: `${(h / 24) * 100}%` }"
        >
          <div class="hour-tick"></div>
          <span v-if="h % 3 === 0" class="hour-label">{{ String(h).padStart(2, '0') }}</span>
        </div>
      </div>

      <!-- Clip markers -->
      <div class="timeline-clips">
        <div
          v-for="clip in clips"
          :key="clip.clipUID"
          class="timeline-marker"
          :style="{
            left: `${timeToPercent(clip.timestamp)}%`,
            width: `${durationToPercent(clip.duration)}%`,
            backgroundColor: cameraColor(clip.cameraID),
          }"
          :class="{ 'has-emoji': !!getClipEmojis(clip) }"
          @mouseenter="onClipHover(clip, $event)"
          @mouseleave="onClipLeave"
          @click.stop="onClipClick(clip)"
        >
          <span v-if="getClipEmojis(clip)" class="marker-emoji">{{ getClipEmojis(clip) }}</span>
        </div>
      </div>

      <!-- Selection overlay -->
      <div v-if="selectionStyle" class="timeline-selection" :style="selectionStyle"></div>

      <!-- Drag preview -->
      <div v-if="dragStyle && isDragging" class="timeline-drag" :style="dragStyle"></div>

      <!-- Scrub cursor -->
      <div v-if="scrubTime && !isDragging" class="timeline-scrub" :style="{ left: `${scrubX}px` }">
        <span class="scrub-label">{{ scrubTime }}</span>
      </div>

      <!-- Loading -->
      <div v-if="loading" class="timeline-loading">
        <div class="spinner-border spinner-border-sm" />
      </div>
    </div>

    <!-- Tooltip -->
    <div
      v-if="tooltipClip"
      class="timeline-tooltip"
      :style="{ left: `${tooltipX + 10}px`, top: `${tooltipY - 60}px` }"
    >
      <div class="tt-camera" :style="{ color: cameraColor(tooltipClip.cameraID) }">
        {{ tooltipClip.cameraName }}
      </div>
      <div class="tt-time">
        {{ format(new Date(tooltipClip.timestamp * 1000), 'HH:mm:ss') }}
        · {{ tooltipClip.duration }}s
      </div>
      <div v-if="tooltipClip.tags" class="tt-tags">{{ tooltipClip.tags }}</div>
    </div>
  </div>
</template>

<style scoped>
.timeline-wrapper {
  position: relative;
  margin-bottom: 0.75rem;
}

/* Navigation */
.timeline-nav {
  display: flex;
  align-items: center;
  gap: 0.4rem;
  margin-bottom: 0.35rem;
}
.timeline-nav-btn {
  background: rgba(255,255,255,0.05);
  color: var(--bs-body-color, #c9d1d9);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.3rem;
  padding: 0.15rem 0.5rem;
  font-size: 0.75rem;
  cursor: pointer;
}
.timeline-nav-btn:hover { border-color: #555; }
.timeline-nav-btn:disabled { opacity: 0.3; cursor: default; }
.timeline-date-label {
  background: none;
  border: none;
  color: var(--bs-body-color, #c9d1d9);
  font-size: 0.8rem;
  font-weight: 600;
  cursor: pointer;
  padding: 0.15rem 0.4rem;
}
.timeline-date-label:hover { text-decoration: underline; }
.timeline-today-btn { color: #58a6ff; border-color: #58a6ff; }
.timeline-clear-btn { color: #ef4444; border-color: #ef4444; font-size: 0.7rem; }
.timeline-range-label {
  font-size: 0.7rem;
  color: rgba(255,255,255,0.5);
  margin-left: 0.5rem;
}

/* Calendar popup */
.timeline-calendar {
  position: absolute;
  top: 2rem;
  left: 2rem;
  z-index: 100;
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  padding: 0.5rem;
  width: 220px;
}
.cal-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 0.3rem;
}
.cal-nav {
  background: none;
  border: none;
  color: var(--bs-body-color);
  cursor: pointer;
  padding: 0.1rem 0.3rem;
  font-size: 0.8rem;
}
.cal-month {
  font-size: 0.75rem;
  font-weight: 600;
}
.cal-grid {
  display: grid;
  grid-template-columns: repeat(7, 1fr);
  gap: 2px;
}
.cal-dow {
  text-align: center;
  font-size: 0.6rem;
  color: rgba(255,255,255,0.4);
  padding: 2px;
}
.cal-cell {
  text-align: center;
  font-size: 0.65rem;
  padding: 3px 2px;
  border-radius: 3px;
  cursor: pointer;
  color: var(--bs-body-color);
}
.cal-cell:not(.cal-empty):hover {
  outline: 1px solid #58a6ff;
}
.cal-empty {
  cursor: default;
}
.cal-has-events {
  font-weight: 600;
}

/* Timeline bar */
.timeline-bar {
  position: relative;
  height: 36px;
  background: rgba(255,255,255,0.03);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.3rem;
  overflow: hidden;
  cursor: crosshair;
  user-select: none;
}

.timeline-hours {
  position: absolute;
  inset: 0;
  pointer-events: none;
}
.timeline-hour {
  position: absolute;
  top: 0;
  height: 100%;
}
.hour-tick {
  width: 1px;
  height: 6px;
  background: rgba(255,255,255,0.15);
}
.hour-label {
  position: absolute;
  top: 6px;
  left: -8px;
  font-size: 0.55rem;
  color: rgba(255,255,255,0.3);
  pointer-events: none;
}

/* Clip markers */
.timeline-clips {
  position: absolute;
  bottom: 2px;
  left: 0;
  right: 0;
  height: 14px;
}
.timeline-marker {
  position: absolute;
  height: 8px;
  min-width: 4px;
  border-radius: 2px;
  cursor: pointer;
  opacity: 0.85;
  transition: opacity 0.1s, height 0.1s;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
}
.timeline-marker:hover {
  opacity: 1;
  height: 12px;
  z-index: 10;
}
.timeline-marker:not(.has-emoji) {
  height: 5px;
  opacity: 0.5;
}
.timeline-marker:not(.has-emoji):hover {
  height: 10px;
  opacity: 0.85;
}
.marker-emoji {
  font-size: 0.6rem;
  line-height: 1;
  pointer-events: none;
}

/* Selection overlay */
.timeline-selection {
  position: absolute;
  top: 0;
  height: 100%;
  background: rgba(37, 99, 235, 0.25);
  border-left: 2px solid #2563eb;
  border-right: 2px solid #2563eb;
  pointer-events: none;
}
.timeline-drag {
  position: absolute;
  top: 0;
  height: 100%;
  background: rgba(37, 99, 235, 0.15);
  pointer-events: none;
}

/* Scrub cursor */
.timeline-scrub {
  position: absolute;
  top: 0;
  height: 100%;
  width: 1px;
  background: rgba(255,255,255,0.4);
  pointer-events: none;
}
.scrub-label {
  position: absolute;
  top: -16px;
  left: -14px;
  font-size: 0.6rem;
  color: rgba(255,255,255,0.7);
  background: rgba(0,0,0,0.8);
  padding: 1px 4px;
  border-radius: 3px;
  white-space: nowrap;
}

/* Loading */
.timeline-loading {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0,0,0,0.3);
}

/* Tooltip */
.timeline-tooltip {
  position: fixed;
  z-index: 1000;
  background: rgba(0,0,0,0.9);
  border: 1px solid #444;
  border-radius: 0.3rem;
  padding: 0.3rem 0.5rem;
  pointer-events: none;
  max-width: 250px;
}
.tt-camera {
  font-size: 0.7rem;
  font-weight: 600;
}
.tt-time {
  font-size: 0.65rem;
  color: rgba(255,255,255,0.6);
}
.tt-tags {
  font-size: 0.65rem;
  color: rgba(255,255,255,0.5);
  margin-top: 2px;
}
</style>
