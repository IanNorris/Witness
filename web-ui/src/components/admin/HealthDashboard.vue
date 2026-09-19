<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { api } from '../../composables/useApi'
import { openHealthMonitor } from '../../composables/useHealthMonitor'
import {
  collectClientHealthSessions,
  type ClientHealthSession,
  type ClientPlayerHealth,
} from '../../composables/useClientHealth'

defineProps<{ standalone?: boolean }>()

interface StreamHealth {
  tier: string
  available: boolean
  state: string
  videoCodec?: string
  audioCodec?: string
  width?: number
  height?: number
  retainedSegments?: number
  acceptedVideoPackets?: number
  droppedVideoPackets?: number
  corruptVideoPackets?: number
  repairedVideoTimestamps?: number
	streamEstablished?: boolean
	startupGraceElapsedMs?: number
	startupAcceptedVideoPackets?: number
	startupDroppedVideoPackets?: number
	startupRepairedVideoTimestamps?: number
	establishedAcceptedVideoPackets?: number
	establishedDroppedVideoPackets?: number
	establishedRepairedVideoTimestamps?: number
  missingVideoDtsPackets?: number
  audioVideoSkewMs?: number | null
  maxSegmentDriftMs?: number
  initStructureValid?: boolean | null
  packetDispositionCounts?: Record<string, number>
  recentMediaEvents?: MediaDiagnosticEvent[]
}

interface MediaDiagnosticEvent {
  sequence: number
  activityId: number
  packetSequence: number
  timestampUnixMs: number
  elapsedMs: number
  generation: number
  segmentIndex: number
  partialIndex: number
  category: string
  severity: string
  phase: string
  component: string
  message: string
  disposition?: string
  audio: boolean
  keyframe: boolean
  corrupt: boolean
  packetSize: number
  sourceDtsUs?: number | null
  sourcePtsUs?: number | null
}

interface DisplayMediaDiagnosticEvent extends MediaDiagnosticEvent {
  tier: string
}

interface ProcessingHealth {
  pendingEssential?: number
  pendingAI?: number
  oldestPendingEssentialMs?: number
  oldestPendingAIMs?: number
  activeJobMs?: number
  ingressWaitMeanMs?: number | null
  ingressWaitMaxMs?: number | null
  coalescedFrames?: number
  coalescedAIFrames?: number
}

interface CameraHealth {
  cameraId: number
  name: string
  state: string
  processing?: ProcessingHealth
  streams: StreamHealth[]
}

interface HealthSnapshot {
  schemaVersion: number
  sampledAtUtc: string
  collectionDurationMs: number
  server: {
    instanceId: string
    webBuildHash: string
    collectionMode: string
    uptimeMs: number
    host: {
      logicalProcessors: number
      totalMemoryBytes: number | null
      availableMemoryBytes: number | null
      memoryLoadPercent: number | null
      processWorkingSetBytes: number | null
      processPrivateBytes: number | null
    }
    audioIntelligence?: {
      workerLoaded: boolean
      clipsProcessed: number
      eventsProduced: number
      decodeFailures: number
      inferenceFailures: number
      lastClipUID: number
      lastInferenceMs: number
      meanInferenceMs: number
    }
  }
  cameras: CameraHealth[]
  coverage: Record<string, unknown>
}

const snapshot = ref<HealthSnapshot | null>(null)
const clientSessions = ref<ClientHealthSession[]>([])
const clientCoverage = ref('Not collected')
const loading = ref(false)
const error = ref('')
const copied = ref(false)
const copyFailed = ref(false)
const expandedCameraId = ref<number | null>(null)
const nowMonotonicMs = ref(performance.now())
const snapshotReceivedAtMonotonicMs = ref<number | null>(null)
const clientBuildHash = __BUILD_HASH__
let ageTimer: ReturnType<typeof setInterval> | null = null

async function refresh() {
  loading.value = true
  error.value = ''
  try {
    const clientRequest = collectClientHealthSessions()
      .then(collection => ({ collection, error: '' }))
      .catch(err => ({
        collection: null,
        error: err instanceof Error ? err.message : String(err),
      }))
    snapshot.value = await api<HealthSnapshot>('/debug/health')
    snapshotReceivedAtMonotonicMs.value = performance.now()
    nowMonotonicMs.value = snapshotReceivedAtMonotonicMs.value
    const clients = await clientRequest
    clientSessions.value = clients.collection?.sessions ?? []
    if (clients.error || !clients.collection) {
      clientCoverage.value = `Unavailable: ${clients.error || 'collection failed'}`
    } else {
      const collection = clients.collection
      const truncated = collection.sessions.filter(session => session.playersTruncated).length
      const truncatedLabel = truncated
        ? ` · ${truncated} tab${truncated === 1 ? '' : 's'} truncated at 64 players`
        : ''
      clientCoverage.value = collection.mode === 'broadcast'
        ? `${collection.sessions.length} browser tab${collection.sessions.length === 1 ? '' : 's'} replied within ${collection.waitMs} ms${truncatedLabel}`
        : `Current tab only (${collection.mode.replace(/-/g, ' ')})${truncatedLabel}`
    }
  } catch (err) {
    error.value = err instanceof Error ? err.message : String(err)
  } finally {
    loading.value = false
  }
}

function formatBytes(value: number | null | undefined): string {
  if (value == null) return 'Unknown'
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']
  let amount = value
  let unit = 0
  while (amount >= 1024 && unit < units.length - 1) {
    amount /= 1024
    unit++
  }
  return `${amount.toFixed(unit >= 2 ? 1 : 0)} ${units[unit]}`
}

function formatMs(value: number | null | undefined): string {
  return value == null ? '—' : `${value.toFixed(value < 10 ? 1 : 0)} ms`
}

function streamLabel(stream: StreamHealth): string {
  const dimensions = stream.width && stream.height ? ` ${stream.width}×${stream.height}` : ''
  return `${(stream.videoCodec || 'unknown').toUpperCase()}${dimensions}`
}

function streamTierLabel(tier: string): string {
  if (tier === 'preview' || tier === 'sub') return 'Preview'
  if (tier === 'main') return 'Main'
  return 'Unknown'
}

function readyStateLabel(value: number | undefined): string {
  return ['No media', 'Metadata', 'Current frame', 'Future frames', 'Buffered'][value ?? -1] ?? 'Unknown'
}

function websocketStateLabel(value: number | null | undefined): string {
  if (value == null) return 'WS ?'
  return ['WS connecting', 'WS open', 'WS closing', 'WS closed'][value] ?? `WS ${value}`
}

function playerBufferClue(player: ClientPlayerHealth): string {
  const parts: string[] = []
  parts.push(player.mediaSourceState ? `MSE ${player.mediaSourceState}` : 'MSE ?')
  parts.push(websocketStateLabel(player.wsReadyState))
  if (player.reconnectPendingMs != null) parts.push(`reconnect ${formatMs(player.reconnectPendingMs)}`)
  if (player.wsOpenAgeMs != null) parts.push(`open ${formatMs(player.wsOpenAgeMs)}`)
  if (player.lastFragAge != null) parts.push(`last frag ${formatMs(player.lastFragAge)}`)
  if (player.lastAppendAgeMs != null) parts.push(`last append ${formatMs(player.lastAppendAgeMs)}`)
  if (player.awaitingInit) parts.push('awaiting init')
  if (player.waitingForKeyframe) parts.push('waiting keyframe')
  if (player.appendQueueLength != null && player.appendQueueLength > 0) {
    const bytes = player.appendQueueBytes != null ? ` / ${formatBytes(player.appendQueueBytes)}` : ''
    const age = player.appendQueueOldestAgeMs != null ? ` / ${formatMs(player.appendQueueOldestAgeMs)}` : ''
    parts.push(`append q ${player.appendQueueLength}${bytes}${age}`)
  }
  if (player.sourceBufferUpdating) {
    const age = player.sourceBufferOperationAgeMs != null ? ` ${formatMs(player.sourceBufferOperationAgeMs)}` : ''
    parts.push(`appending ${player.sourceBufferOperation ?? ''}${age}`.trim())
  }
  if (player.hasInitialBuffer === false) parts.push('no initial buffer')
  return parts.join(' · ')
}

function playerRestartClue(player: ClientPlayerHealth): string {
  const reason = player.lastRestartReason || player.lastEventType
  return reason ? ` · ${reason}` : ''
}

function playerLabel(id: string): string {
  const cameraId = Number.parseInt(id, 10)
  const camera = snapshot.value?.cameras.find(candidate => candidate.cameraId === cameraId)
  return camera ? `${camera.name} (#${cameraId})` : id.replace(/_mse$/i, '')
}

function formatCount(value: number | undefined): string {
  return value == null ? '—' : value.toLocaleString()
}

function steadyCount(stream: StreamHealth, establishedKey: keyof StreamHealth, _lifetimeKey: keyof StreamHealth): number | undefined {
	if (!stream.streamEstablished) return undefined
	const value = stream[establishedKey]
  return typeof value === 'number' ? value : undefined
}

const dispositionLabels: Record<string, string> = {
  waitingForKeyframe: 'waiting for keyframe',
  missingTimestamp: 'missing timestamp',
  beforeVideoEpoch: 'before video epoch',
  negativeTimestamp: 'negative timestamp',
  nonMonotonicInput: 'non-monotonic input',
  noMuxBuffer: 'no mux buffer',
  nonMonotonicOutput: 'non-monotonic output',
  muxError: 'mux error',
  decodeCorruption: 'decode suppression',
  decodeRecovery: 'decode recovery',
}

function dropReasonSummary(stream: StreamHealth): string {
  return Object.entries(stream.packetDispositionCounts ?? {})
    .filter(([, count]) => count > 0)
    .sort((left, right) => right[1] - left[1])
    .map(([reason, count]) => `${count.toLocaleString()} ${dispositionLabels[reason] ?? reason}`)
    .join(' · ')
}

function cameraMediaEvents(camera: CameraHealth): DisplayMediaDiagnosticEvent[] {
  return camera.streams
    .flatMap(stream => (stream.recentMediaEvents ?? []).map(event => ({ ...event, tier: stream.tier })))
    .sort((left, right) => right.timestampUnixMs - left.timestampUnixMs || right.sequence - left.sequence)
}

function mediaEventTime(timestampUnixMs: number): string {
  const date = new Date(timestampUnixMs)
  const pad = (value: number, length = 2) => String(value).padStart(length, '0')
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.${pad(date.getMilliseconds(), 3)}`
}

function mediaEventClass(severity: string): string {
  if (severity === 'error') return 'event-error'
  if (severity === 'warning') return 'event-warning'
  return 'event-info'
}

function toggleCameraEvents(cameraId: number) {
  expandedCameraId.value = expandedCameraId.value === cameraId ? null : cameraId
}

function stateClass(state: string): string {
  const normalized = state.toLowerCase()
  if (normalized === 'connected' || normalized === 'healthy') return 'bg-success'
  if (['starting', 'connecting', 'reconnecting'].includes(normalized)) return 'bg-warning text-dark'
  if (['disconnected', 'failed', 'error', 'unhealthy'].includes(normalized)) return 'bg-danger'
  return 'bg-secondary'
}

const memoryUsed = computed(() => {
  const host = snapshot.value?.server.host
  if (!host?.totalMemoryBytes || host.availableMemoryBytes == null) return null
  return host.totalMemoryBytes - host.availableMemoryBytes
})

const snapshotAge = computed(() => {
  if (!snapshot.value || snapshotReceivedAtMonotonicMs.value == null) {
    return { label: 'No snapshot', className: 'bg-secondary' }
  }
  const seconds = Math.max(0, Math.floor((nowMonotonicMs.value - snapshotReceivedAtMonotonicMs.value) / 1000))
  if (seconds < 5) return { label: 'Just now', className: 'bg-success' }
  if (seconds < 60) return { label: `${seconds}s ago`, className: 'bg-secondary' }
  const minutes = Math.floor(seconds / 60)
  return {
    label: `${minutes}m ago`,
    className: minutes >= 5 ? 'bg-danger' : 'bg-warning text-dark',
  }
})

function combinedExport() {
  if (!snapshot.value) return
  const data = {
    schemaVersion: 1,
    exportedAtUtc: new Date().toISOString(),
    server: snapshot.value,
    client: {
      buildHash: __BUILD_HASH__,
      userAgent: navigator.userAgent,
      visibilityState: document.visibilityState,
      sessions: clientSessions.value,
      coverage: clientCoverage.value,
    },
  }
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const anchor = document.createElement('a')
  anchor.href = url
  anchor.download = `witness-health-${new Date().toISOString().replace(/[:.]/g, '-')}.json`
  anchor.click()
  URL.revokeObjectURL(url)
}

async function copyTable() {
  if (!snapshot.value) return
  const header = [
    'Camera', 'State', 'Tier', 'Codec', 'Resolution', 'Pending essential',
	'Oldest essential ms', 'Retained segments', 'Steady-state dropped packets',
	'Lifetime corrupt packets', 'Steady-state repaired timestamps', 'Lifetime missing DTS',
  ]
  const rows = snapshot.value.cameras.flatMap(camera => {
    const streams = camera.streams.length ? camera.streams : [{ tier: 'none', available: false, state: 'unavailable' } as StreamHealth]
    return streams.map(stream => [
      camera.name,
      camera.state,
      stream.tier,
      stream.videoCodec ?? '',
      stream.width && stream.height ? `${stream.width}x${stream.height}` : '',
      camera.processing?.pendingEssential ?? '',
      camera.processing?.oldestPendingEssentialMs ?? '',
      stream.retainedSegments ?? '',
	  steadyCount(stream, 'establishedDroppedVideoPackets', 'droppedVideoPackets') ?? '',
      stream.corruptVideoPackets ?? '',
	  steadyCount(stream, 'establishedRepairedVideoTimestamps', 'repairedVideoTimestamps') ?? '',
      stream.missingVideoDtsPackets ?? '',
    ])
  })
  const safe = (value: unknown) => {
    const text = String(value).replace(/[\t\r\n]/g, ' ')
    return /^\s*[=+\-@]/.test(text) ? `'${text}` : text
  }
  try {
    await navigator.clipboard.writeText([header, ...rows].map(row => row.map(safe).join('\t')).join('\n'))
    copied.value = true
    copyFailed.value = false
  } catch {
    copyFailed.value = true
    copied.value = false
  }
  window.setTimeout(() => { copied.value = false; copyFailed.value = false }, 2000)
}

onMounted(() => {
  refresh()
  ageTimer = window.setInterval(() => { nowMonotonicMs.value = performance.now() }, 1000)
})
onBeforeUnmount(() => {
  if (ageTimer) window.clearInterval(ageTimer)
})
</script>

<template>
  <div class="health-dashboard" data-bs-theme="dark">
    <div class="d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3">
      <div>
        <h5 class="mb-1">Performance and health</h5>
        <div class="health-secondary small">
          On-demand server snapshot combined with active players in this browser profile.
        </div>
      </div>
      <div class="d-flex gap-2">
        <button v-if="!standalone" class="btn btn-sm btn-outline-primary" @click="openHealthMonitor">Open monitor</button>
        <button class="btn btn-sm btn-outline-secondary" :disabled="!snapshot" @click="copyTable">
          {{ copied ? 'Copied' : copyFailed ? 'Copy failed' : 'Copy table' }}
        </button>
        <button class="btn btn-sm btn-outline-secondary" :disabled="!snapshot" @click="combinedExport">Export JSON</button>
        <button class="btn btn-sm btn-primary" :disabled="loading" @click="refresh">
          {{ loading ? 'Collecting…' : 'Refresh' }}
        </button>
      </div>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>

    <template v-if="snapshot">
      <div class="row g-3 mb-3">
        <div class="col-sm-6 col-xl-3">
          <div class="card h-100"><div class="card-body py-3">
            <div class="health-secondary small">Host memory</div>
            <div class="fs-5">{{ formatBytes(memoryUsed) }} / {{ formatBytes(snapshot.server.host.totalMemoryBytes) }}</div>
            <div class="small">{{ snapshot.server.host.memoryLoadPercent ?? 'Unknown' }}% physical memory used · {{ snapshot.server.host.logicalProcessors }} logical CPUs</div>
          </div></div>
        </div>
        <div class="col-sm-6 col-xl-3">
          <div class="card h-100"><div class="card-body py-3">
            <div class="health-secondary small">Witness process private memory</div>
            <div class="fs-5">{{ formatBytes(snapshot.server.host.processPrivateBytes) }}</div>
            <div class="small">{{ formatBytes(snapshot.server.host.processWorkingSetBytes) }} working set</div>
          </div></div>
        </div>
        <div class="col-sm-6 col-xl-3">
          <div class="card h-100"><div class="card-body py-3">
            <div class="health-secondary small">Build identity</div>
            <div class="fs-6 font-monospace text-break">{{ snapshot.server.webBuildHash || 'Unknown' }}</div>
            <div class="small">Client {{ clientBuildHash }}</div>
          </div></div>
        </div>
        <div class="col-sm-6 col-xl-3">
          <div class="card h-100"><div class="card-body py-3">
            <div class="health-secondary small">Snapshot</div>
            <div class="fs-6 d-flex align-items-center gap-2">
              {{ new Date(snapshot.sampledAtUtc).toLocaleString() }}
              <span class="badge" :class="snapshotAge.className">{{ snapshotAge.label }}</span>
            </div>
            <div class="small">Collected in {{ snapshot.collectionDurationMs }} ms · {{ snapshot.server.collectionMode }}</div>
          </div></div>
        </div>
      </div>

      <div v-if="snapshot.server.audioIntelligence" class="card mb-3">
        <div class="card-header d-flex justify-content-between">
          <span>Audio intelligence</span>
          <span class="badge" :class="snapshot.server.audioIntelligence.workerLoaded ? 'bg-success' : 'bg-secondary'">
            {{ snapshot.server.audioIntelligence.workerLoaded ? 'Model ready' : 'Unavailable' }}
          </span>
        </div>
        <div class="card-body py-2 small d-flex flex-wrap gap-4">
          <span><strong>{{ snapshot.server.audioIntelligence.clipsProcessed.toLocaleString() }}</strong> clips analysed</span>
          <span><strong>{{ snapshot.server.audioIntelligence.eventsProduced.toLocaleString() }}</strong> events</span>
          <span><strong>{{ snapshot.server.audioIntelligence.meanInferenceMs.toFixed(1) }} ms</strong> mean inference</span>
          <span><strong>{{ snapshot.server.audioIntelligence.lastInferenceMs.toFixed(1) }} ms</strong> last inference</span>
          <span :class="snapshot.server.audioIntelligence.decodeFailures ? 'text-warning' : ''">
            {{ snapshot.server.audioIntelligence.decodeFailures }} decode failures
          </span>
          <span :class="snapshot.server.audioIntelligence.inferenceFailures ? 'text-danger' : ''">
            {{ snapshot.server.audioIntelligence.inferenceFailures }} inference failures
          </span>
        </div>
      </div>

      <div class="card mb-3">
        <div class="card-header d-flex flex-wrap justify-content-between gap-2">
          <span>Server cameras</span>
          <span class="health-secondary small">Cumulative totals · reset scope varies · rates unavailable</span>
        </div>
        <div class="table-responsive">
          <table class="table table-dark table-sm table-hover align-middle mb-0 health-table">
            <thead><tr>
              <th>Camera</th><th>Connection</th><th>Streams</th><th>Essential queue</th>
              <th>AI queue</th><th>Packet integrity</th><th>Stream timing</th>
            </tr></thead>
            <tbody>
              <template v-for="camera in snapshot.cameras" :key="camera.cameraId">
              <tr>
                <td>
                  <div class="camera-name">{{ camera.name }}</div>
                  <div class="health-secondary small">Camera #{{ camera.cameraId }}</div>
                  <button
                    class="btn btn-link btn-sm event-toggle p-0 mt-1"
                    :disabled="cameraMediaEvents(camera).length === 0"
                    @click="toggleCameraEvents(camera.cameraId)"
                  >
                    {{ expandedCameraId === camera.cameraId ? 'Hide' : 'Show' }} media events
                    <span v-if="cameraMediaEvents(camera).length">({{ cameraMediaEvents(camera).length }})</span>
                  </button>
                </td>
                <td><span class="badge" :class="stateClass(camera.state)">{{ camera.state }}</span></td>
                <td>
                  <div v-for="stream in camera.streams" :key="stream.tier" class="stream-line">
                    <span class="stream-tier">{{ streamTierLabel(stream.tier) }}</span>
                    <span class="badge stream-state me-1" :class="stateClass(stream.state)">{{ stream.state }}</span>
                    {{ streamLabel(stream) }}
                    <span v-if="stream.retainedSegments != null" class="health-secondary"> · {{ stream.retainedSegments }} retained segments</span>
                    <span v-if="stream.initStructureValid === false" class="badge bg-danger ms-1">Invalid init</span>
                  </div>
                  <span v-if="camera.streams.length === 0" class="health-secondary">No stream data</span>
                </td>
                <td>
                  <strong>{{ camera.processing?.pendingEssential ?? '—' }}</strong> pending
                  <div class="health-secondary small">Oldest {{ formatMs(camera.processing?.oldestPendingEssentialMs) }} · mean wait {{ formatMs(camera.processing?.ingressWaitMeanMs) }}</div>
                </td>
                <td>
                  <strong>{{ camera.processing?.pendingAI ?? '—' }}</strong> pending
                  <div class="health-secondary small">Oldest {{ formatMs(camera.processing?.oldestPendingAIMs) }} · {{ formatCount(camera.processing?.coalescedAIFrames) }} coalesced</div>
                </td>
                <td>
                  <div v-for="stream in camera.streams" :key="`${stream.tier}-packets`" class="small">
                    <span class="evidence-tier">{{ streamTierLabel(stream.tier) }}</span>
					{{ steadyCount(stream, 'establishedDroppedVideoPackets', 'droppedVideoPackets') ?? '—' }} steady-state dropped ·
					{{ steadyCount(stream, 'establishedRepairedVideoTimestamps', 'repairedVideoTimestamps') ?? '—' }} steady-state repairs ·
					{{ stream.corruptVideoPackets ?? '—' }} lifetime corrupt ·
					{{ stream.missingVideoDtsPackets ?? '—' }} lifetime missing DTS
					<div v-if="stream.streamEstablished && (stream.startupDroppedVideoPackets || stream.startupRepairedVideoTimestamps)" class="health-secondary">
					  Startup excluded: {{ stream.startupDroppedVideoPackets ?? 0 }} dropped ·
					  {{ stream.startupRepairedVideoTimestamps ?? 0 }} repairs
					</div>
                    <div v-if="dropReasonSummary(stream)" class="health-secondary disposition-summary">
                      {{ streamTierLabel(stream.tier) }} dispositions: {{ dropReasonSummary(stream) }}
                    </div>
                  </div>
                </td>
                <td>
                  <div v-for="stream in camera.streams" :key="`${stream.tier}-av`" class="small">
                    <span class="evidence-tier">{{ streamTierLabel(stream.tier) }}</span>
                    A/V skew {{ formatMs(stream.audioVideoSkewMs) }} · maximum segment duration difference {{ formatMs(stream.maxSegmentDriftMs) }}
                  </div>
                </td>
              </tr>
              <tr v-if="expandedCameraId === camera.cameraId" class="media-event-row">
                <td colspan="7">
                  <div class="media-event-header">
                    Recent media activity for {{ camera.name }}
                    <span class="health-secondary">Newest first · bounded to the last 48 events per stream</span>
                  </div>
                  <div class="media-event-feed">
                    <div
                      v-for="event in cameraMediaEvents(camera)"
                      :key="`${event.tier}-${event.sequence}`"
                      class="media-event"
                      :class="mediaEventClass(event.severity)"
                    >
                      <div class="event-meta">
                        <span class="event-time">{{ mediaEventTime(event.timestampUnixMs) }}</span>
                        <span class="badge bg-secondary">{{ streamTierLabel(event.tier) }}</span>
                        <span class="badge" :class="event.severity === 'error' ? 'bg-danger' : event.severity === 'warning' ? 'bg-warning text-dark' : 'bg-info text-dark'">{{ event.severity }}</span>
                        <span>{{ event.category }} · {{ event.phase }} · {{ event.component }}</span>
                      </div>
                      <code class="event-message">{{ event.message }}</code>
                      <div class="event-correlation health-secondary">
                        Activity #{{ event.activityId || '—' }} · packet #{{ event.packetSequence || '—' }} · generation {{ event.generation }} · segment {{ event.segmentIndex }}.{{ event.partialIndex }}
                        <template v-if="event.disposition"> · {{ event.disposition }}</template>
                        <template v-if="event.sourceDtsUs != null"> · DTS {{ event.sourceDtsUs }}µs</template>
                        <template v-if="event.keyframe"> · keyframe</template>
                      </div>
                    </div>
                    <div v-if="cameraMediaEvents(camera).length === 0" class="health-secondary p-2">No recent media events.</div>
                  </div>
                </td>
              </tr>
              </template>
            </tbody>
          </table>
        </div>
      </div>

      <div class="card">
        <div class="card-header d-flex flex-wrap justify-content-between gap-2">
          <span>Browser players</span><span class="health-secondary small">{{ clientCoverage }}</span>
        </div>
        <div class="monitor-hint small">
          Open the monitor from a dashboard, then keep both windows visible side by side for representative rendering data. Minimized or obscured pages may be reported as hidden by the browser.
        </div>
        <div v-if="clientSessions.every(session => session.players.length === 0)" class="card-body health-secondary">
          No active live players replied from this browser profile. Keep a dashboard or stream tab open to collect presentation evidence.
        </div>
        <div v-else class="table-responsive">
          <table class="table table-dark table-sm table-hover mb-0 health-table client-table">
            <thead><tr><th>Camera</th><th>Stream</th><th>Playback lag</th><th>Buffer state</th><th>Transport / buffer detail</th><th>Frames</th><th>Dropped</th><th>Corrupt</th><th>Restarts / stalls / errors</th></tr></thead>
            <tbody>
              <template v-for="session in clientSessions" :key="session.sessionId">
                <tr class="session-row">
                  <td colspan="9">
                    <span class="session-path">{{ session.path }}</span>
                    <span class="badge ms-2" :class="session.visibilityState === 'visible' ? 'bg-success' : 'bg-secondary'">{{ session.visibilityState }}</span>
                    <span class="health-secondary ms-2">Build {{ session.buildHash }} · sampled {{ new Date(session.sampledAtUtc).toLocaleTimeString() }}<span v-if="session.playersTruncated"> · truncated</span></span>
                  </td>
                </tr>
                <tr v-for="player in session.players" :key="`${session.sessionId}-${player.id}`">
                  <td>{{ playerLabel(player.id) }}</td><td>{{ streamTierLabel(player.selectedStream ?? 'unknown') }}</td><td>{{ formatMs(player.latencyMs) }}</td>
                  <td>{{ readyStateLabel(player.readyState) }}</td><td class="health-secondary">{{ playerBufferClue(player) }}</td><td>{{ formatCount(player.totalVideoFrames) }}</td>
                  <td>{{ formatCount(player.droppedVideoFrames) }}</td><td>{{ formatCount(player.corruptedVideoFrames) }}</td>
                  <td>{{ player.restartCount ?? 0 }} / {{ player.stallCount ?? 0 }} / {{ player.errorCount ?? 0 }}<span class="health-secondary">{{ playerRestartClue(player) }}</span></td>
                </tr>
              </template>
            </tbody>
          </table>
        </div>
      </div>
    </template>
  </div>
</template>

<style scoped>
.health-dashboard {
  --bs-body-color: #e1e4e8;
  --bs-secondary-color: #9da7b3;
  --bs-emphasis-color: #f3f4f6;
  color: #e1e4e8;
}
.health-secondary { color: #9da7b3; }
.health-table { --bs-table-color: #d8dee6; --bs-table-hover-color: #f3f4f6; }
.health-table th { white-space: nowrap; font-size: 0.75rem; color: #aeb7c2; letter-spacing: 0.015em; }
.health-table td { padding-top: 0.45rem; padding-bottom: 0.45rem; font-size: 0.82rem; }
.camera-name, .session-path { color: #f3f4f6; font-weight: 600; }
.stream-line { white-space: nowrap; line-height: 1.55; }
.stream-tier, .evidence-tier { display: inline-block; min-width: 3.4rem; color: #7eb8f2; font-weight: 600; }
.monitor-hint { padding: 0.55rem 0.75rem; color: #aeb7c2; background: rgba(74, 144, 217, 0.08); border-bottom: 1px solid var(--bs-border-color); }
.session-row td { padding: 0.55rem 0.75rem; background: #20252d; border-top-width: 2px; }
.client-table tbody tr:not(.session-row) td:first-child { padding-left: 1.25rem; }
.event-toggle { color: #7eb8f2; font-size: 0.74rem; text-decoration: none; }
.event-toggle:disabled { color: #77818c; opacity: 1; }
.disposition-summary { max-width: 30rem; margin-top: 0.15rem; line-height: 1.35; }
.media-event-row td { padding: 0 !important; background: #11151b; }
.media-event-header { display: flex; justify-content: space-between; gap: 1rem; padding: 0.55rem 0.75rem; color: #f3f4f6; background: #20252d; font-size: 0.8rem; font-weight: 600; }
.media-event-feed { max-height: 22rem; overflow: auto; }
.media-event { padding: 0.6rem 0.75rem; border-bottom: 1px solid #2d333b; border-left: 3px solid #58a6ff; }
.media-event.event-warning { border-left-color: #d29922; }
.media-event.event-error { border-left-color: #f85149; }
.event-meta { display: flex; flex-wrap: wrap; align-items: center; gap: 0.4rem; color: #aeb7c2; font-size: 0.73rem; }
.event-time { min-width: 6.8rem; color: #d8dee6; font-variant-numeric: tabular-nums; }
.event-message { display: block; margin-top: 0.35rem; color: #f0f3f6; white-space: pre-wrap; overflow-wrap: anywhere; }
.event-correlation { margin-top: 0.3rem; font-size: 0.72rem; font-variant-numeric: tabular-nums; }
</style>
