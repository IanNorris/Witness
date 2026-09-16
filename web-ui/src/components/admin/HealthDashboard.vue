<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { api } from '../../composables/useApi'
import {
  collectClientHealthSessions,
  type ClientHealthSession,
} from '../../composables/useClientHealth'

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
  missingVideoDtsPackets?: number
  audioVideoSkewMs?: number | null
  maxSegmentDriftMs?: number
  initStructureValid?: boolean | null
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
const clientBuildHash = __BUILD_HASH__

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
  return `${stream.tier === 'preview' ? 'P' : 'M'} ${stream.videoCodec || 'unknown'}${dimensions}`
}

function stateClass(state: string): string {
  const normalized = state.toLowerCase()
  if (normalized === 'connected' || normalized === 'healthy') return 'bg-success'
  if (normalized.includes('start') || normalized.includes('connect')) return 'bg-warning text-dark'
  return 'bg-danger'
}

const memoryUsed = computed(() => {
  const host = snapshot.value?.server.host
  if (!host?.totalMemoryBytes || host.availableMemoryBytes == null) return null
  return host.totalMemoryBytes - host.availableMemoryBytes
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
    'Oldest essential ms', 'Retained segments', 'Dropped packets',
    'Corrupt packets', 'Repaired timestamps', 'Missing DTS',
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
      stream.droppedVideoPackets ?? '',
      stream.corruptVideoPackets ?? '',
      stream.repairedVideoTimestamps ?? '',
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

onMounted(refresh)
</script>

<template>
  <div>
    <div class="d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3">
      <div>
        <h5 class="mb-1">Performance and health</h5>
        <div class="text-muted small">
          On-demand server snapshot combined with players active in this browser.
        </div>
      </div>
      <div class="d-flex gap-2">
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
            <div class="text-muted small">Host memory</div>
            <div class="fs-5">{{ formatBytes(memoryUsed) }} / {{ formatBytes(snapshot.server.host.totalMemoryBytes) }}</div>
            <div class="small">{{ snapshot.server.host.memoryLoadPercent ?? 'Unknown' }}% physical memory used · {{ snapshot.server.host.logicalProcessors }} logical CPUs</div>
          </div></div>
        </div>
        <div class="col-sm-6 col-xl-3">
          <div class="card h-100"><div class="card-body py-3">
            <div class="text-muted small">Witness process private memory</div>
            <div class="fs-5">{{ formatBytes(snapshot.server.host.processPrivateBytes) }}</div>
            <div class="small">{{ formatBytes(snapshot.server.host.processWorkingSetBytes) }} working set</div>
          </div></div>
        </div>
        <div class="col-sm-6 col-xl-3">
          <div class="card h-100"><div class="card-body py-3">
            <div class="text-muted small">Build identity</div>
            <div class="fs-6 font-monospace text-break">{{ snapshot.server.webBuildHash || 'Unknown' }}</div>
            <div class="small">Client {{ clientBuildHash }}</div>
          </div></div>
        </div>
        <div class="col-sm-6 col-xl-3">
          <div class="card h-100"><div class="card-body py-3">
            <div class="text-muted small">Snapshot</div>
            <div class="fs-6">{{ new Date(snapshot.sampledAtUtc).toLocaleString() }}</div>
            <div class="small">Collected in {{ snapshot.collectionDurationMs }} ms · {{ snapshot.server.collectionMode }}</div>
          </div></div>
        </div>
      </div>

      <div class="card mb-3">
        <div class="card-header d-flex justify-content-between">
          <span>Server cameras</span>
          <span class="text-muted small">Rates intentionally disabled until reset epochs are available</span>
        </div>
        <div class="table-responsive">
          <table class="table table-sm table-hover align-middle mb-0 health-table">
            <thead><tr>
              <th>Camera</th><th>State</th><th>Streams</th><th>Essential queue</th>
              <th>AI queue</th><th>Cumulative packet evidence</th><th>Timing evidence</th>
            </tr></thead>
            <tbody>
              <tr v-for="camera in snapshot.cameras" :key="camera.cameraId">
                <td><div>{{ camera.name }}</div><div class="text-muted small">#{{ camera.cameraId }}</div></td>
                <td><span class="badge" :class="stateClass(camera.state)">{{ camera.state }}</span></td>
                <td>
                  <div v-for="stream in camera.streams" :key="stream.tier" class="stream-line">
                    <span class="badge me-1" :class="stateClass(stream.state)">{{ stream.tier === 'preview' ? 'P' : 'M' }}</span>
                    {{ streamLabel(stream) }}
                    <span v-if="stream.retainedSegments != null" class="text-muted"> · {{ stream.retainedSegments }} retained</span>
                  </div>
                  <span v-if="camera.streams.length === 0" class="text-muted">None</span>
                </td>
                <td>
                  {{ camera.processing?.pendingEssential ?? '—' }} pending
                  <div class="text-muted small">oldest {{ formatMs(camera.processing?.oldestPendingEssentialMs) }} · mean {{ formatMs(camera.processing?.ingressWaitMeanMs) }}</div>
                </td>
                <td>
                  {{ camera.processing?.pendingAI ?? '—' }} pending
                  <div class="text-muted small">oldest {{ formatMs(camera.processing?.oldestPendingAIMs) }} · coalesced {{ camera.processing?.coalescedAIFrames ?? '—' }}</div>
                </td>
                <td>
                  <div v-for="stream in camera.streams" :key="`${stream.tier}-packets`" class="small">
                    {{ stream.tier === 'preview' ? 'P' : 'M' }}:
                    {{ stream.droppedVideoPackets ?? '—' }} drop · {{ stream.corruptVideoPackets ?? '—' }} corrupt ·
                    {{ stream.repairedVideoTimestamps ?? '—' }} repair · {{ stream.missingVideoDtsPackets ?? '—' }} no DTS
                  </div>
                </td>
                <td>
                  <div v-for="stream in camera.streams" :key="`${stream.tier}-av`" class="small">
                    {{ stream.tier === 'preview' ? 'P' : 'M' }} A/V skew {{ formatMs(stream.audioVideoSkewMs) }} · max segment duration discrepancy {{ formatMs(stream.maxSegmentDriftMs) }}
                  </div>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>

      <div class="card">
        <div class="card-header d-flex justify-content-between">
          <span>This browser profile</span><span class="text-muted small">{{ clientCoverage }}</span>
        </div>
        <div v-if="clientSessions.every(session => session.players.length === 0)" class="card-body text-muted">
          No active live players replied from this browser profile. Keep a dashboard or stream tab open to collect presentation evidence.
        </div>
        <div v-else class="table-responsive">
          <table class="table table-sm mb-0 health-table">
            <thead><tr><th>Tab</th><th>Player</th><th>Tier</th><th>Latency</th><th>Ready</th><th>Frames</th><th>Dropped</th><th>Corrupt</th><th>Restarts / stalls / errors</th></tr></thead>
            <tbody>
              <template v-for="session in clientSessions" :key="session.sessionId">
                <tr v-for="player in session.players" :key="`${session.sessionId}-${player.id}`">
                  <td><div>{{ session.path }}</div><div class="text-muted small">{{ session.visibilityState }} · {{ session.buildHash }} · {{ new Date(session.sampledAtUtc).toLocaleTimeString() }}<span v-if="session.playersTruncated"> · truncated</span></div></td>
                  <td>{{ player.id }}</td><td>{{ player.selectedStream ?? 'unknown' }}</td><td>{{ formatMs(player.latencyMs) }}</td>
                  <td>{{ player.readyState ?? '—' }}</td><td>{{ player.totalVideoFrames ?? '—' }}</td>
                  <td>{{ player.droppedVideoFrames ?? '—' }}</td><td>{{ player.corruptedVideoFrames ?? '—' }}</td>
                  <td>{{ player.restartCount ?? 0 }} / {{ player.stallCount ?? 0 }} / {{ player.errorCount ?? 0 }}</td>
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
.health-table th { white-space: nowrap; font-size: 0.75rem; color: var(--bs-secondary-color); }
.health-table td { font-size: 0.8rem; }
.stream-line { white-space: nowrap; }
</style>
