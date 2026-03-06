<script setup lang="ts">
import { ref, computed, onMounted, watch, onUnmounted } from 'vue'
import { api } from '../../composables/useApi'
import { useEventStream, type WsEvent } from '../../composables/useEventStream'
import { useTagStore } from '../../stores/tags'
import { useFilterStore } from '../../stores/filters'
import { useCameraStore } from '../../stores/cameras'
import { format, startOfDay, startOfWeek, startOfMonth, subWeeks, subMonths, isToday as isTodayFn } from 'date-fns'
import DvrMultiPlayer from './DvrMultiPlayer.vue'

interface TimelineEvent {
  from: number
  to: number
  clipCount: number
  cameraIDs: number[]
  tags: string
}

interface CalendarDay {
  date: string
  count: number
}

const tagStore = useTagStore()
const filterStore = useFilterStore()

const events = ref<TimelineEvent[]>([])
const loading = ref(false)
const bucketSeconds = ref(300)
const retentionCutoff = ref<number | null>(null)
const retentionDays = ref<number | null>(null)

// DVR coverage
interface DvrRange { from: number; to: number }
const dvrCoverage = ref<Map<number, DvrRange[]>>(new Map())
const dvrPlayback = ref<{ from: number; to: number; startAt?: number } | null>(null)

const cameraStore = useCameraStore()
const { onEvent } = useEventStream()

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
const tooltipEvent = ref<TimelineEvent | null>(null)
const tooltipX = ref(0)
const tooltipY = ref(0)

// DVR thumbnail preview
const dvrThumbs = ref<{ camId: number; url: string; name: string }[]>([])
const dvrThumbTime = ref<string | null>(null)
const dvrThumbLeft = ref(0)
const dvrThumbY = ref(0)
let dvrThumbTimer: ReturnType<typeof setTimeout> | null = null

const SECONDS_PER_DAY = 86400

// Derived from filter store
const rangeFrom = computed(() => filterStore.timeRange?.from ?? Math.floor(startOfDay(new Date()).getTime() / 1000))
const rangeTo = computed(() => filterStore.timeRange?.to ?? rangeFrom.value + SECONDS_PER_DAY)
const rangeDuration = computed(() => rangeTo.value - rangeFrom.value)
const isMultiDay = computed(() => rangeDuration.value > SECONDS_PER_DAY)

// Camera color from ID
function cameraColor(id: number): string {
  const hue = (id * 137) % 360
  return `hsl(${hue}, 65%, 55%)`
}

// Range label
const rangeLabel = computed(() => {
  const from = new Date(rangeFrom.value * 1000)
  const to = new Date((rangeTo.value - 1) * 1000) // -1 so end-of-day shows same day
  if (rangeDuration.value <= SECONDS_PER_DAY) {
    return isTodayFn(from) ? 'Today' : format(from, 'EEE, dd MMM yyyy')
  }
  if (rangeDuration.value <= SECONDS_PER_DAY * 2) {
    return `${format(from, 'dd MMM')} — ${format(to, 'dd MMM')}`
  }
  return `${format(from, 'dd MMM yyyy')} — ${format(to, 'dd MMM yyyy')}`
})

function prevPeriod() {
  const dur = rangeDuration.value
  filterStore.setFilter('timeFrom', rangeFrom.value - dur)
  filterStore.setFilter('timeTo', rangeTo.value - dur)
}

function nextPeriod() {
  const dur = rangeDuration.value
  const newTo = rangeTo.value + dur
  const now = Math.floor(Date.now() / 1000) + SECONDS_PER_DAY
  if (newTo <= now) {
    filterStore.setFilter('timeFrom', rangeFrom.value + dur)
    filterStore.setFilter('timeTo', newTo)
  }
}

function goToday() {
  const todayStart = Math.floor(startOfDay(new Date()).getTime() / 1000)
  filterStore.setFilter('timeFrom', todayStart)
  filterStore.setFilter('timeTo', todayStart + SECONDS_PER_DAY)
}

const canGoNext = computed(() => {
  const now = Math.floor(Date.now() / 1000) + SECONDS_PER_DAY
  return rangeTo.value + rangeDuration.value <= now
})

async function fetchTimeline() {
  loading.value = true
  try {
    const data = await api<{ events: TimelineEvent[]; bucketSeconds: number; retentionCutoff?: number; retentionDays?: number }>(
      `/clip/timeline/${rangeFrom.value}/${rangeTo.value}`
    )
    events.value = data.events ?? []
    bucketSeconds.value = data.bucketSeconds ?? 300
    retentionCutoff.value = data.retentionCutoff ?? null
    retentionDays.value = data.retentionDays ?? null

    // For very wide ranges (like "Older"), tighten from to earliest event
    if (events.value.length > 0 && rangeDuration.value > SECONDS_PER_DAY * 60) {
      const earliest = Math.min(...events.value.map(e => e.from))
      if (earliest > rangeFrom.value + SECONDS_PER_DAY) {
        // Snap from to start of that day
        const d = new Date(earliest * 1000)
        const dayStart = Math.floor(startOfDay(d).getTime() / 1000)
        filterStore.setFilter('timeFrom', dayStart)
      }
    }
  } catch (err) {
    console.error('Timeline fetch failed:', err)
    events.value = []
  } finally {
    loading.value = false
  }
}

// DVR coverage
async function fetchDvrCoverage() {
  const cameras = cameraStore.cameras
  const from = rangeFrom.value
  const to = rangeTo.value
  const newCoverage = new Map<number, DvrRange[]>()

  for (const cam of cameras) {
    try {
      const data = await api<{ ranges: DvrRange[] }>(`/dvr/coverage/${cam.id}/${from}/${to}`)
      if (data.ranges?.length) {
        newCoverage.set(cam.id, data.ranges)
      }
    } catch { /* camera may not have continuous recording */ }
  }
  dvrCoverage.value = newCoverage
}

// Camera IDs that have DVR coverage
const dvrCameraIds = computed(() => Array.from(dvrCoverage.value.keys()).sort((a, b) => a - b))

// Merge all cameras' coverage into a single unified bar
const dvrMergedRanges = computed(() => {
  const all: DvrRange[] = []
  for (const ranges of dvrCoverage.value.values()) {
    all.push(...ranges)
  }
  if (all.length === 0) return []
  all.sort((a, b) => a.from - b.from)
  const merged: DvrRange[] = [{ ...all[0]! }]
  for (let i = 1; i < all.length; i++) {
    const last = merged[merged.length - 1]!
    if (all[i]!.from <= last.to + 2) {
      last.to = Math.max(last.to, all[i]!.to)
    } else {
      merged.push({ ...all[i]! })
    }
  }
  return merged
})

const dvrSegments = computed(() => {
  return dvrMergedRanges.value.map(r => {
    const left = tsToPercent(r.from)
    const right = tsToPercent(r.to)
    const width = Math.max(0.2, right - left)
    return { left, width, from: r.from, to: r.to }
  })
})

function onDvrClick(seg: { from: number; to: number }, event: MouseEvent) {
  const bar = (event.currentTarget as HTMLElement)
  const rect = bar.getBoundingClientRect()
  const pct = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width))
  const clickedTs = seg.from + pct * (seg.to - seg.from)
  dvrPlayback.value = { from: seg.from, to: seg.to, startAt: clickedTs }
}

function closeDvrPlayer() {
  dvrPlayback.value = null
}

// Calendar
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
    const d = new Date(rangeFrom.value * 1000)
    calendarYear.value = d.getFullYear()
    calendarMonth.value = d.getMonth() + 1
    fetchCalendar()
  }
}

function calendarPrevMonth() {
  calendarMonth.value--
  if (calendarMonth.value < 1) { calendarMonth.value = 12; calendarYear.value-- }
  fetchCalendar()
}

function calendarNextMonth() {
  calendarMonth.value++
  if (calendarMonth.value > 12) { calendarMonth.value = 1; calendarYear.value++ }
  fetchCalendar()
}

function selectCalendarDay(dateStr: string) {
  const d = new Date(dateStr + 'T00:00:00')
  const from = Math.floor(d.getTime() / 1000)
  filterStore.setFilter('timeFrom', from)
  filterStore.setFilter('timeTo', from + SECONDS_PER_DAY)
  showCalendar.value = false
}

const calendarGrid = computed(() => {
  const firstDay = new Date(calendarYear.value, calendarMonth.value - 1, 1)
  const daysInMonth = new Date(calendarYear.value, calendarMonth.value, 0).getDate()
  const startDow = firstDay.getDay()
  const countMap = new Map<string, number>()
  for (const d of calendarDays.value) countMap.set(d.date, d.count)
  const maxCount = Math.max(1, ...calendarDays.value.map(d => d.count))

  const cells: { day: number; date: string; count: number; intensity: number; empty: boolean }[] = []
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

// Axis ticks
interface AxisTick {
  pct: number
  label: string
  major: boolean
}

const axisTicks = computed((): AxisTick[] => {
  const ticks: AxisTick[] = []
  const dur = rangeDuration.value

  if (dur <= SECONDS_PER_DAY) {
    // Day view: tick every hour, label every 3 hours
    for (let h = 0; h < 24; h++) {
      ticks.push({
        pct: (h / 24) * 100,
        label: h % 3 === 0 ? String(h).padStart(2, '0') : '',
        major: h % 3 === 0,
      })
    }
  } else if (dur <= SECONDS_PER_DAY * 7) {
    // Week view: tick every 12h, label every day
    const days = Math.ceil(dur / SECONDS_PER_DAY)
    for (let d = 0; d < days; d++) {
      const ts = rangeFrom.value + d * SECONDS_PER_DAY
      const pct = (d / days) * 100
      ticks.push({ pct, label: format(new Date(ts * 1000), 'EEE'), major: true })
      if (d < days) {
        ticks.push({ pct: ((d + 0.5) / days) * 100, label: '', major: false })
      }
    }
  } else {
    // Long range: cap to ~20 labeled ticks max
    const days = Math.ceil(dur / SECONDS_PER_DAY)
    const labelEvery = Math.max(1, Math.ceil(days / 20))
    const tickEvery = Math.max(1, Math.ceil(days / 60))
    for (let d = 0; d < days; d += tickEvery) {
      const ts = rangeFrom.value + d * SECONDS_PER_DAY
      const pct = (d / days) * 100
      const isMajor = d % labelEvery === 0
      const labelFmt = days > 60 ? 'dd MMM' : 'd'
      ticks.push({ pct, label: isMajor ? format(new Date(ts * 1000), labelFmt) : '', major: isMajor })
    }
  }
  return ticks
})

// Timeline position helpers
function tsToPercent(timestamp: number): number {
  const offset = timestamp - rangeFrom.value
  return Math.max(0, Math.min(100, (offset / rangeDuration.value) * 100))
}

function bucketToPercent(): number {
  return Math.max(0.3, (bucketSeconds.value / rangeDuration.value) * 100)
}

function xToTimestamp(x: number): number {
  if (!timelineEl.value) return rangeFrom.value
  const rect = timelineEl.value.getBoundingClientRect()
  const pct = Math.max(0, Math.min(1, (x - rect.left) / rect.width))
  return rangeFrom.value + pct * rangeDuration.value
}

function formatTimestamp(ts: number): string {
  const d = new Date(ts * 1000)
  if (isMultiDay.value) {
    return format(d, 'EEE dd MMM HH:mm')
  }
  return format(d, 'HH:mm')
}

// Mouse handlers
function onMouseMove(e: MouseEvent) {
  if (!timelineEl.value) return
  const rect = timelineEl.value.getBoundingClientRect()
  scrubX.value = e.clientX - rect.left
  scrubTime.value = formatTimestamp(xToTimestamp(e.clientX))

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
  // Track drag globally so cursor can leave the bar
  window.addEventListener('mousemove', onDragMove)
  window.addEventListener('mouseup', onDragEnd)
}

function onDragMove(e: MouseEvent) {
  if (!timelineEl.value) return
  const rect = timelineEl.value.getBoundingClientRect()
  // Clamp to bar bounds
  dragEndX.value = Math.max(rect.left, Math.min(rect.right, e.clientX))
  scrubX.value = dragEndX.value - rect.left
  scrubTime.value = formatTimestamp(xToTimestamp(dragEndX.value))
}

function onDragEnd() {
  window.removeEventListener('mousemove', onDragMove)
  window.removeEventListener('mouseup', onDragEnd)
  if (!isDragging.value) return
  isDragging.value = false

  const tsStart = xToTimestamp(Math.min(dragStartX.value, dragEndX.value))
  const tsEnd = xToTimestamp(Math.max(dragStartX.value, dragEndX.value))

  // Only register if dragged more than ~5 min
  if (tsEnd - tsStart > 300) {
    selectionFrom.value = tsStart
    selectionTo.value = tsEnd
    filterStore.setFilter('timeFrom', tsStart)
    filterStore.setFilter('timeTo', tsEnd)
  }
}

function onMouseUp() {
  // handled by onDragEnd via window listener
}

function onMouseLeave() {
  if (!isDragging.value) {
    scrubTime.value = null
  }
}

// Selection overlay position
const selectionStyle = computed(() => {
  if (!selectionFrom.value || !selectionTo.value) return null
  const left = tsToPercent(selectionFrom.value)
  const right = tsToPercent(selectionTo.value)
  return { left: `${left}%`, width: `${right - left}%` }
})

const dragStyle = computed(() => {
  if (!isDragging.value || !timelineEl.value) return null
  const rect = timelineEl.value.getBoundingClientRect()
  const left = Math.min(dragStartX.value, dragEndX.value) - rect.left
  const width = Math.abs(dragEndX.value - dragStartX.value)
  return { left: `${left}px`, width: `${width}px` }
})

const dragStartLabel = computed(() => {
  if (!isDragging.value) return ''
  return formatTimestamp(xToTimestamp(Math.min(dragStartX.value, dragEndX.value)))
})

const dragEndLabel = computed(() => {
  if (!isDragging.value) return ''
  return formatTimestamp(xToTimestamp(Math.max(dragStartX.value, dragEndX.value)))
})

// Event emojis
function getEventEmojis(evt: TimelineEvent): string {
  if (!evt.tags) return ''
  const tags = evt.tags.split(/[;,]/).map(t => t.trim()).filter(Boolean)
  const emojis: string[] = []
  const seen = new Set<string>()
  for (const t of tags) {
    const info = tagStore.getTagDisplay(t)
    if (info.icon && !seen.has(info.icon)) {
      seen.add(info.icon)
      emojis.push(info.icon)
    }
  }
  return emojis.join('')
}

// Retention cutoff line position (percentage)
const retentionLinePct = computed(() => {
  if (!retentionCutoff.value) return null
  const pct = tsToPercent(retentionCutoff.value)
  if (pct <= 0 || pct >= 100) return null
  return pct
})

// Emoji row above timeline — group all emojis per event as a string, suppress duplicates nearby
interface EmojiMarker {
  emojis: string  // all emojis for this event as a string
  leftPct: number
  key: string
}

const emojiMarkers = computed(() => {
  const markers: EmojiMarker[] = []

  for (const evt of events.value) {
    const emojis = getEventEmojis(evt)
    if (!emojis) continue

    const leftPct = Math.min(tsToPercent(evt.from), 98)
    markers.push({ emojis, leftPct, key: `${evt.from}-${emojis}` })
  }
  return markers
})

function onEventHover(evt: TimelineEvent, e: MouseEvent) {
  tooltipEvent.value = evt
  tooltipX.value = e.clientX
  tooltipY.value = e.clientY
}

function onEventLeave() {
  tooltipEvent.value = null
}

// DVR bar hover → thumbnail preview
function camerasAtTimestamp(ts: number): number[] {
  const result: number[] = []
  for (const [camId, ranges] of dvrCoverage.value) {
    for (const r of ranges) {
      if (ts >= r.from && ts <= r.to) { result.push(camId); break }
    }
  }
  return result.sort((a, b) => a - b)
}

function onDvrHover(seg: { from: number; to: number }, e: MouseEvent) {
  const bar = e.currentTarget as HTMLElement
  const rect = bar.getBoundingClientRect()
  const pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width))
  const ts = Math.floor(seg.from + pct * (seg.to - seg.from))

  dvrThumbY.value = rect.top
  dvrThumbTime.value = formatTimestamp(ts)

  if (dvrThumbTimer) clearTimeout(dvrThumbTimer)
  dvrThumbTimer = setTimeout(() => {
    const camIds = camerasAtTimestamp(ts)
    dvrThumbs.value = camIds.map(id => ({
      camId: id,
      url: `/dvr/thumbnail/${id}/${ts}`,
      name: cameraStore.cameras.find(c => c.id === id)?.name ?? `Camera ${id}`,
    }))
    // Compute clamped left so tooltip doesn't overflow viewport
    const cellW = 164 // 160px img + 4px gap
    const tooltipW = camIds.length * cellW + 12
    const left = e.clientX - tooltipW / 2
    dvrThumbLeft.value = Math.max(8, Math.min(window.innerWidth - tooltipW - 8, left))
  }, 100)
}

function onDvrLeave() {
  if (dvrThumbTimer) { clearTimeout(dvrThumbTimer); dvrThumbTimer = null }
  dvrThumbs.value = []
  dvrThumbTime.value = null
}

// Time presets dropdown
const DAY = 86400

type TimePreset = 'today' | 'yesterday' | 'thisWeek' | 'lastWeek' | 'thisMonth' | 'lastMonth' | 'older' | 'custom'

const showCustomPopup = ref(false)
const customFrom = ref('')
const customTo = ref('')
let customCloseTimer: ReturnType<typeof setTimeout> | null = null

function startCustomClose() {
  customCloseTimer = setTimeout(() => { showCustomPopup.value = false }, 400)
}
function cancelCustomClose() {
  if (customCloseTimer) { clearTimeout(customCloseTimer); customCloseTimer = null }
}

// Show time range only when user has dragged a custom selection
const timeRangeLabel = computed(() => {
  if (!selectionFrom.value || !selectionTo.value) return ''
  if (rangeDuration.value > SECONDS_PER_DAY * 3) return ''
  const from = new Date(rangeFrom.value * 1000)
  const to = new Date(rangeTo.value * 1000)
  return `${format(from, 'HH:mm')} – ${format(to, 'HH:mm')}`
})

const activeTimePreset = computed<TimePreset | ''>(() => {
  const r = filterStore.timeRange
  if (!r) return ''
  const now = new Date()
  const todayStart = Math.floor(startOfDay(now).getTime() / 1000)
  const todayEnd = todayStart + DAY
  if (r.from === todayStart && r.to === todayEnd) return 'today'
  if (r.from === todayStart - DAY && r.to === todayStart) return 'yesterday'
  const thisWeekStart = Math.floor(startOfWeek(now, { weekStartsOn: 1 }).getTime() / 1000)
  if (r.from === thisWeekStart && r.to === todayEnd) return 'thisWeek'
  const lastWeekStart = Math.floor(startOfWeek(subWeeks(now, 1), { weekStartsOn: 1 }).getTime() / 1000)
  if (r.from === lastWeekStart && r.to === thisWeekStart) return 'lastWeek'
  const thisMonthStart = Math.floor(startOfMonth(now).getTime() / 1000)
  if (r.from === thisMonthStart && r.to === todayEnd) return 'thisMonth'
  const lastMonthStart = Math.floor(startOfMonth(subMonths(now, 1)).getTime() / 1000)
  if (r.from === lastMonthStart && r.to === thisMonthStart) return 'lastMonth'
  if (r.to <= lastMonthStart) return 'older'
  return ''
})

function onPresetChange(e: Event) {
  const preset = (e.target as HTMLSelectElement).value as TimePreset | ''
  showCustomPopup.value = false
  if (!preset) {
    filterStore.clearFilter('timeFrom')
    filterStore.clearFilter('timeTo')
    return
  }
  const now = new Date()
  const todayStart = Math.floor(startOfDay(now).getTime() / 1000)
  const todayEnd = todayStart + DAY
  switch (preset) {
    case 'today':
      filterStore.setFilter('timeFrom', todayStart)
      filterStore.setFilter('timeTo', todayEnd)
      break
    case 'yesterday':
      filterStore.setFilter('timeFrom', todayStart - DAY)
      filterStore.setFilter('timeTo', todayStart)
      break
    case 'thisWeek':
      filterStore.setFilter('timeFrom', Math.floor(startOfWeek(now, { weekStartsOn: 1 }).getTime() / 1000))
      filterStore.setFilter('timeTo', todayEnd)
      break
    case 'lastWeek': {
      const lwStart = Math.floor(startOfWeek(subWeeks(now, 1), { weekStartsOn: 1 }).getTime() / 1000)
      filterStore.setFilter('timeFrom', lwStart)
      filterStore.setFilter('timeTo', Math.floor(startOfWeek(now, { weekStartsOn: 1 }).getTime() / 1000))
      break
    }
    case 'thisMonth':
      filterStore.setFilter('timeFrom', Math.floor(startOfMonth(now).getTime() / 1000))
      filterStore.setFilter('timeTo', todayEnd)
      break
    case 'lastMonth': {
      const lmStart = Math.floor(startOfMonth(subMonths(now, 1)).getTime() / 1000)
      filterStore.setFilter('timeFrom', lmStart)
      filterStore.setFilter('timeTo', Math.floor(startOfMonth(now).getTime() / 1000))
      break
    }
    case 'older':
      filterStore.setFilter('timeFrom', 0)
      filterStore.setFilter('timeTo', Math.floor(startOfMonth(subMonths(now, 1)).getTime() / 1000))
      break
    case 'custom':
      showCustomPopup.value = true
      if (filterStore.timeRange) {
        customFrom.value = format(new Date(filterStore.timeRange.from * 1000), 'yyyy-MM-dd')
        customTo.value = format(new Date(filterStore.timeRange.to * 1000), 'yyyy-MM-dd')
      } else {
        customFrom.value = format(new Date(), 'yyyy-MM-dd')
        customTo.value = format(new Date(), 'yyyy-MM-dd')
      }
      break
  }
}

function applyCustomRange() {
  if (!customFrom.value || !customTo.value) return
  const from = Math.floor(new Date(customFrom.value + 'T00:00:00').getTime() / 1000)
  const to = Math.floor(new Date(customTo.value + 'T00:00:00').getTime() / 1000) + DAY
  if (from < to) {
    filterStore.setFilter('timeFrom', from)
    filterStore.setFilter('timeTo', to)
  }
}

// Watch filter store time range and refetch timeline
watch(() => filterStore.timeRange, () => {
  selectionFrom.value = null
  selectionTo.value = null
  fetchTimeline()
  fetchDvrCoverage()
}, { deep: true })

// Listen for real-time DVR segment events
let unsubDvr: (() => void) | null = null

onMounted(() => {
  if (tagStore.tags.length === 0) tagStore.fetchTags()
  // Set initial time range if not already set
  if (!filterStore.timeRange) {
    goToday()
  }
  fetchTimeline()
  fetchDvrCoverage()

  unsubDvr = onEvent((evt: WsEvent) => {
    if (evt.event === 'dvr:segment') {
      const seg = evt.data as { cameraId: number; from: number; to: number }
      // Extend coverage in-place if within visible range
      if (seg.from <= rangeTo.value && seg.to >= rangeFrom.value) {
        const existing = dvrCoverage.value.get(seg.cameraId) ?? []
        // Try to extend last range
        if (existing.length > 0 && seg.from <= existing[existing.length - 1]!.to + 5) {
          existing[existing.length - 1]!.to = Math.max(existing[existing.length - 1]!.to, seg.to)
        } else {
          existing.push({ from: seg.from, to: seg.to })
        }
        dvrCoverage.value.set(seg.cameraId, existing)
        // Trigger reactivity
        dvrCoverage.value = new Map(dvrCoverage.value)
      }
    }
  })
})

onUnmounted(() => {
  window.removeEventListener('mousemove', onDragMove)
  window.removeEventListener('mouseup', onDragEnd)
  if (unsubDvr) unsubDvr()
})
</script>

<template>
  <div class="timeline-wrapper">
    <!-- Navigation -->
    <div class="timeline-nav">
      <div class="timeline-nav-spacer"></div>
      <div class="time-preset-wrapper"
        @mouseenter="cancelCustomClose"
        @mouseleave="startCustomClose"
        @focusin="cancelCustomClose"
        @focusout="startCustomClose"
      >
        <select class="time-preset-select" :value="activeTimePreset" @change="onPresetChange">
          <option value="">All time</option>
          <option value="today">Today</option>
          <option value="yesterday">Yesterday</option>
          <option value="thisWeek">This Week</option>
          <option value="lastWeek">Last Week</option>
          <option value="thisMonth">This Month</option>
          <option value="lastMonth">Last Month</option>
          <option value="older">Older</option>
          <option value="custom">Custom range…</option>
        </select>
        <!-- Custom range popup -->
        <div v-if="showCustomPopup" class="custom-popup">
          <div class="custom-popup-row">
            <label class="custom-popup-label">From</label>
            <input type="date" v-model="customFrom" class="custom-popup-date" @change="applyCustomRange" />
          </div>
          <div class="custom-popup-row">
            <label class="custom-popup-label">To</label>
            <input type="date" v-model="customTo" class="custom-popup-date" @change="applyCustomRange" />
          </div>
        </div>
      </div>
      <button class="timeline-nav-btn" @click="prevPeriod" title="Previous period">←</button>
      <div class="timeline-date-group">
        <button class="timeline-date-label" @click="toggleCalendar">{{ rangeLabel }}</button>
        <span v-if="timeRangeLabel" class="timeline-time-range">{{ timeRangeLabel }}</span>
      </div>
      <button class="timeline-nav-btn" @click="nextPeriod" :disabled="!canGoNext" title="Next period">→</button>
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
    <div class="timeline-container">
      <!-- Emoji row above -->
      <div class="emoji-row" v-if="emojiMarkers.length > 0">
        <span
          v-for="m in emojiMarkers"
          :key="m.key"
          class="emoji-marker"
          :style="{ left: `${m.leftPct}%` }"
        >{{ m.emojis }}</span>
      </div>

      <div
        ref="timelineEl"
        class="timeline-bar"
        @mousemove="onMouseMove"
        @mousedown="onMouseDown"
        @mouseup="onMouseUp"
        @mouseleave="onMouseLeave"
      >
        <!-- Axis ticks -->
        <div class="timeline-hours">
          <div
            v-for="(tick, i) in axisTicks"
            :key="i"
            class="timeline-hour"
            :style="{ left: `${tick.pct}%` }"
          >
            <div class="hour-tick" :class="{ major: tick.major }"></div>
            <span v-if="tick.label" class="hour-label">{{ tick.label }}</span>
          </div>
        </div>

        <!-- DVR coverage bars -->
        <div class="timeline-dvr-coverage">
          <div
            v-for="(seg, i) in dvrSegments"
            :key="`dvr-${i}`"
            class="dvr-bar"
            :style="{
              left: `${seg.left}%`,
              width: `${seg.width}%`,
            }"
            :title="`DVR: ${formatTimestamp(seg.from)} — ${formatTimestamp(seg.to)}`"
            @click.stop="onDvrClick(seg, $event)"
            @mousemove="onDvrHover(seg, $event)"
            @mouseleave="onDvrLeave"
          ></div>
        </div>

        <!-- Event markers -->
        <div class="timeline-clips">
          <div
            v-for="(evt, i) in events"
            :key="i"
            class="timeline-marker"
            :class="{ 'has-emoji': !!getEventEmojis(evt) }"
            :style="{
              left: `${tsToPercent(evt.from)}%`,
              width: `${bucketToPercent()}%`,
              background: evt.cameraIDs.length === 1
                ? cameraColor(evt.cameraIDs[0]!)
                : `linear-gradient(90deg, ${evt.cameraIDs.map((id: number, j: number) => cameraColor(id) + ' ' + (j / evt.cameraIDs.length * 100) + '%').join(', ')})`,
            }"
            @mouseenter="onEventHover(evt, $event)"
            @mouseleave="onEventLeave"
          ></div>
        </div>

        <!-- Retention cutoff line -->
        <!-- (moved outside timeline-bar to avoid overflow:hidden clipping) -->

        <!-- Selection overlay -->
        <div v-if="selectionStyle" class="timeline-selection" :style="selectionStyle"></div>

        <!-- Drag preview -->
        <div v-if="dragStyle && isDragging" class="timeline-drag" :style="dragStyle">
          <span class="drag-label drag-start">{{ dragStartLabel }}</span>
          <span class="drag-label drag-end">{{ dragEndLabel }}</span>
        </div>

        <!-- Scrub cursor — always visible on hover -->
        <div v-if="scrubTime" class="timeline-scrub" :style="{ left: `${scrubX}px` }">
          <span class="scrub-label">{{ scrubTime }}</span>
        </div>

        <!-- Loading -->
        <div v-if="loading" class="timeline-loading">
          <div class="spinner-border spinner-border-sm" />
        </div>
      </div>

      <!-- Retention cutoff line (outside bar to avoid overflow:hidden) -->
      <div
        v-if="retentionLinePct !== null"
        class="retention-line"
        :style="{ left: `${retentionLinePct}%` }"
        :title="`Clips older than ${retentionDays} days are auto-deleted`"
      >
        <span class="retention-label">🗑️ {{ retentionDays }}d</span>
      </div>
    </div>

    <!-- Tooltip -->
    <div
      v-if="tooltipEvent"
      class="timeline-tooltip"
      :style="{ left: `${tooltipX + 10}px`, top: `${tooltipY - 60}px` }"
    >
      <div class="tt-time">
        {{ formatTimestamp(tooltipEvent.from) }}
        · {{ tooltipEvent.clipCount }} clip{{ tooltipEvent.clipCount > 1 ? 's' : '' }}
      </div>
      <div v-if="getEventEmojis(tooltipEvent)" class="tt-tags">{{ getEventEmojis(tooltipEvent) }}</div>
    </div>

    <!-- DVR thumbnail preview -->
    <div
      v-if="dvrThumbTime && dvrThumbs.length"
      class="dvr-thumb-tooltip"
      :style="{ left: `${dvrThumbLeft}px`, top: `${dvrThumbY}px` }"
    >
      <div class="dvr-thumb-grid">
        <div v-for="t in dvrThumbs" :key="t.camId" class="dvr-thumb-cell">
          <img :src="t.url" class="dvr-thumb-img" alt=""
            @error="($event.target as HTMLImageElement).style.display='none';
              (($event.target as HTMLElement).nextElementSibling as HTMLElement).style.display='flex'" />
          <div class="dvr-thumb-offline" style="display:none">Offline</div>
          <div class="dvr-thumb-cam">{{ t.name }}</div>
        </div>
      </div>
      <div class="dvr-thumb-time">{{ dvrThumbTime }}</div>
    </div>

    <!-- DVR multi-camera player -->
    <DvrMultiPlayer
      v-if="dvrPlayback"
      :camera-ids="dvrCameraIds"
      :from="dvrPlayback.from"
      :to="dvrPlayback.to"
      :start-at="dvrPlayback.startAt"
      @close="closeDvrPlayer"
    />
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
.timeline-date-group {
  display: flex;
  align-items: baseline;
  gap: 0.35rem;
}
.timeline-time-range {
  font-size: 0.7rem;
  color: rgba(255,255,255,0.45);
  white-space: nowrap;
}
.timeline-nav-spacer { flex: 1; }

/* Time preset dropdown */
.time-preset-wrapper {
  position: relative;
}
.time-preset-select {
  background: rgba(255,255,255,0.05);
  color: var(--bs-body-color, #c9d1d9);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.3rem;
  padding: 0.15rem 0.4rem;
  font-size: 0.75rem;
  cursor: pointer;
}
.time-preset-select:hover { border-color: #555; }
.time-preset-select option {
  background: var(--bs-dark, #1e1e2e);
  color: var(--bs-body-color, #c9d1d9);
}

/* Custom range popup */
.custom-popup {
  position: absolute;
  top: 100%;
  right: 0;
  z-index: 100;
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  padding: 0.75rem;
  margin-top: 0.3rem;
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
  min-width: 260px;
  box-shadow: 0 4px 12px rgba(0,0,0,0.4);
}
.custom-popup-row {
  display: flex;
  align-items: center;
  gap: 0.5rem;
}
.custom-popup-label {
  font-size: 0.8rem;
  color: rgba(255,255,255,0.6);
  min-width: 2.5rem;
}
.custom-popup-date {
  flex: 1;
  background: rgba(255,255,255,0.05);
  color: var(--bs-body-color, #c9d1d9);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.3rem;
  padding: 0.3rem 0.5rem;
  font-size: 0.8rem;
}
.custom-popup-date::-webkit-calendar-picker-indicator {
  filter: invert(0.7);
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

/* Timeline container */
.timeline-container {
  position: relative;
}

/* Emoji row above timeline */
.emoji-row {
  position: relative;
  height: 24px;
  margin-bottom: 1px;
  overflow: visible;
}

/* Timeline container (holds emoji row + bar + retention line) */
.timeline-container {
  position: relative;
}
.emoji-marker {
  position: absolute;
  transform: translateX(-50%);
  font-size: 1.15rem;
  line-height: 1;
  pointer-events: none;
  white-space: nowrap;
  letter-spacing: -0.5em;
}

/* Timeline bar */
.timeline-bar {
  position: relative;
  height: 28px;
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
.hour-tick.major {
  height: 8px;
  background: rgba(255,255,255,0.25);
}
.hour-label {
  position: absolute;
  top: 6px;
  left: -8px;
  font-size: 0.55rem;
  color: rgba(255,255,255,0.3);
  pointer-events: none;
}

/* DVR coverage */
.timeline-dvr-coverage {
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  height: 100%;
  z-index: 1;
  pointer-events: none;
}
.dvr-bar {
  position: absolute;
  height: 100%;
  background: rgba(59, 130, 246, 0.15);
  pointer-events: auto;
  cursor: pointer;
  transition: background 0.15s;
}
.dvr-bar:hover {
  background: rgba(59, 130, 246, 0.35);
}

/* Clip markers */
.timeline-clips {
  position: absolute;
  bottom: 2px;
  left: 0;
  right: 0;
  height: 14px;
  pointer-events: none;
}
.timeline-marker {
  position: absolute;
  height: 8px;
  min-width: 4px;
  border-radius: 2px;
  cursor: pointer;
  opacity: 0.85;
  transition: opacity 0.1s, height 0.1s;
  pointer-events: auto;
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

/* Retention cutoff line — positioned relative to .timeline-container */
.retention-line {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  background: #ef4444;
  z-index: 5;
  pointer-events: auto;
  cursor: help;
}
.retention-label {
  position: absolute;
  top: -2px;
  left: 4px;
  font-size: 0.6rem;
  color: #ef4444;
  white-space: nowrap;
}

.timeline-drag {
  position: absolute;
  top: 0;
  height: 100%;
  background: rgba(37, 99, 235, 0.15);
  pointer-events: none;
}
.drag-label {
  position: absolute;
  bottom: -14px;
  font-size: 0.55rem;
  color: #58a6ff;
  background: rgba(0,0,0,0.8);
  padding: 0 3px;
  border-radius: 2px;
  white-space: nowrap;
}
.drag-start { left: 0; }
.drag-end { right: 0; }

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
  top: 1px;
  left: 4px;
  font-size: 0.6rem;
  color: rgba(255,255,255,0.9);
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
.tt-time {
  font-size: 0.7rem;
  color: rgba(255,255,255,0.8);
}
.tt-tags {
  font-size: 0.85rem;
  margin-top: 2px;
}

/* DVR thumbnail hover preview */
.dvr-thumb-tooltip {
  position: fixed;
  transform: translateY(-100%);
  margin-top: -8px;
  z-index: 1001;
  background: rgba(0, 0, 0, 0.92);
  border: 1px solid #555;
  border-radius: 6px;
  padding: 6px;
  pointer-events: none;
  text-align: center;
  white-space: nowrap;
}
.dvr-thumb-grid {
  display: flex;
  gap: 4px;
  flex-wrap: wrap;
  justify-content: center;
}
.dvr-thumb-cell {
  text-align: center;
}
.dvr-thumb-img {
  display: block;
  width: 160px;
  height: auto;
  border-radius: 3px;
}
.dvr-thumb-offline {
  width: 160px;
  height: 90px;
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
.dvr-thumb-cam {
  font-size: 0.6rem;
  color: rgba(255, 255, 255, 0.7);
  margin-top: 1px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 160px;
}
.dvr-thumb-time {
  font-size: 0.7rem;
  color: rgba(255, 255, 255, 0.85);
  margin-top: 3px;
}
</style>
