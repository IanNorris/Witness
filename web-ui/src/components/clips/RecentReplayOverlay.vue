<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { format } from 'date-fns'
import DvrPlayer from './DvrPlayer.vue'
import { api } from '../../composables/useApi'

const props = defineProps<{ cameraId: number; cameraName: string; captureOnly?: boolean; startAt?: number }>()
const emit = defineEmits<{ close: [] }>()
interface Range { from: number; to: number }
interface PendingRange extends Range { targetSeconds: number }
interface CaptureJob {
  id: string; state: 'recording' | 'currentlySaving' | 'exporting' | 'completed' | 'failed' | 'cancelled'
  from: number; to: number; actualFrom: number; actualTo: number; error: string; gaps: Range[]
}
const capture = ref<CaptureJob | null>(null)
const captureError = ref('')
const captureBusy = ref(false)
const captureKey = `witness-recent-capture-${props.cameraId}`
const captureActive = computed(() => capture.value && ['recording', 'currentlySaving', 'exporting'].includes(capture.value.state))
const captureLabel = computed(() => {
  switch (capture.value?.state) {
    case 'recording': return 'Recording — continues after closing this replay. Stop here when finished (six-hour maximum).'
    case 'currentlySaving': return 'Waiting for DVR footage to finish saving…'
    case 'exporting': return 'Preparing your clip…'
    case 'completed': return capture.value.gaps.length ? 'Clip ready — some requested footage was unavailable. See gaps below.' : 'Clip ready.'
    case 'failed': return `Capture failed: ${capture.value.error}`
    case 'cancelled': return 'Capture cancelled.'
    default: return ''
  }
})
const anchor = Math.floor(Date.now() / 1000)
const from = Math.max(0, Math.min(anchor - 30 * 60, props.startAt ?? anchor))
const viewedAt = ref(props.startAt ?? anchor - 5 * 60)
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
let captureTimer: ReturnType<typeof setInterval> | undefined
let pollingCapture = false
let captureRevision = 0
let disposed = false
let fetching = false

function rememberCapture(job: CaptureJob) {
  capture.value = job
  try { localStorage.setItem(captureKey, job.id) } catch { /* Storage can be disabled. */ }
}
async function refreshCapture() {
  if (disposed || pollingCapture || captureBusy.value || document.hidden) return
  let id = capture.value?.id
  try { id ??= localStorage.getItem(captureKey) ?? undefined } catch { /* Optional persistence. */ }
  if (!id || (capture.value && !captureActive.value)) return
  pollingCapture = true
  const revision = captureRevision
  try {
    const job = await api<CaptureJob>(`/dvr/capture/${props.cameraId}/${encodeURIComponent(id)}/status`, { redirectOnFail: false })
    if (!disposed && revision === captureRevision) { rememberCapture(job); captureError.value = '' }
  } catch {
    if (!disposed && revision === captureRevision) captureError.value = 'Capture status unavailable. The server may have restarted; retry before starting another recording.'
  } finally { pollingCapture = false }
}
async function startCapture(continuing: boolean) {
  if (captureBusy.value || captureActive.value) return
  captureBusy.value = true; captureError.value = ''
  ++captureRevision
  try {
    const job = await api<CaptureJob>(`/dvr/capture/${props.cameraId}`, {
      method: 'POST', body: { from: viewedAt.value, continueRecording: continuing }, redirectOnFail: false,
    })
    if (!disposed) rememberCapture(job)
    else try { localStorage.setItem(captureKey, job.id) } catch { /* The server job is independent. */ }
  } catch (error) {
    if (!disposed) captureError.value = error instanceof Error ? error.message : 'Capture could not be started.'
  } finally { captureBusy.value = false }
}
async function captureAction(action: 'stop' | 'cancel') {
  if (!capture.value || captureBusy.value) return
  captureBusy.value = true
  ++captureRevision
  try {
    const job = await api<CaptureJob>(`/dvr/capture/${props.cameraId}/${capture.value.id}/${action}`, {
      method: 'POST', body: {}, redirectOnFail: false,
    })
    if (!disposed) { rememberCapture(job); captureError.value = '' }
  } catch (error) {
    if (!disposed) captureError.value = error instanceof Error ? error.message : 'Capture update failed.'
  } finally { captureBusy.value = false }
}

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
    const targets = [...modal.value.querySelectorAll<HTMLElement>('button:not(:disabled), a[href], select, input, [tabindex="0"]')]
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
  if (!props.captureOnly) {
    refreshCoverage()
    timer = setInterval(refreshCoverage, 15_000)
  }
  refreshCapture()
  captureTimer = setInterval(refreshCapture, 3000)
})
onUnmounted(() => {
  disposed = true
  clearInterval(timer)
  clearInterval(captureTimer)
  if (previousFocus?.isConnected) previousFocus.focus()
})
</script>

<template>
  <Teleport to="body">
    <div ref="modal" class="recent-replay-backdrop" role="dialog" aria-modal="true"
      :aria-label="`Recent replay: ${cameraName}`" tabindex="0" @keydown="keydown" @click.self="emit('close')">
      <section class="recent-replay-panel">
        <header class="recent-replay-header">
          <div><span class="recent-replay-eyebrow">{{ captureOnly ? 'SAVE RECENT FOOTAGE' : 'RECENT REPLAY' }}</span><h2>{{ cameraName }}</h2></div>
          <button class="btn btn-sm btn-primary" @click="emit('close')">{{ captureOnly ? 'Close ×' : 'Return to live ×' }}</button>
        </header>
        <div v-if="!captureOnly" class="recent-replay-tools">
          <label>Show this camera
            <select v-model="lookback" class="form-select form-select-sm" @change="jumpBack">
              <option :value="60">1 minute ago</option><option :value="300">5 minutes ago</option>
              <option :value="600">10 minutes ago</option><option :value="1800">30 minutes ago</option>
            </select>
          </label>
          <span class="recent-replay-clock">{{ viewedTime }}</span>
          <button class="btn btn-sm btn-outline-secondary" @click="refreshReview">Refresh availability</button>
        </div>
        <DvrPlayer v-if="!captureOnly" ref="player" :camera-id="cameraId" :camera-name="cameraName" :from="from" :to="anchor"
          hide-header external-timeline :pending-save="selectedPending"
          :start-at="requestedAt" @time="viewedAt = $event" @close="emit('close')" />
        <div v-if="!captureOnly" class="recent-replay-timeline">
          <div class="recent-replay-track" aria-hidden="true">
            <span v-for="(range, i) in coverage" :key="`r${i}`" class="recent-replay-range" :style="rangeStyle(range)" />
            <span v-for="(range, i) in pending" :key="`p${i}`" class="recent-replay-range saving" :style="rangeStyle(range)" />
          </div>
          <input type="range" :min="from" :max="anchor" step="1" :value="viewedAt" aria-label="Replay clock time"
            @change="seekTo(Number(($event.target as HTMLInputElement).value))" />
          <div class="recent-replay-labels"><span>{{ format(new Date(from * 1000), 'HH:mm') }}</span><span>Opened at {{ format(new Date(anchor * 1000), 'HH:mm:ss') }}</span></div>
        </div>
        <div v-if="captureOnly" class="recent-replay-tools">
          <label>Save from
            <select class="form-select form-select-sm" @change="viewedAt = anchor - Number(($event.target as HTMLSelectElement).value)">
              <option :value="anchor - viewedAt">{{ viewedTime }} · selected time</option>
              <option value="30">30 seconds ago</option><option value="60">1 minute ago</option>
              <option value="120">2 minutes ago</option><option value="300">5 minutes ago</option>
            </select>
          </label>
        </div>
        <p v-if="selectedPending && !captureOnly" class="recent-replay-notice" role="status">Currently saving — this time is not playable yet. Files normally finish after about {{ savingMinutes }} minute(s), at the next keyframe. Refresh and try again once it becomes available.</p>
        <p v-if="coverageError" class="recent-replay-notice" role="status">{{ coverageError }}</p>
        <section class="recent-capture" aria-label="Save recent footage">
          <div class="recent-capture-actions">
            <button class="btn btn-sm btn-primary" :disabled="captureBusy || !!captureActive" @click="startCapture(false)">Save from {{ viewedTime }} to now</button>
            <button class="btn btn-sm btn-outline-info" :disabled="captureBusy || !!captureActive" @click="startCapture(true)">Record from {{ viewedTime }}</button>
            <button v-if="capture?.state === 'recording'" class="btn btn-sm btn-warning" :disabled="captureBusy" @click="captureAction('stop')">Stop this capture now</button>
            <button v-if="captureActive" class="btn btn-sm btn-outline-secondary" :disabled="captureBusy" @click="captureAction('cancel')">Cancel capture</button>
            <a v-if="capture?.state === 'completed'" class="btn btn-sm btn-success" :href="`/dvr/capture/${cameraId}/${capture.id}/download`">Download clip</a>
          </div>
          <p v-if="captureLabel" role="status">{{ captureLabel }}</p>
          <p v-if="captureError" role="alert">{{ captureError }} <button class="btn btn-sm btn-outline-secondary" @click="refreshCapture">Retry status</button></p>
          <p v-if="capture?.state === 'completed'">Media: {{ format(new Date(capture.actualFrom * 1000), 'HH:mm:ss') }}–{{ format(new Date(capture.actualTo * 1000), 'HH:mm:ss') }}</p>
          <p v-if="capture?.state === 'completed' && capture.error" role="status">{{ capture.error }}</p>
          <p v-for="(gap, i) in capture?.gaps ?? []" :key="i">Unavailable: {{ format(new Date(gap.from * 1000), 'HH:mm:ss') }}–{{ format(new Date(gap.to * 1000), 'HH:mm:ss') }}</p>
          <small>Starts at a preceding keyframe; the end may include a later reference frame. Your existing camera recordings are unchanged.</small>
        </section>
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
.recent-capture { margin: 12px 20px; padding: 14px; border: 1px solid #34485d; border-radius: 10px; background: #0c1622; }
.recent-capture-actions { display: flex; flex-wrap: wrap; gap: 8px; }
.recent-capture p { margin: 10px 0 0; font-size: .85rem; }
.recent-capture small { display: block; margin-top: 10px; color: #b1c3d4; }
</style>
