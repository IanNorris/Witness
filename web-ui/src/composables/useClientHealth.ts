import { sanitizeDriftSample, sanitizeDriftSamples, type DriftSample } from './driftDiagnostics'
import { api } from './useApi'

export interface ClientPlayerHealth {
  id: string
  selectedStream?: string
  latencyMs?: number
  readyState?: number
  mediaSourceState?: string
  wsReadyState?: number | null
  wsOpenAgeMs?: number | null
  reconnectPendingMs?: number | null
  lastFragAge?: number | null
  appendQueueLength?: number
  appendQueueBytes?: number
  appendQueueOldestAgeMs?: number
  sourceBufferGeneration?: number
  sourceBufferUpdating?: boolean | null
  sourceBufferOperation?: string
  sourceBufferOperationAgeMs?: number
  pipelineAgeMs?: number
  lastAppendAgeMs?: number | null
  waitingForKeyframe?: boolean
  hasInitialBuffer?: boolean
  awaitingInit?: boolean
  totalVideoFrames?: number
  droppedVideoFrames?: number
  corruptedVideoFrames?: number
  restartCount?: number
  stallCount?: number
  errorCount?: number
  totalFragments?: number
  integrityMismatchCount?: number
  decodeCorruptionCount?: number
  renderSuppressionCount?: number
  lastEventType?: string
  lastRestartReason?: string
  recentEvents?: ClientPlayerEvent[]
  driftSamples?: DriftSample[]
  lastDriftRestart?: ClientPlayerEvent
  lastError?: ClientPlayerEvent
}

export interface ClientPlayerEvent {
  t: string
  type: string
  reason?: string
  code?: number
  message?: string
  name?: string
  readyState?: number
  mediaSourceState?: string
  generation?: number
  segmentIndex?: number
  partIndex?: number
  backoffMs?: number
  consecutive?: number
  mimeType?: string
  lagMs?: number
  drift?: DriftSample
  driftSamples?: DriftSample[]
  wsReadyState?: number
  intentionalClose?: boolean
}

export interface ClientHealthSession {
  browserId?: string
  username?: string
  userUid?: number
  receivedUnixMs?: number
  serverRecorded?: boolean
  sessionId: string
  sampledAtUtc: string
  buildHash: string
  userAgent: string
  path: string
  visibilityState: DocumentVisibilityState
  players: ClientPlayerHealth[]
  playersTruncated: boolean
}

export interface ClientHealthCollection {
  sessions: ClientHealthSession[]
  mode: 'broadcast' | 'current-tab-no-broadcast-channel' | 'current-tab-channel-unavailable' | 'current-tab-no-responses'
  waitMs: number
}

const CHANNEL_NAME = 'witness-client-health-v1'
const MAX_SESSIONS = 32
const MAX_PLAYERS_PER_SESSION = 64
const MAX_EVENTS_PER_PLAYER = 40
const TAB_ID = typeof crypto.randomUUID === 'function'
  ? crypto.randomUUID()
  : `${Date.now()}-${Math.random().toString(16).slice(2)}`
const BROWSER_ID = (() => {
  const key = 'witness-browser-health-id-v1'
  try {
    const saved = localStorage.getItem(key)
    if (saved && /^[a-zA-Z0-9_-]{1,80}$/.test(saved)) return saved
    localStorage.setItem(key, TAB_ID)
  } catch { /* Storage-disabled browsers still report a per-page identity. */ }
  return TAB_ID
})()
let responder: BroadcastChannel | null = null
let lastResponseAt = 0

function boundedText(value: unknown, maxLength: number): string {
  return typeof value === 'string' ? value.slice(0, maxLength) : ''
}

function optionalFinite(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined
}

function optionalBoundedText(value: unknown, maxLength: number): string | undefined {
  const text = boundedText(value, maxLength)
  return text || undefined
}

function sanitizeEvent(value: unknown): ClientPlayerEvent | null {
  if (!value || typeof value !== 'object') return null
  const source = value as Record<string, unknown>
  const t = boundedText(source.t, 40)
  const type = boundedText(source.type, 64)
  if (!Number.isFinite(Date.parse(t)) || !type) return null
  return {
    t,
    type,
    reason: optionalBoundedText(source.reason, 64),
    code: optionalFinite(source.code),
    message: optionalBoundedText(source.message, 160),
    name: optionalBoundedText(source.name, 80),
    readyState: optionalFinite(source.readyState),
    mediaSourceState: optionalBoundedText(source.mediaSourceState, 32),
    generation: optionalFinite(source.generation),
    segmentIndex: optionalFinite(source.segmentIndex),
    partIndex: optionalFinite(source.partIndex),
    backoffMs: optionalFinite(source.backoffMs),
    consecutive: optionalFinite(source.consecutive),
    mimeType: optionalBoundedText(source.mimeType, 120),
    lagMs: optionalFinite(source.lagMs),
    drift: sanitizeDriftSample(source.drift),
    driftSamples: sanitizeDriftSamples(source.driftSamples),
    wsReadyState: optionalFinite(source.wsReadyState),
    intentionalClose: typeof source.intentionalClose === 'boolean' ? source.intentionalClose : undefined,
  }
}

function sanitizeEvents(value: unknown): ClientPlayerEvent[] | undefined {
  if (!Array.isArray(value)) return undefined
  const events = value.slice(-MAX_EVENTS_PER_PLAYER)
    .map(sanitizeEvent)
    .filter((event): event is ClientPlayerEvent => event !== null)
  return events.length > 0 ? events : undefined
}

function lastEventOfType(events: ClientPlayerEvent[] | undefined, type: string): ClientPlayerEvent | undefined {
  if (!events) return undefined
  for (let i = events.length - 1; i >= 0; --i) {
    if (events[i]!.type === type) return events[i]
  }
  return undefined
}

function validateSession(value: unknown): ClientHealthSession | null {
  if (!value || typeof value !== 'object') return null
  const source = value as Record<string, unknown>
  const id = boundedText(source.sessionId, 80)
  if (!id || !Array.isArray(source.players) || source.players.length > MAX_PLAYERS_PER_SESSION) return null
  const players: ClientPlayerHealth[] = []
  for (const candidate of source.players) {
    if (!candidate || typeof candidate !== 'object') return null
    const player = candidate as Record<string, unknown>
    const playerId = boundedText(player.id, 80)
    if (!playerId) return null
    players.push({
      id: playerId,
      selectedStream: boundedText(player.selectedStream, 32) || undefined,
      latencyMs: optionalFinite(player.latencyMs),
      readyState: optionalFinite(player.readyState),
      mediaSourceState: optionalBoundedText(player.mediaSourceState, 32),
      wsReadyState: optionalFinite(player.wsReadyState) ?? null,
      wsOpenAgeMs: optionalFinite(player.wsOpenAgeMs) ?? null,
      reconnectPendingMs: optionalFinite(player.reconnectPendingMs) ?? null,
      lastFragAge: optionalFinite(player.lastFragAge) ?? null,
      appendQueueLength: optionalFinite(player.appendQueueLength),
      appendQueueBytes: optionalFinite(player.appendQueueBytes),
      appendQueueOldestAgeMs: optionalFinite(player.appendQueueOldestAgeMs),
      sourceBufferGeneration: optionalFinite(player.sourceBufferGeneration),
      sourceBufferUpdating: typeof player.sourceBufferUpdating === 'boolean' ? player.sourceBufferUpdating : null,
      sourceBufferOperation: optionalBoundedText(player.sourceBufferOperation, 64),
      sourceBufferOperationAgeMs: optionalFinite(player.sourceBufferOperationAgeMs),
      pipelineAgeMs: optionalFinite(player.pipelineAgeMs),
      lastAppendAgeMs: optionalFinite(player.lastAppendAgeMs) ?? null,
      waitingForKeyframe: player.waitingForKeyframe === true,
      hasInitialBuffer: player.hasInitialBuffer === true,
      awaitingInit: player.awaitingInit === true,
      totalVideoFrames: optionalFinite(player.totalVideoFrames),
      droppedVideoFrames: optionalFinite(player.droppedVideoFrames),
      corruptedVideoFrames: optionalFinite(player.corruptedVideoFrames),
      restartCount: optionalFinite(player.restartCount),
      stallCount: optionalFinite(player.stallCount),
      errorCount: optionalFinite(player.errorCount),
      totalFragments: optionalFinite(player.totalFragments),
      integrityMismatchCount: optionalFinite(player.integrityMismatchCount),
      decodeCorruptionCount: optionalFinite(player.decodeCorruptionCount),
      renderSuppressionCount: optionalFinite(player.renderSuppressionCount),
      lastEventType: optionalBoundedText(player.lastEventType, 64),
      lastRestartReason: optionalBoundedText(player.lastRestartReason, 64),
      recentEvents: sanitizeEvents(player.recentEvents),
      driftSamples: sanitizeDriftSamples(player.driftSamples),
      lastDriftRestart: sanitizeEvent(player.lastDriftRestart) ?? undefined,
      lastError: sanitizeEvent(player.lastError) ?? undefined,
    })
  }
  const visibility = source.visibilityState
  const sampledAtUtc = boundedText(source.sampledAtUtc, 40)
  if (!Number.isFinite(Date.parse(sampledAtUtc))) return null
  return {
    browserId: boundedText(source.browserId, 80) || undefined,
    sessionId: id,
    sampledAtUtc,
    buildHash: boundedText(source.buildHash, 80),
    userAgent: boundedText(source.userAgent, 512),
    path: boundedText(source.path, 256),
    visibilityState: visibility === 'visible' || visibility === 'hidden'
      ? visibility
      : 'hidden',
    players,
    playersTruncated: source.playersTruncated === true,
  }
}

export function collectLocalClientHealth(): ClientHealthSession {
  const root = window as unknown as Record<string, unknown>
  const registries = [root._witnessDiag, root._witnessMseDiag]
  const players: ClientPlayerHealth[] = []
  const seen = new Set<string>()
  let playersTruncated = false

  registryLoop: for (const registry of registries) {
    if (!registry || typeof registry !== 'object') continue
    for (const [id, diagnostic] of Object.entries(registry as Record<string, unknown>)) {
      if (seen.has(id) || !diagnostic || typeof diagnostic !== 'object') continue
      const snapshotFn = (diagnostic as { snapshot?: () => Record<string, any> }).snapshot
      if (typeof snapshotFn !== 'function') continue
      if (players.length >= MAX_PLAYERS_PER_SESSION) {
        playersTruncated = true
        break registryLoop
      }
      let data: Record<string, any>
      try {
        data = snapshotFn.call(diagnostic)
      } catch {
        continue
      }
      const quality = data.currentState?.playbackQuality
      const recentEvents = sanitizeEvents(data.importantEvents ?? data.events)
      const lastEvent = recentEvents?.[recentEvents.length - 1]
      const lastRestart = lastEventOfType(recentEvents, 'restart') ??
        lastEventOfType(recentEvents, 'reconnect') ??
        lastEventOfType(recentEvents, 'stuckRestart') ??
        lastEventOfType(recentEvents, 'frozenRestart') ??
        lastEventOfType(recentEvents, 'initialTimeout')
      players.push({
        id: boundedText(id, 80),
        selectedStream: boundedText(data.liveState?.selectedStream, 32) || undefined,
        latencyMs: optionalFinite(data.liveState?.latencyMs),
        readyState: optionalFinite(data.currentState?.readyState),
        mediaSourceState: optionalBoundedText(data.liveState?.mediaSourceState, 32),
        wsReadyState: optionalFinite(data.liveState?.wsReadyState) ?? null,
        wsOpenAgeMs: optionalFinite(data.liveState?.wsOpenAgeMs) ?? null,
        reconnectPendingMs: optionalFinite(data.liveState?.reconnectPendingMs) ?? null,
        lastFragAge: optionalFinite(data.liveState?.lastFragAge) ?? null,
        appendQueueLength: optionalFinite(data.liveState?.appendQueueLength),
        appendQueueBytes: optionalFinite(data.liveState?.appendQueueBytes),
        appendQueueOldestAgeMs: optionalFinite(data.liveState?.appendQueueOldestAgeMs),
        sourceBufferGeneration: optionalFinite(data.liveState?.sourceBufferGeneration),
        sourceBufferUpdating: typeof data.liveState?.sourceBufferUpdating === 'boolean' ? data.liveState.sourceBufferUpdating : null,
        sourceBufferOperation: optionalBoundedText(data.liveState?.sourceBufferOperation, 64),
        sourceBufferOperationAgeMs: optionalFinite(data.liveState?.sourceBufferOperationAgeMs),
        pipelineAgeMs: optionalFinite(data.liveState?.pipelineAgeMs),
        lastAppendAgeMs: optionalFinite(data.liveState?.lastAppendAgeMs) ?? null,
        waitingForKeyframe: data.liveState?.waitingForKeyframe === true,
        hasInitialBuffer: data.liveState?.hasInitialBuffer === true,
        awaitingInit: data.liveState?.awaitingInit === true,
        totalVideoFrames: optionalFinite(quality?.totalVideoFrames),
        droppedVideoFrames: optionalFinite(quality?.droppedVideoFrames),
        corruptedVideoFrames: optionalFinite(quality?.corruptedVideoFrames),
        restartCount: optionalFinite(data.stats?.restartCount),
        stallCount: optionalFinite(data.stats?.stallCount),
        errorCount: optionalFinite(data.stats?.errorCount),
        totalFragments: optionalFinite(data.stats?.totalFragments),
        integrityMismatchCount: optionalFinite(data.stats?.integrityMismatchCount),
        decodeCorruptionCount: optionalFinite(data.stats?.decodeCorruptionCount),
        renderSuppressionCount: optionalFinite(data.stats?.renderSuppressionCount),
        lastEventType: lastEvent?.type,
        lastRestartReason: lastRestart?.reason ?? lastRestart?.type,
        recentEvents,
        driftSamples: sanitizeDriftSamples(data.driftSamples),
        lastDriftRestart: sanitizeEvent(data.lastDriftRestart) ?? undefined,
        lastError: sanitizeEvent(data.lastError) ?? undefined,
      })
      seen.add(id)
    }
  }

  players.sort((a, b) => a.id.localeCompare(b.id, undefined, { numeric: true }))
  return {
    browserId: BROWSER_ID,
    sessionId: TAB_ID,
    sampledAtUtc: new Date().toISOString(),
    buildHash: boundedText(__BUILD_HASH__, 80),
    userAgent: boundedText(navigator.userAgent, 512),
    path: boundedText(location.pathname, 256),
    visibilityState: document.visibilityState,
    players,
    playersTruncated,
  }
}

export interface StoredClientHealth {
  sequence: number
  receivedUnixMs: number
  userUid: number
  username: string
  browserId: string
  sessionId: string
  snapshot: ClientHealthSession
}
export interface ClientHealthHistory {
  ready: boolean
  storageAvailable: boolean
  durableThroughSequence: number
  pending: number
  rejected: number
  storageFailures: number
  retentionMs: number
  maxHistoryBytes: number
  maxSamples: number
  sessions: StoredClientHealth[]
  history: StoredClientHealth[]
}

export function compactHealthReport(session: ClientHealthSession): ClientHealthSession {
  // Preserve the scalar evidence needed to distinguish suspended playback from
  // delivery stalls even when full per-player ranges exceed the POST budget.
  const compactDrift = (sample?: DriftSample): DriftSample | undefined => sample ? {
    t: sample.t, generation: sample.generation, visibility: sample.visibility,
    currentTime: sample.currentTime, bufferedEnd: sample.bufferedEnd, lagMs: sample.lagMs,
    paused: sample.paused, readyState: sample.readyState, playbackRate: sample.playbackRate,
    schedulingDelayMs: sample.schedulingDelayMs, sampleIntervalMs: sample.sampleIntervalMs,
    playbackAdvanceMs: sample.playbackAdvanceMs, bufferAdvanceMs: sample.bufferAdvanceMs,
    fragmentAgeMs: sample.fragmentAgeMs, appendAgeMs: sample.appendAgeMs,
    bufferedRangeCount: sample.bufferedRangeCount, playheadBuffered: sample.playheadBuffered,
    nextBufferedStartSeconds: sample.nextBufferedStartSeconds, playheadGapMs: sample.playheadGapMs,
    headroomInCurrentRangeMs: sample.headroomInCurrentRangeMs, hasAudio: sample.hasAudio,
    sourceBufferMode: sample.sourceBufferMode, timestampOffsetSeconds: sample.timestampOffsetSeconds,
    presentedMediaTime: sample.presentedMediaTime, presentedFrames: sample.presentedFrames,
    presentationStallMs: sample.presentationStallMs, presentationVisible: sample.presentationVisible,
  } : undefined
  const compactRestart = (event?: ClientPlayerEvent): ClientPlayerEvent | undefined => event ? {
    t: event.t, type: event.type, reason: event.reason, lagMs: event.lagMs,
    drift: compactDrift(event.drift),
  } : undefined
  const compactError = (event?: ClientPlayerEvent): ClientPlayerEvent | undefined => event ? {
    t: event.t, type: event.type, name: event.name, reason: event.reason, code: event.code,
    generation: event.generation, consecutive: event.consecutive, readyState: event.readyState,
    mediaSourceState: event.mediaSourceState, wsReadyState: event.wsReadyState,
    intentionalClose: event.intentionalClose, drift: compactDrift(event.drift),
  } : undefined
  const encoder = new TextEncoder()
  const maxReportBytes = 60 * 1024
  const overBudget = () => encoder.encode(JSON.stringify(report)).length > maxReportBytes
  const report = { ...session, players: session.players.map(player => ({ ...player,
    recentEvents: player.recentEvents?.slice(-8).map(event => ({ ...event, message: undefined, driftSamples: undefined, drift: undefined })),
    driftSamples: player.driftSamples?.slice(-4), lastDriftRestart: compactRestart(player.lastDriftRestart),
    lastError: compactError(player.lastError),
  })) }
  if (overBudget()) {
    for (const player of report.players) {
      player.recentEvents = player.recentEvents?.slice(-2)
      player.driftSamples = player.driftSamples?.slice(-1).map(sample => compactDrift(sample)!)
    }
  }
  if (overBudget()) {
    for (const player of report.players) {
      player.recentEvents = undefined; player.driftSamples = undefined; player.lastDriftRestart = undefined
      player.lastError = undefined
    }
  }
  if (overBudget()) {
    // Even bounded strings/numbers can exceed the wire budget across 64
    // players (UTF-8 bytes are not JS character counts). Retain a stable prefix
    // with explicit incomplete coverage instead of losing the entire POST.
    report.playersTruncated = true
    let bytes = encoder.encode(JSON.stringify({ ...report, players: [] })).length
    const retained: typeof report.players = []
    for (const player of report.players) {
      const playerBytes = encoder.encode(JSON.stringify(player)).length + (retained.length ? 1 : 0)
      if (bytes + playerBytes > maxReportBytes) break
      retained.push(player)
      bytes += playerBytes
    }
    report.players = retained
  }
  return report
}

export function startClientHealthReporting(isAllowed: () => boolean): () => void {
  let stopped = false
  let busy = false
  let controller: AbortController | null = null
  let timer: ReturnType<typeof setTimeout> | null = null
  let lastAttempt = -Infinity
  async function send() {
    if (stopped || busy || !isAllowed() || performance.now() - lastAttempt < 1000) return
    busy = true
    lastAttempt = performance.now()
    controller = new AbortController()
    const timeout = setTimeout(() => controller?.abort(), 5000)
    try {
      // api supplies the session's CSRF token; cancellation bounds a stuck HTTP
      // connection so it cannot stop all subsequent reports indefinitely.
      await api('/debug/client-health', { method: 'POST', body: { snapshot: compactHealthReport(collectLocalClientHealth()) },
        redirectOnFail: false, signal: controller.signal })
    } catch { /* Telemetry must never interrupt playback or redirect a dashboard. */ }
    finally {
      clearTimeout(timeout)
      controller = null
      busy = false
      if (!stopped && isAllowed()) {
        if (timer) clearTimeout(timer)
        timer = setTimeout(send, 15000)
      }
    }
  }
  function changed() {
    if (timer) clearTimeout(timer)
    timer = setTimeout(send, 1000)
  }
  document.addEventListener('visibilitychange', changed)
  window.addEventListener('online', changed)
  timer = setTimeout(send, Math.random() * 1000)
  return () => {
    stopped = true
    if (timer) clearTimeout(timer)
    controller?.abort()
    document.removeEventListener('visibilitychange', changed)
    window.removeEventListener('online', changed)
  }
}

export function ensureClientHealthResponder(isAllowed: () => boolean): () => void {
  if (!isAllowed() || responder || typeof BroadcastChannel === 'undefined') return () => {}
  let channel: BroadcastChannel
  try {
    channel = new BroadcastChannel(CHANNEL_NAME)
    responder = channel
  } catch {
    responder = null
    return () => {}
  }
  channel.addEventListener('message', event => {
    if (!isAllowed()) return
    const requestId = boundedText(event.data?.requestId, 80)
    if (event.data?.type !== 'request' || !requestId) return
    const now = performance.now()
    if (now - lastResponseAt < 200) return
    lastResponseAt = now
    channel.postMessage({
      type: 'response',
      requestId,
      session: collectLocalClientHealth(),
    })
  })
  return () => {
    channel.close()
    if (responder === channel) responder = null
  }
}

export async function collectClientHealthSessions(waitMs = 350): Promise<ClientHealthCollection> {
  if (typeof BroadcastChannel === 'undefined') {
    return {
      sessions: [collectLocalClientHealth()],
      mode: 'current-tab-no-broadcast-channel',
      waitMs,
    }
  }

  const requestId = typeof crypto.randomUUID === 'function'
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`
  const sessions = new Map<string, ClientHealthSession>()
  let collector: BroadcastChannel
  try {
    collector = new BroadcastChannel(CHANNEL_NAME)
  } catch {
    return {
      sessions: [collectLocalClientHealth()],
      mode: 'current-tab-channel-unavailable',
      waitMs,
    }
  }
  collector.addEventListener('message', event => {
    if (event.data?.type !== 'response' || event.data.requestId !== requestId) return
    if (sessions.size >= MAX_SESSIONS) return
    const session = validateSession(event.data.session)
    if (session) sessions.set(session.sessionId, session)
  })
  collector.postMessage({ type: 'request', requestId })
  await new Promise(resolve => window.setTimeout(resolve, waitMs))
  collector.close()

  if (sessions.size === 0) {
    const local = collectLocalClientHealth()
    sessions.set(local.sessionId, local)
    return {
      sessions: [...sessions.values()],
      mode: 'current-tab-no-responses',
      waitMs,
    }
  }
  return {
    sessions: [...sessions.values()].sort((a, b) => a.path.localeCompare(b.path)),
    mode: 'broadcast',
    waitMs,
  }
}
