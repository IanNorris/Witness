export interface ClientPlayerHealth {
  id: string
  selectedStream?: string
  latencyMs?: number
  readyState?: number
  totalVideoFrames?: number
  droppedVideoFrames?: number
  corruptedVideoFrames?: number
  restartCount?: number
  stallCount?: number
  errorCount?: number
}

export interface ClientHealthSession {
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
const TAB_ID = typeof crypto.randomUUID === 'function'
  ? crypto.randomUUID()
  : `${Date.now()}-${Math.random().toString(16).slice(2)}`
let responder: BroadcastChannel | null = null
let lastResponseAt = 0

function boundedText(value: unknown, maxLength: number): string {
  return typeof value === 'string' ? value.slice(0, maxLength) : ''
}

function optionalFinite(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined
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
      totalVideoFrames: optionalFinite(player.totalVideoFrames),
      droppedVideoFrames: optionalFinite(player.droppedVideoFrames),
      corruptedVideoFrames: optionalFinite(player.corruptedVideoFrames),
      restartCount: optionalFinite(player.restartCount),
      stallCount: optionalFinite(player.stallCount),
      errorCount: optionalFinite(player.errorCount),
    })
  }
  const visibility = source.visibilityState
  const sampledAtUtc = boundedText(source.sampledAtUtc, 40)
  if (!Number.isFinite(Date.parse(sampledAtUtc))) return null
  return {
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
      players.push({
        id: boundedText(id, 80),
        selectedStream: boundedText(data.liveState?.selectedStream, 32) || undefined,
        latencyMs: optionalFinite(data.liveState?.latencyMs),
        readyState: optionalFinite(data.currentState?.readyState),
        totalVideoFrames: optionalFinite(quality?.totalVideoFrames),
        droppedVideoFrames: optionalFinite(quality?.droppedVideoFrames),
        corruptedVideoFrames: optionalFinite(quality?.corruptedVideoFrames),
        restartCount: optionalFinite(data.stats?.restartCount),
        stallCount: optionalFinite(data.stats?.stallCount),
        errorCount: optionalFinite(data.stats?.errorCount),
      })
      seen.add(id)
    }
  }

  players.sort((a, b) => a.id.localeCompare(b.id, undefined, { numeric: true }))
  return {
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
