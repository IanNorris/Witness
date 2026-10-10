<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { format } from 'date-fns'
import DvrPlayer from './DvrPlayer.vue'
import { api } from '../../composables/useApi'

const props = defineProps<{ cameraId: number; cameraName: string }>()
const emit = defineEmits<{ close: [] }>()
interface Range { from: number; to: number }
interface PendingRange extends Range { targetSeconds: number }
const anchor = Math.floor(Date.now() / 1000)
const from = Math.max(0, anchor - 30 * 60)
const viewedAt = ref(anchor - 5 * 60)
const requestedAt = ref(viewedAt.value)
const lookback = ref(300)
const player = ref<InstanceType<typeof DvrPlayer> | null>(null)
const modal = ref<HTMLElement | null>(null)
const coverage = ref<Range[]>([])
const pending = ref<PendingRange[]>([])
const coverageError = ref('')
const selectedPending = computed(() => pending.value.some(r => viewedAt.value >= r.from && viewedAt.value < r.to))
const savingMinutes = computed(() => Math.ceil((pending.value.find(r => viewedAt.value >= r.from && viewedAt.value < r.to)?.targetSeconds ?? 60) / 60))
const viewedTime = computed(() => format(new Date(viewedAt.value * 1000), 'HH:mm:ss'))
let previousFocus: HTMLElement | null = null
let timer: ReturnType<typeof setInterval> | undefined
let disposed = false
let fetching = false

function seekTo(timestamp: number) {
  viewedAt.value = Math.max(from, Math.min(anchor, timestamp))
  requestedAt.value = viewedAt.value
  // Calling explicitly permits retrying the exact same timestamp after a file finishes.
  player.value?.seekToTimestamp(viewedAt.value)
}
function jumpBack() { seekTo(anchor - Number(lookback.value)) }
async function refreshReview() {
  await refreshCoverage()
  player.value?.refreshSegments()
}
function rangeStyle(range: Range) {
  const start = Math.max(from, range.from), end = Math.min(anchor, range.to)
  return { left: `${(start - from) / (anchor - from) * 100}%`, width: `${Math.max(0, end - start) / (anchor - from) * 100}%` }
}
async function refreshCoverage() {
  if (fetching || disposed || document.hidden) return
  fetching = true
  try {
    const result = await api<{ ranges: Range[]; pending?: PendingRange[] }>(`/dvr/coverage/${props.cameraId}/${from}/${anchor}`)
    if (disposed) return
    coverage.value = result.ranges ?? []
    pending.value = result.pending ?? []
    coverageError.value = ''
  } catch {
    if (!disposed) coverageError.value = 'Recording availability could not be refreshed.'
  } finally { fetching = false }
}
function keydown(event: KeyboardEvent) {
  event.stopPropagation()
  if (event.key === 'Escape') { event.preventDefault(); emit('close'); return }
  if (event.key === 'Tab' && modal.value) {
    const targets = [...modal.value.querySelectorAll<HTMLElement>('button:not(:disabled), select, input, [tabindex="0"]')]
    const first = targets[0], last = targets[targets.length - 1]
    if (event.shiftKey && (document.activeElement === first || document.activeElement === modal.value)) { event.preventDefault(); last?.focus() }
    else if (!event.shiftKey && (document.activeElement === last || document.activeElement === modal.value)) { event.preventDefault(); first?.focus() }
    return
  }
  if ((event.target as HTMLElement)?.matches('input, select, button')) return
  if (event.key === ' ') { event.preventDefault(); player.value?.togglePlay() }
  if (event.key === 'ArrowLeft') { event.preventDefault(); seekTo(viewedAt.value - 5) }
  if (event.key === 'ArrowRight') { event.preventDefault(); seekTo(viewedAt.value + 5) }
}
onMounted(() => {
  previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
  modal.value?.focus()
  refreshCoverage()
  timer = setInterval(refreshCoverage, 15_000)
})
onUnmounted(() => {
  disposed = true
  clearInterval(timer)
  if (previousFocus?.isConnected) previousFocus.focus()
})
</script>

<template>
  <Teleport to="body">
    <div ref="modal" class="recent-replay-backdrop" role="dialog" aria-modal="true"
      :aria-label="`Recent replay: ${cameraName}`" tabindex="0" @keydown="keydown" @click.self="emit('close')">
      <section class="recent-replay-panel">
        <header class="recent-replay-header">
          <div><span class="recent-replay-eyebrow">RECENT REPLAY</span><h2>{{ cameraName }}</h2></div>
          <button class="btn btn-sm btn-primary" @click="emit('close')">Return to live ×</button>
        </header>
        <div class="recent-replay-tools">
          <label>Show this camera
            <select v-model="lookback" class="form-select form-select-sm" @change="jumpBack">
              <option :value="60">1 minute ago</option><option :value="300">5 minutes ago</option>
              <option :value="600">10 minutes ago</option><option :value="1800">30 minutes ago</option>
            </select>
          </label>
          <span class="recent-replay-clock">{{ viewedTime }}</span>
          <button class="btn btn-sm btn-outline-secondary" @click="refreshReview">Refresh availability</button>
        </div>
        <DvrPlayer ref="player" :camera-id="cameraId" :camera-name="cameraName" :from="from" :to="anchor"
          hide-header external-timeline :pending-save="selectedPending"
          :start-at="requestedAt" @time="viewedAt = $event" @close="emit('close')" />
        <div class="recent-replay-timeline">
          <div class="recent-replay-track" aria-hidden="true">
            <span v-for="(range, i) in coverage" :key="`r${i}`" class="recent-replay-range" :style="rangeStyle(range)" />
            <span v-for="(range, i) in pending" :key="`p${i}`" class="recent-replay-range saving" :style="rangeStyle(range)" />
          </div>
          <input type="range" :min="from" :max="anchor" step="1" :value="viewedAt" aria-label="Replay clock time"
            @change="seekTo(Number(($event.target as HTMLInputElement).value))" />
          <div class="recent-replay-labels"><span>{{ format(new Date(from * 1000), 'HH:mm') }}</span><span>Opened at {{ format(new Date(anchor * 1000), 'HH:mm:ss') }}</span></div>
        </div>
        <p v-if="selectedPending" class="recent-replay-notice" role="status">Currently saving — this time is not playable yet. Files normally finish after about {{ savingMinutes }} minute(s), at the next keyframe. Refresh and try again once it becomes available.</p>
        <p v-if="coverageError" class="recent-replay-notice" role="status">{{ coverageError }}</p>
        <footer>Blue: available · Hatched: currently saving · Blank: no recording. Live cameras stay running behind this replay.</footer>
      </section>
    </div>
  </Teleport>
</template>

<style scoped>
.recent-replay-backdrop { position: fixed; inset: 0; z-index: 2100; background: #07101bd9; backdrop-filter: blur(8px); display: grid; place-items: center; padding: 20px; overflow: auto; }
.recent-replay-panel { width: min(100%, 1050px); max-height: calc(100dvh - 40px); overflow: auto; border: 1px solid #47617a; border-radius: 16px; background: #111d2b; color: #e7eff8; box-shadow: 0 24px 80px #0008; }
.recent-replay-header, .recent-replay-tools { display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 16px 20px; flex-wrap: wrap; }
.recent-replay-header { border-bottom: 1px solid #34485d; }
.recent-replay-header h2 { margin: 3px 0 0; font-size: 1.3rem; }
.recent-replay-eyebrow { font-size: .65rem; letter-spacing: .16em; color: #8bb9dd; }
.recent-replay-tools label { display: flex; gap: 10px; align-items: center; font-size: .85rem; }
.recent-replay-tools select { width: auto; }
.recent-replay-clock { font: 1.3rem ui-monospace, monospace; font-variant-numeric: tabular-nums; }
.recent-replay-timeline { padding: 16px 20px 8px; }
.recent-replay-track { height: 12px; position: relative; background: #080e16; border-radius: 4px; overflow: hidden; }
.recent-replay-range { position: absolute; height: 100%; background: #397a9e; }
.recent-replay-range.saving { background: repeating-linear-gradient(135deg, #b68a3440 0 4px, #b68a3490 4px 7px); }
.recent-replay-timeline input { width: 100%; accent-color: #67b5ff; }
.recent-replay-labels { display: flex; justify-content: space-between; font-size: .75rem; color: #b1c3d4; }
.recent-replay-notice { margin: 8px 20px; padding: 10px 12px; background: #b68a3420; border: 1px solid #b68a3460; border-radius: 8px; font-size: .85rem; }
footer { padding: 12px 20px 18px; color: #b1c3d4; font-size: .75rem; }
</style>
