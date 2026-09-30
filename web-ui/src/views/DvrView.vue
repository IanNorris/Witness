<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { format } from 'date-fns'
import DvrPlayer from '../components/clips/DvrPlayer.vue'
import AppLayout from '../components/layout/AppLayout.vue'
import { buildHighlights, isVideoActivityAt, nextHighlight } from '../utils/dvrHighlights'
import { api } from '../composables/useApi'
import { useCameraStore } from '../stores/cameras'

interface Range { from: number; to: number }
interface Activity extends Range { id: number }
interface AudioEvent extends Range { group: string; score: number }
interface Timeline { ranges: Range[]; clips: Activity[]; audio: AudioEvent[]; clipsTruncated?: boolean; audioTruncated?: boolean }
interface PlayerHandle {
  setPlaying: (value: boolean) => void
  setRate: (rate: number) => void
}

const route = useRoute()
const cameraStore = useCameraStore()
const selected = ref<number[]>([])
const targetAt = ref(Math.floor(Date.now() / 1000) - 300)
const viewedAt = ref(targetAt.value)
const windowStart = ref(targetAt.value - 1800)
const windowEnd = computed(() => windowStart.value + 3600)
const timeline = ref<Record<number, Timeline>>({})
const timelineError = ref('')
const loading = ref(false)
const players = new Map<number, PlayerHandle>()
const playing = ref(true)
const rate = ref(1)
const highlightsEnabled = ref(false)
const highlightDriver = ref<number | null>(null)
let pendingHighlightSeek: number | null = null
const highlightsAvailable = computed(() => selected.value.length > 0 && !loading.value && !timelineError.value
  && selected.value.every(id => timeline.value[id] && !timeline.value[id]!.clipsTruncated))
const highlights = computed(() => buildHighlights(selected.value.map(id => ({
  id, clips: timeline.value[id]?.clips ?? [], ranges: timeline.value[id]?.ranges ?? [],
})), windowStart.value, windowEnd.value))
const highlightStatus = ref('')
const tooEarly = ref<number | null>(null)
const tooLate = ref<number | null>(null)
const history = ref<{ early: number | null; late: number | null; target: number }[]>([])
let fetchGeneration = 0

const localInput = ref(format(new Date(targetAt.value * 1000), "yyyy-MM-dd'T'HH:mm:ss"))
const viewedClock = computed(() => format(new Date(viewedAt.value * 1000), 'dd MMM yyyy HH:mm:ss'))
const timelineTicks = computed(() => Array.from({ length: 7 }, (_, index) => ({
  at: windowStart.value + index * 600,
  label: format(new Date((windowStart.value + index * 600) * 1000), 'HH:mm'),
})))
const videoEventCount = computed(() => selected.value.reduce((count, id) => count + (timeline.value[id]?.clips.length ?? 0), 0))
const audioEventCount = computed(() => selected.value.reduce((count, id) => count + (timeline.value[id]?.audio.length ?? 0), 0))
const remaining = computed(() => tooEarly.value !== null && tooLate.value !== null
  ? Math.max(0, tooLate.value - tooEarly.value) : null)

function selectCamera(id: number) {
  if (!selected.value.includes(id)) seekTo(viewedAt.value, true)
  selected.value = selected.value.includes(id)
    ? selected.value.filter(v => v !== id)
    : [...selected.value, id]
}

function seekTo(ts: number, highlightSeek = false) {
  if (!Number.isFinite(ts) || ts < 0) return
  // Disable before updating the target: the highlight watcher must not pull a
  // manual seek in a quiet period straight back into the reel. Internal skips
  // and camera selection still preserve highlights and its lead-in/tail.
  if (!highlightSeek && highlightsEnabled.value && (!highlightsAvailable.value
    || !isVideoActivityAt(selected.value.flatMap(id => timeline.value[id]?.clips ?? []), ts))) {
    highlightsEnabled.value = false
  }
  targetAt.value = ts
  viewedAt.value = targetAt.value
  pendingHighlightSeek = null
  if (!highlightSeek) highlightDriver.value = null
  localInput.value = format(new Date(targetAt.value * 1000), "yyyy-MM-dd'T'HH:mm:ss")
  if (targetAt.value < windowStart.value || targetAt.value >= windowEnd.value) {
    windowStart.value = Math.floor(targetAt.value) - 1800
  }
}

function setClock() {
  const parsed = new Date(localInput.value).getTime() / 1000
  if (Number.isFinite(parsed)) seekTo(parsed)
}

function move(seconds: number) { seekTo(viewedAt.value + seconds) }
function shiftWindow(seconds: number) {
  windowStart.value += seconds
  seekTo(viewedAt.value + seconds)
}

function mark(kind: 'early' | 'late') {
  const current = Math.floor(viewedAt.value)
  history.value.push({ early: tooEarly.value, late: tooLate.value, target: current })
  if (kind === 'early') tooEarly.value = current
  else tooLate.value = current
  if (tooEarly.value !== null && tooLate.value !== null && tooEarly.value >= tooLate.value) {
    const previous = history.value.pop()!
    tooEarly.value = previous.early
    tooLate.value = previous.late
    return
  }
  const lower = tooEarly.value ?? windowStart.value
  const upper = tooLate.value ?? windowEnd.value
  playing.value = false
  for (const player of players.values()) player.setPlaying(false)
  seekTo(Math.floor((lower + upper) / 2))
}

function undoMark() {
  const previous = history.value.pop()
  if (!previous) return
  tooEarly.value = previous.early
  tooLate.value = previous.late
  seekTo(previous.target)
}

function clearMarks() {
  history.value = []
  tooEarly.value = null
  tooLate.value = null
}

function percent(ts: number) {
  return Math.max(0, Math.min(100, (ts - windowStart.value) / 3600 * 100))
}

function barStyle(range: Range) {
  const left = percent(range.from)
  return { left: `${left}%`, width: `${Math.max(0.25, percent(range.to) - left)}%` }
}

function timelineClick(event: MouseEvent) {
  const rect = (event.currentTarget as HTMLElement).getBoundingClientRect()
  seekTo(windowStart.value + Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)) * 3600)
}

function onPlayerTime(id: number, ts: number) {
  if (!Number.isFinite(ts)) return
  if (highlightsEnabled.value) {
    if (!highlightsAvailable.value) return
    if (id !== highlightDriver.value) return
    if (pendingHighlightSeek !== null) {
      if (Math.abs(ts - pendingHighlightSeek) > 2) return
      pendingHighlightSeek = null
    }
    viewedAt.value = ts
    advanceHighlights(ts)
  } else if (selected.value[0] === id) viewedAt.value = ts
}

function onGap(id: number, ts: number) {
  if (highlightsEnabled.value) {
    if (id !== highlightDriver.value || pendingHighlightSeek !== null) return
    // Coverage can end before the next motion interval. Continue on another
    // selected camera with coverage, or skip to the next recorded highlight.
    advanceHighlights(ts + 0.01, true)
    return
  }
  if (selected.value[0] !== id) return
  viewedAt.value = ts
  playing.value = false
  for (const player of players.values()) player.setPlaying(false)
}

function advanceHighlights(at: number, forceSeek = false) {
  if (!highlightsEnabled.value || !playing.value || !highlightsAvailable.value || pendingHighlightSeek !== null) return
  const next = nextHighlight(highlights.value, at)
  if (!next) {
    highlightStatus.value = highlights.value.length ? 'End of highlights in this hour.' : 'No recorded video activity in this hour.'
    playing.value = false
    for (const player of players.values()) player.setPlaying(false)
    return
  }
  const driverChanged = highlightDriver.value !== next.cameraId
  highlightDriver.value = next.cameraId
  highlightStatus.value = 'Skipping quiet periods · 2s lead-in / tail · selected hour only'
  if (at < next.from || driverChanged || forceSeek) {
    // Preserve fractional coverage boundaries; rounding down can seek into a gap.
    const target = Math.max(next.from, at)
    seekTo(target, true)
    pendingHighlightSeek = targetAt.value
    for (const player of players.values()) player.setPlaying(true)
  }
}

function setPlayer(id: number, instance: unknown) {
  if (instance) {
    players.set(id, instance as PlayerHandle)
    ;(instance as PlayerHandle).setRate(rate.value)
    ;(instance as PlayerHandle).setPlaying(playing.value)
  } else players.delete(id)
}

function togglePlayback() {
  playing.value = !playing.value
  for (const player of players.values()) player.setPlaying(playing.value)
}

function setPlaybackRate(next: number) {
  rate.value = next
  for (const player of players.values()) player.setRate(next)
}

async function loadTimeline() {
  const generation = ++fetchGeneration
  timelineError.value = ''
  if (!selected.value.length) { timeline.value = {}; loading.value = false; return }
  loading.value = true
  try {
    const entries = await Promise.all(selected.value.map(async id => {
      const [coverage, events] = await Promise.all([
        api<{ ranges: Range[] }>(`/dvr/coverage/${id}/${windowStart.value}/${windowEnd.value}`),
        api<{ clips: Activity[]; audio: AudioEvent[]; clipsTruncated: boolean; audioTruncated: boolean }>(`/dvr/events/${id}/${windowStart.value}/${windowEnd.value}`),
      ])
      return [id, { ranges: coverage.ranges ?? [], clips: events.clips ?? [], audio: events.audio ?? [], clipsTruncated: events.clipsTruncated, audioTruncated: events.audioTruncated }] as const
    }))
    if (generation === fetchGeneration) timeline.value = Object.fromEntries(entries)
  } catch {
    if (generation === fetchGeneration) timelineError.value = 'Could not load recording and activity timeline.'
  } finally {
    if (generation === fetchGeneration) loading.value = false
  }
}

watch([selected, windowStart], () => {
  pendingHighlightSeek = null
  highlightDriver.value = null
  loadTimeline()
})
watch([highlightsEnabled, highlightsAvailable, playing, targetAt], () => {
  if (!highlightsEnabled.value) {
    highlightDriver.value = null
    pendingHighlightSeek = null
    highlightStatus.value = ''
    for (const player of players.values()) player.setPlaying(playing.value)
  } else if (highlightsAvailable.value) {
    advanceHighlights(viewedAt.value)
    for (const player of players.values()) player.setPlaying(playing.value)
  } else {
    pendingHighlightSeek = null
    for (const player of players.values()) player.setPlaying(false)
  }
})

onMounted(async () => {
  if (!cameraStore.cameras.length) await cameraStore.fetchCameras()
  const cameraId = Number(route.query.camera)
  const at = Number(route.query.at)
  if (Number.isFinite(at) && at > 0) seekTo(at)
  if (Number.isInteger(cameraId) && cameraStore.cameras.some(c => c.id === cameraId)) selected.value = [cameraId]
})
</script>

<template>
  <AppLayout>
  <template #title>Recording workspace</template>
  <main class="dvr-workspace container-fluid py-3">
    <div class="dvr-header d-flex flex-wrap align-items-center justify-content-between gap-3 mb-4">
      <div>
        <div class="ui-eyebrow mb-2">REVIEW / REWIND / DISCOVER</div>
        <h1 class="h4 mb-1">DVR</h1>
        <p class="text-body-secondary mb-0">Your recordings, on your timeline. Choose cameras to begin.</p>
      </div>
      <div class="dvr-jump-controls d-flex flex-wrap align-items-end gap-2">
        <label class="small">Go to local time
          <input v-model="localInput" type="datetime-local" step="1" class="form-control form-control-sm" @change="setClock" @keyup.enter="setClock" />
        </label>
        <button class="btn btn-sm btn-outline-secondary" @click="move(-30)">−30s</button>
        <button class="btn btn-sm btn-outline-secondary" @click="move(-5)">−5s</button>
        <button class="btn btn-sm btn-outline-secondary" @click="move(5)">+5s</button>
        <button class="btn btn-sm btn-outline-secondary" @click="move(30)">+30s</button>
      </div>
    </div>

    <div class="dvr-panel dvr-playback-panel mb-3">
      <div class="d-flex flex-wrap align-items-center justify-content-between gap-2 mb-2">
        <div class="dvr-clock-block" :aria-label="viewedClock">
          <span class="ui-eyebrow">VIEWED TIME · LOCAL</span>
          <strong class="dvr-digital-clock">{{ format(new Date(viewedAt * 1000), 'HH:mm:ss') }}</strong>
          <span class="text-body-secondary small">{{ format(new Date(viewedAt * 1000), 'EEEE, dd MMM yyyy') }}</span>
        </div>
        <div class="dvr-transport d-flex flex-wrap align-items-center gap-2">
          <button class="btn btn-sm btn-outline-secondary" @click="shiftWindow(-3600)">Earlier hour</button>
          <button class="btn btn-sm btn-outline-secondary" @click="shiftWindow(3600)">Later hour</button>
          <button class="btn btn-sm btn-outline-primary" :disabled="!selected.length" @click="togglePlayback">{{ playing ? 'Pause' : 'Play' }} selected</button>
          <label class="dvr-highlights-toggle small d-flex align-items-center gap-2">
            <input v-model="highlightsEnabled" type="checkbox" class="form-check-input m-0" :disabled="!highlightsAvailable && !highlightsEnabled" />
            Highlights reel
          </label>
          <select :value="rate" aria-label="Playback speed" class="form-select form-select-sm dvr-speed" @change="setPlaybackRate(Number(($event.target as HTMLSelectElement).value))">
            <option :value="1">1×</option><option :value="2">2×</option><option :value="4">4×</option>
          </select>
        </div>
      </div>
      <p v-if="highlightsEnabled" class="small mt-2 mb-0" role="status">{{ !highlightsAvailable ? 'Highlights paused while activity is unavailable or incomplete. Choose a less dense hour.' : highlightStatus }}</p>
      <div class="dvr-camera-heading"><span class="ui-eyebrow">CAMERAS</span><span class="small text-body-secondary">{{ selected.length }} of {{ cameraStore.cameras.length }} selected · streams are opt-in</span></div>
      <div class="d-flex flex-wrap gap-2">
        <button v-for="camera in cameraStore.cameras" :key="camera.id" class="btn btn-sm dvr-camera-chip"
          :class="selected.includes(camera.id) ? 'btn-primary' : 'btn-outline-secondary'"
          :aria-pressed="selected.includes(camera.id)" @click="selectCamera(camera.id)"><span class="dvr-selection-dot" aria-hidden="true" />{{ camera.name }}</button>
        <button v-if="selected.length" class="btn btn-sm btn-outline-warning" @click="selected = []">All off</button>
      </div>
    </div>

    <div class="dvr-panel mb-3">
      <div class="d-flex flex-wrap align-items-center justify-content-between gap-2 mb-3">
        <div><strong>Timeline</strong><span v-if="selected.length && !loading && !timelineError" class="dvr-event-summary">{{ videoEventCount }} video · {{ audioEventCount }} audio events<span v-if="selected.some(id => timeline[id]?.clipsTruncated || timeline[id]?.audioTruncated)"> · partial results</span></span></div>
        <span class="small text-body-secondary">{{ format(new Date(windowStart * 1000), 'dd MMM HH:mm') }} – {{ format(new Date(windowEnd * 1000), 'HH:mm') }} · click to seek</span>
      </div>
      <p v-if="!selected.length" class="text-body-secondary small mb-0">Select a camera to see its recording coverage, video clips and audio events.</p>
      <p v-else-if="timelineError" class="text-warning small mb-0">{{ timelineError }}</p>
      <p v-else-if="loading" class="text-body-secondary small mb-0">Loading timeline…</p>
      <div v-if="selected.length" class="dvr-ruler-row" aria-hidden="true"><span /><div class="dvr-time-ruler"><span v-for="tick in timelineTicks" :key="tick.at">{{ tick.label }}</span></div></div>
      <div v-for="id in selected" :key="id" class="dvr-track-row">
        <span class="small text-truncate" :title="cameraStore.getCameraById(id)?.name ?? `Camera ${id}`">{{ cameraStore.getCameraById(id)?.name ?? `Camera ${id}` }}</span>
        <div class="dvr-track" role="button" tabindex="0" :aria-label="`Seek ${cameraStore.getCameraById(id)?.name ?? id}`" @click="timelineClick" @keydown.left.prevent="move(-5)" @keydown.right.prevent="move(5)">
          <div v-for="(range, i) in timeline[id]?.ranges ?? []" :key="`r${i}`" class="dvr-range dvr-coverage" :style="barStyle(range)" title="Recording available" />
          <div v-for="clip in timeline[id]?.clips ?? []" :key="`c${clip.id}`" class="dvr-range dvr-activity" :style="barStyle(clip)" title="Video activity" />
          <div v-for="(event, i) in timeline[id]?.audio ?? []" :key="`a${i}`" class="dvr-range dvr-audio" :style="barStyle(event)" :title="`${event.group} (${Math.round(event.score * 100)}%)`" />
          <div class="dvr-cursor" :style="{ left: `${percent(viewedAt)}%` }" />
          <div v-if="tooEarly !== null" class="dvr-bound" :style="{ left: `${percent(tooEarly)}%` }" />
          <div v-if="tooLate !== null" class="dvr-bound" :style="{ left: `${percent(tooLate)}%` }" />
        </div>
      </div>
      <p v-if="selected.some(id => timeline[id]?.clipsTruncated || timeline[id]?.audioTruncated)" class="text-warning small mb-0">Activity is dense in this hour; showing the first 500 events per type and camera.</p>
      <div v-if="selected.length" class="d-flex flex-wrap align-items-center gap-3 mt-2 small text-body-secondary">
        <span><i class="dvr-key dvr-coverage" /> Recording</span><span><i class="dvr-key dvr-activity" /> Video activity</span><span><i class="dvr-key dvr-audio" /> Audio event</span>
      </div>
    </div>

    <div class="dvr-panel dvr-search-panel mb-3">
      <div class="d-flex flex-wrap align-items-center gap-2">
        <div class="dvr-search-heading"><strong>Find when it happened</strong><span class="text-body-secondary small">Narrow the time range, one step at a time.</span></div>
        <button class="btn btn-sm btn-outline-secondary" @click="mark('early')">Too early</button>
        <button class="btn btn-sm btn-outline-secondary" @click="mark('late')">Too late</button>
        <button class="btn btn-sm btn-outline-secondary" :disabled="!history.length" @click="undoMark">Undo</button>
        <button class="btn btn-sm btn-outline-secondary" :disabled="!history.length" @click="clearMarks">Clear</button>
        <span class="small text-body-secondary">{{ remaining === null ? 'Mark each side to narrow the range.' : `${remaining}s remaining` }}</span>
      </div>
    </div>

    <div v-if="!selected.length" class="dvr-empty-state">
      <svg width="42" height="42" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.3" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 6v6l4 2"/></svg>
      <h2>Choose your cameras</h2>
      <p>Start with one camera above, then add others to compare.<br />No streams start until you select them.</p>
    </div>
    <div v-else class="dvr-player-grid">
      <DvrPlayer v-for="id in selected" :key="id" :ref="el => setPlayer(id, el)" :camera-id="id"
        :camera-name="cameraStore.getCameraById(id)?.name ?? `Camera ${id}`" :from="windowStart" :to="windowEnd"
        :start-at="targetAt" compact @time="ts => onPlayerTime(id, ts)" @gap="ts => onGap(id, ts)" />
    </div>
  </main>
  </AppLayout>
</template>

<style scoped>
.dvr-workspace { max-width: 1600px; padding-inline: 0; }
.dvr-header h1 { font-size: 2rem; font-weight: 650; letter-spacing: -.04em; }
.dvr-jump-controls { max-width: 100%; }
.dvr-jump-controls input { min-height: 36px; max-width: 100%; }
.dvr-panel { padding: 1.25rem; border: 1px solid #303d50; border-radius: .85rem; background: #151e2b; color: #e7edf6; box-shadow: 0 6px 20px #00000018; }
.dvr-playback-panel { background: linear-gradient(115deg, #1b2a3d, #151e2b 65%); border-top: 2px solid #65b9ee; }
.dvr-clock-block { display: flex; flex-direction: column; gap: .2rem; margin-bottom: .5rem; }
.dvr-digital-clock { font-size: clamp(2rem, 4vw, 3rem); font-weight: 550; font-variant-numeric: tabular-nums; line-height: 1.15; letter-spacing: -.025em; }
.dvr-highlights-toggle { border: 1px solid #384a61; border-radius: .5rem; padding: .5rem .65rem; background: #101927; }
.dvr-highlights-toggle:has(input:checked) { color: #a7d7ff; border-color: #589bc7; }
.dvr-camera-heading { display: flex; justify-content: space-between; flex-wrap: wrap; gap: .5rem; margin: 1.1rem 0 .65rem; padding-top: 1rem; border-top: 1px solid #34435a; }
.dvr-camera-chip { display: inline-flex; align-items: center; gap: .5rem; padding: .45rem .75rem; border-radius: .5rem; }
.dvr-selection-dot { width: 6px; height: 6px; border: 1px solid currentColor; border-radius: 50%; flex-shrink: 0; }
.dvr-camera-chip[aria-pressed="true"] .dvr-selection-dot { background: currentColor; }
.dvr-event-summary { margin-left: .75rem; font-size: .75rem; color: #abc2dd; }
.dvr-speed { width: 5rem; }
.dvr-track-row, .dvr-ruler-row { display: grid; grid-template-columns: 10rem minmax(0, 1fr); gap: .8rem; align-items: center; margin: .6rem 0; }
.dvr-time-ruler { display: flex; justify-content: space-between; font-size: .65rem; font-variant-numeric: tabular-nums; color: #a4b6ce; }
.dvr-track { height: 2.6rem; position: relative; cursor: crosshair; border-radius: .35rem; border: 1px solid #50617a; background: #0d1521; overflow: hidden; }
.dvr-track::after { content: ''; position: absolute; inset: 0; background: repeating-linear-gradient(90deg, transparent, transparent calc(16.666% - 1px), #ffffff22 calc(16.666% - 1px), #ffffff22 16.666%); pointer-events: none; }
.dvr-track:focus-visible { outline: 2px solid #69bfff; outline-offset: 2px; }
.dvr-range { position: absolute; }
.dvr-coverage { top: 0; height: 100%; background: #397a9e; }
.dvr-activity { top: 0; height: 40%; background: #e99a39; }
.dvr-audio { bottom: 0; height: 35%; background: #8a6bd6; }
.dvr-cursor { position: absolute; top: 0; bottom: 0; width: 2px; background: #fff; box-shadow: 0 0 0 1px #000; pointer-events: none; z-index: 2; }
.dvr-bound { position: absolute; top: 0; bottom: 0; width: 1px; background: #fa5e5e; pointer-events: none; }
.dvr-key { display: inline-block; width: .8rem; height: .8rem; position: static; vertical-align: middle; }
.dvr-player-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 360px), 1fr)); gap: .7rem; }
.dvr-search-heading { display: flex; flex-direction: column; gap: .2rem; margin-right: auto; }
.dvr-empty-state { display: flex; flex-direction: column; align-items: center; text-align: center; padding: 2rem 1rem; border: 1px dashed #35445b; border-radius: .85rem; background: #101824; }
.dvr-empty-state svg { color: #79b8ec; margin-bottom: .8rem; }
.dvr-empty-state h2 { font-size: 1.1rem; margin-bottom: .4rem; }
.dvr-empty-state p { color: #a5b4c8; font-size: .85rem; margin-bottom: 0; }
@media (max-width: 600px) {
  .dvr-panel { padding: 1rem; }
  .dvr-track-row, .dvr-ruler-row { grid-template-columns: 5.5rem minmax(0, 1fr); gap: .5rem; }
  .dvr-time-ruler span:nth-child(even) { visibility: hidden; }
  .dvr-search-heading { flex-basis: 100%; margin-bottom: .5rem; }
  .dvr-jump-controls { width: 100%; }
  .dvr-jump-controls > label { width: 100%; }
  .dvr-jump-controls input { width: 100%; }
  .dvr-jump-controls > button { flex: 1; }
  .dvr-transport { width: 100%; }
  .dvr-event-summary { display: block; margin: .25rem 0 0; }
}
</style>
