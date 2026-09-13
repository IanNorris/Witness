import { ref, onUnmounted, type Ref } from 'vue'

// ── Constants ─────────────────────────────────────────────────────────
const MSE_WATCHDOG_INTERVAL_MS = 250
const MSE_INITIAL_TIMEOUT_MS = 5000
const MSE_BACK_BUFFER_SECONDS = 5
const MSE_TARGET_HEADROOM_SECONDS = 1.25
const MSE_CATCH_UP_START_SECONDS = 1.75

// ── Diagnostics ───────────────────────────────────────────────────────
const DIAG_MAX_AGE_MS = 24 * 60 * 60 * 1000

interface DiagEvent {
  t: string
  type: string
  [key: string]: unknown
}

interface MseDiagStats {
  restartCount: number
  stallCount: number
  errorCount: number
  totalFragments: number
  maxLatencyMs: number
  minLatencyMs: number | null
  verifiedBinaryMessages: number
  integrityMismatchCount: number
}

function fnv1a32(data: ArrayBuffer): string {
  const bytes = new Uint8Array(data)
  let hash = 0x811c9dc5
  for (let i = 0; i < bytes.length; i++) {
    hash = Math.imul((hash ^ bytes[i]!) >>> 0, 0x01000193) >>> 0
  }
  return hash.toString(16).padStart(8, '0')
}

class MseDiagnostics {
  cameraID: string
  startTime: number
  events: DiagEvent[] = []
  importantEvents: DiagEvent[] = []
  stats: MseDiagStats = {
    restartCount: 0,
    stallCount: 0,
    errorCount: 0,
    totalFragments: 0,
    maxLatencyMs: 0,
    minLatencyMs: null,
    verifiedBinaryMessages: 0,
    integrityMismatchCount: 0,
  }
  _element: HTMLVideoElement | null = null

  constructor(cameraID: string) {
    this.cameraID = cameraID
    this.startTime = Date.now()
  }

  private _stateGetter: (() => Record<string, unknown>) | null = null

  setRef(element: HTMLVideoElement) {
    this._element = element
  }

  setStateGetter(getter: () => Record<string, unknown>) {
    this._stateGetter = getter
  }

  log(type: string, extra?: Record<string, unknown>) {
    const event: DiagEvent = {
      t: new Date().toISOString(),
      type,
      ...extra,
    }
    this.events.push(event)
    // Routine fragment traffic quickly fills the rolling diagnostic window.
    // Retain lifecycle, error, and recovery events separately so a later dump
    // still contains the cause of an earlier stall or restart.
    if (type !== 'partial' && type !== 'segmentComplete') {
      this.importantEvents.push(event)
    }
    this.pruneOldEvents()
  }

  recordLatency(ms: number) {
    if (ms > this.stats.maxLatencyMs) this.stats.maxLatencyMs = ms
    if (this.stats.minLatencyMs === null || ms < this.stats.minLatencyMs)
      this.stats.minLatencyMs = ms
  }

  pruneOldEvents() {
    const cutoff = Date.now() - DIAG_MAX_AGE_MS
    while (this.events.length > 0 && new Date(this.events[0]!.t).getTime() < cutoff) {
      this.events.shift()
    }
    while (
      this.importantEvents.length > 0 &&
      new Date(this.importantEvents[0]!.t).getTime() < cutoff
    ) {
      this.importantEvents.shift()
    }
  }

  snapshot() {
    const el = this._element
    const playbackQuality = el && typeof el.getVideoPlaybackQuality === 'function'
      ? el.getVideoPlaybackQuality()
      : null
    return {
      cameraID: this.cameraID,
      startTime: new Date(this.startTime).toISOString(),
      stats: { ...this.stats },
      liveState: this._stateGetter ? this._stateGetter() : null,
      currentState: el
        ? {
            readyState: el.readyState,
            currentTime: el.currentTime,
            paused: el.paused,
            ended: el.ended,
            playbackRate: el.playbackRate,
            error: el.error ? { code: el.error.code, message: el.error.message } : null,
            playbackQuality: playbackQuality
              ? {
                  creationTime: playbackQuality.creationTime,
                  totalVideoFrames: playbackQuality.totalVideoFrames,
                  droppedVideoFrames: playbackQuality.droppedVideoFrames,
                  corruptedVideoFrames: playbackQuality.corruptedVideoFrames,
                }
              : null,
            buffered:
              el.buffered.length > 0
                ? Array.from({ length: el.buffered.length }, (_, i) => [
                    el.buffered.start(i),
                    el.buffered.end(i),
                  ])
                : [],
          }
        : null,
      importantEvents: this.importantEvents.slice(-100),
      events: this.events.slice(-100),
    }
  }
}

// Global diagnostics map (shared with HLS diagnostics via window._witnessDiag)
const diagMap: Record<string, MseDiagnostics> = {}

// Register global dump function — merges MSE data with HLS data and downloads
if (typeof window !== 'undefined') {
  ;(window as any)._witnessMseDiag = diagMap
  const existingDumpAll = (window as any)._witnessDumpAll
  ;(window as any)._witnessDumpAll = async function () {
    const result: any = existingDumpAll ? await existingDumpAll() : { client: { timestamp: new Date().toISOString(), cameras: {} }, server: null }
    // Add MSE diagnostics to client cameras
    if (!result.client) result.client = { cameras: {} }
    if (!result.client.cameras) result.client.cameras = {}
    for (const [id, d] of Object.entries(diagMap)) {
      result.client.cameras[id] = d.snapshot()
    }
    // Download the combined dump
    const blob = new Blob([JSON.stringify(result, null, 2)], { type: 'application/json' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = 'witness-full-debug-' + new Date().toISOString().replace(/[:.]/g, '-') + '.json'
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)
  }
}

// ── Composable ────────────────────────────────────────────────────────
export interface MseStreamState {
  showSpinner: Ref<boolean>
  connectionLost: Ref<boolean>
  isActive: Ref<boolean>
  latencyMs: Ref<number>
  codecUnsupported: Ref<boolean>
}

interface PartialAppendMetadata {
  duration: number
  independent: boolean
  keyframeSeekSafe: boolean
  segmentIndex: number
  partIndex: number
  expectedBytes: number | null
  expectedHash?: string
}

interface BinaryIntegrityMetadata {
  expectedBytes: number | null
  expectedHash?: string
  segmentIndex?: number
  partIndex?: number
}

interface AppendQueueItem {
  data: ArrayBuffer
  partial?: PartialAppendMetadata
}

type SourceBufferOperation =
  | {
      kind: 'append'
      item: AppendQueueItem
      startTime: number | null
      buffer: SourceBuffer
      generation: number
      failed: boolean
    }
  | {
      kind: 'remove'
      removeTo: number
      buffer: SourceBuffer
      generation: number
      failed: boolean
    }

export function useMseStream(
  cameraId: number,
  videoRef: Ref<HTMLVideoElement | null>,
  suffix: string = '',
  useSubStream: boolean = false,
  codecHint?: string,
): MseStreamState {
  const showSpinner = ref(false)
  const connectionLost = ref(false)
  const isActive = ref(false)
  const latencyMs = ref(0)
  const codecUnsupported = ref(false)

  const diagId = String(cameraId) + (suffix ? '_' + suffix : '_mse')
  const diag = new MseDiagnostics(diagId)
  diagMap[diagId] = diag

  let ws: WebSocket | null = null
  let mediaSource: MediaSource | null = null
  let sourceBuffer: SourceBuffer | null = null
  let appendQueue: AppendQueueItem[] = []
  let sourceBufferOperation: SourceBufferOperation | null = null
  let sourceBufferGeneration = 0
  let lastTrimmedTo = 0
  let lastFragTime = 0
  let streamStartTime = Date.now()
  let watchdog: ReturnType<typeof setInterval> | null = null
  let restartBackoffMs = 3000
  let stuckBackoffMs = 3000
  let lowReadyStateSince = 0
  let lastCurrentTime = -1
  let currentTimeStalledSince = 0
  let highLatencySince = 0
  let catchUpActive = false
  let destroyed = false
  let intentionalClose = false
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let initGeneration = -1
  let expectingBinary: 'init' | 'partial' | null = null
  let pendingBinaryIntegrity: BinaryIntegrityMetadata | null = null
  let pendingPartialMetadata: PartialAppendMetadata | null = null
  let waitingForKeyframe = true  // Skip partials until first independent (keyframe)
  let hasInitialBuffer = false   // Seek to buffered range after first append
  let pendingInitData: ArrayBuffer | null = null  // Buffered init segment when MediaSource isn't open yet
  let consecutiveAppendErrors = 0  // Track consecutive appendBuffer failures for restart
  let keyframeTimes: number[] = []
  let lastKeyframeSeekTarget = -1

  function getWsUrl(): string {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
    const path = useSubStream ? `/ws/stream/sub/${cameraId}` : `/ws/stream/${cameraId}`
    return `${proto}//${location.host}${path}`
  }

  function processAppendQueue() {
    if (
      !sourceBuffer ||
      sourceBuffer.updating ||
      sourceBufferOperation ||
      appendQueue.length === 0
    ) return
    const buffer = sourceBuffer
    const generation = sourceBufferGeneration
    const item = appendQueue.shift()!
    let startTime: number | null = null
    if (item.partial && buffer.buffered.length > 0) {
      startTime = buffer.buffered.end(buffer.buffered.length - 1)
    }
    sourceBufferOperation = {
      kind: 'append',
      item,
      startTime,
      buffer,
      generation,
      failed: false,
    }
    try {
      buffer.appendBuffer(item.data)
    } catch (e: any) {
      sourceBufferOperation = null
      if (e.name === 'QuotaExceededError') {
        diag.log('quotaExceeded')
        trimBuffer(true)
        // Re-queue and retry
        appendQueue.unshift(item)
        setTimeout(() => processAppendQueue(), 100)
      } else {
        diag.stats.errorCount++
        consecutiveAppendErrors++
        const videoError = videoRef.value?.error
        diag.log('appendError', {
          name: e?.name ?? null,
          message: e?.message ?? String(e),
          consecutive: consecutiveAppendErrors,
          generation,
          mediaSourceState: mediaSource?.readyState ?? null,
          sourceBufferUpdating: buffer.updating,
          segmentIndex: item.partial?.segmentIndex ?? null,
          partIndex: item.partial?.partIndex ?? null,
          videoError: videoError
            ? { code: videoError.code, message: videoError.message }
            : null,
        })
        // Restart after 5 consecutive append failures — SourceBuffer is likely corrupted
        if (consecutiveAppendErrors >= 5) {
          restartStream('appendErrors')
          return
        }
      }
    }
  }

  function appendData(data: ArrayBuffer, partial?: PartialAppendMetadata): boolean {
    const generation = sourceBufferGeneration
    appendQueue.push({ data, partial })
    processAppendQueue()
    return sourceBufferGeneration === generation && sourceBuffer !== null
  }

  function trimBuffer(aggressive = false) {
    if (!sourceBuffer || sourceBuffer.updating || sourceBufferOperation) return false
    const video = videoRef.value
    if (!video) return false

    const trimTo = video.currentTime - (aggressive ? 1 : MSE_BACK_BUFFER_SECONDS)
    if (trimTo > 0 && sourceBuffer.buffered.length > 0) {
      const bufferedStart = sourceBuffer.buffered.start(0)
      if (trimTo <= bufferedStart + 0.05 || trimTo <= lastTrimmedTo + 0.05) return false
      const buffer = sourceBuffer
      sourceBufferOperation = {
        kind: 'remove',
        removeTo: trimTo,
        buffer,
        generation: sourceBufferGeneration,
        failed: false,
      }
      try {
        buffer.remove(0, trimTo)
        return true
      } catch {
        sourceBufferOperation = null
      }
    }
    return false
  }

  function setupMediaSource(element: HTMLVideoElement) {
    mediaSource = new MediaSource()
    element.src = URL.createObjectURL(mediaSource)

    mediaSource.addEventListener('sourceopen', () => {
      diag.log('sourceOpen')
      // If we received an init segment before sourceopen, process it now
      if (pendingInitData) {
        const data = pendingInitData
        pendingInitData = null
        if (!sourceBuffer) {
          createSourceBuffer()
        }
        if (sourceBuffer) {
          appendData(data)
          diag.log('pendingInitAppended')
        }
      }
    })

    mediaSource.addEventListener('sourceended', () => {
      diag.log('sourceEnded')
    })

    mediaSource.addEventListener('sourceclose', () => {
      diag.log('sourceClose')
    })
  }

  let audioCodec: string | undefined

  function createSourceBuffer(codec?: string) {
    if (!mediaSource || mediaSource.readyState !== 'open') return false

    // Use explicit codec if provided, then codecHint from caller, then default H.264 baseline
    const resolvedCodec = codec || codecHint || 'avc1.42001e'
    const mimeType = `video/mp4; codecs="${resolvedCodec}${audioCodec ? `,${audioCodec}` : ''}"`

    if (!MediaSource.isTypeSupported(mimeType)) {
      diag.log('unsupportedCodec', { mimeType })
      codecUnsupported.value = true
      return false
    }

    try {
      sourceBuffer = mediaSource.addSourceBuffer(mimeType)
      sourceBuffer.mode = 'sequence'
      const buffer = sourceBuffer
      const generation = ++sourceBufferGeneration
      buffer.addEventListener('updateend', () => {
        // Events from a removed SourceBuffer may arrive after a replacement
        // pipeline has started. They must never consume its operation record.
        if (sourceBuffer !== buffer || sourceBufferGeneration !== generation) return
        const operation = sourceBufferOperation
        if (!operation || operation.buffer !== buffer || operation.generation !== generation) return
        sourceBufferOperation = null

        if (operation.kind === 'remove') {
          if (!operation.failed) lastTrimmedTo = Math.max(lastTrimmedTo, operation.removeTo)
          processAppendQueue()
          return
        }

        if (operation.failed) {
          processAppendQueue()
          return
        }

        consecutiveAppendErrors = 0  // Reset only after a successful append

        // `independent` is carried alongside the binary partial through the
        // append queue. Record its actual position in the sequence timeline
        // only once the browser confirms that append has completed.
        const partial = operation.item.partial
        if (partial?.independent && partial.keyframeSeekSafe && partial.partIndex === 0) {
          const buffered = buffer.buffered
          if (buffered.length > 0) {
            const end = buffered.end(buffered.length - 1)
            const start = operation.startTime ?? buffered.start(buffered.length - 1)
            const bufferedGrew = end > start + 0.001
            if (bufferedGrew && !keyframeTimes.some((time) => Math.abs(time - start) < 0.01)) {
              keyframeTimes.push(start)
              diag.log('keyframeBuffered', {
                time: start,
                segmentIndex: partial.segmentIndex,
                partIndex: partial.partIndex,
              })
            }
            if (keyframeTimes.length > 20) keyframeTimes.splice(0, keyframeTimes.length - 20)
          }
        }

        // Build enough reserve to absorb ordinary network/camera burstiness before
        // starting playback. Camera 11 regularly delivers 600-700ms gaps.
        if (!hasInitialBuffer) {
          const video = videoRef.value
          if (video && video.buffered.length > 0) {
            const start = video.buffered.start(0)
            const end = video.buffered.end(video.buffered.length - 1)
            const reserve = end - start
            if (reserve >= MSE_TARGET_HEADROOM_SECONDS) {
              hasInitialBuffer = true
              video.currentTime = start
              video.play().catch(() => {})
              diag.log('initialSeek', { time: video.currentTime, reserve })
            }
          }
        }

        // Periodically trim back buffer
        const video = videoRef.value
        if (video && video.currentTime > MSE_BACK_BUFFER_SECONDS + 2) {
          trimBuffer()
        }

        processAppendQueue()
      })
      buffer.addEventListener('error', () => {
        if (sourceBuffer !== buffer || sourceBufferGeneration !== generation) return
        if (sourceBufferOperation?.buffer === buffer) sourceBufferOperation.failed = true
        diag.stats.errorCount++
        consecutiveAppendErrors++
        diag.log('sourceBufferError', { consecutive: consecutiveAppendErrors })
        if (consecutiveAppendErrors >= 5) {
          restartStream('sourceBufferErrors')
        }
      })
      buffer.addEventListener('abort', () => {
        if (sourceBuffer !== buffer || sourceBufferGeneration !== generation) return
        if (sourceBufferOperation?.buffer === buffer) sourceBufferOperation.failed = true
      })
      diag.log('sourceBufferCreated', { mimeType })
      return true
    } catch (e: any) {
      diag.stats.errorCount++
      diag.log('sourceBufferCreateFailed', { message: e.message })
      return false
    }
  }

  function handleControlMessage(json: string) {
    try {
      const msg = JSON.parse(json)
      switch (msg.type) {
        case 'initSegment':
          initGeneration = msg.generation
          audioCodec = typeof msg.audioCodec === 'string' ? msg.audioCodec : undefined
          expectingBinary = 'init'
          pendingBinaryIntegrity = {
            expectedBytes: Number.isFinite(Number(msg.bytes)) ? Number(msg.bytes) : null,
            expectedHash: typeof msg.hash === 'string' ? msg.hash : undefined,
          }
          diag.log('initSegment', {
            generation: msg.generation,
            audioCodec,
            expectedBytes: pendingBinaryIntegrity.expectedBytes,
            expectedHash: pendingBinaryIntegrity.expectedHash,
          })
          break

        case 'partial':
          // Skip non-independent partials until first keyframe
          if (waitingForKeyframe && !msg.independent) {
            expectingBinary = null // Will discard the binary frame
            pendingPartialMetadata = null
            diag.log('skippedPartial', {
              segmentIndex: msg.segmentIndex,
              partIndex: msg.partIndex,
              reason: 'waitingForKeyframe',
            })
            break
          }
          if (waitingForKeyframe && msg.independent) {
            waitingForKeyframe = false
            diag.log('keyframeFound', {
              segmentIndex: msg.segmentIndex,
              partIndex: msg.partIndex,
            })
          }
          expectingBinary = 'partial'
          pendingPartialMetadata = {
            duration: Number(msg.duration) || 0,
            independent: Boolean(msg.independent),
            keyframeSeekSafe: Boolean(msg.keyframeSeekSafe),
            segmentIndex: Number(msg.segmentIndex),
            partIndex: Number(msg.partIndex),
            expectedBytes: Number.isFinite(Number(msg.bytes)) ? Number(msg.bytes) : null,
            expectedHash: typeof msg.hash === 'string' ? msg.hash : undefined,
          }
          diag.log('partial', {
            segmentIndex: msg.segmentIndex,
            partIndex: msg.partIndex,
            duration: msg.duration,
            independent: msg.independent,
            expectedBytes: pendingPartialMetadata.expectedBytes,
            expectedHash: pendingPartialMetadata.expectedHash,
          })
          break

        case 'segment':
          diag.log('segmentComplete', {
            segmentIndex: msg.segmentIndex,
            duration: msg.duration,
          })
          break

        case 'discontinuity':
          initGeneration = msg.generation
          diag.log('discontinuity', { generation: msg.generation })
          handleDiscontinuity()
          break
      }
    } catch {
      diag.log('invalidControlMessage')
    }
  }

  function verifyBinaryIntegrity(
    data: ArrayBuffer,
    kind: 'init' | 'partial',
    metadata: BinaryIntegrityMetadata | null,
  ) {
    if (!metadata || (metadata.expectedBytes === null && !metadata.expectedHash)) return
    const actualHash = metadata.expectedHash ? fnv1a32(data) : undefined
    const sizeMatches = metadata.expectedBytes === null || metadata.expectedBytes === data.byteLength
    const hashMatches = !metadata.expectedHash || metadata.expectedHash === actualHash
    if (sizeMatches && hashMatches) {
      diag.stats.verifiedBinaryMessages++
      return
    }
    diag.stats.integrityMismatchCount++
    diag.log('fragmentIntegrityMismatch', {
      kind,
      segmentIndex: metadata.segmentIndex,
      partIndex: metadata.partIndex,
      expectedBytes: metadata.expectedBytes,
      actualBytes: data.byteLength,
      expectedHash: metadata.expectedHash,
      actualHash,
    })
  }

  function handleBinaryData(data: ArrayBuffer) {
    if (expectingBinary === 'init') {
      verifyBinaryIntegrity(data, 'init', pendingBinaryIntegrity)
      // Init segment — create or reset source buffer and append
      if (!sourceBuffer) {
        createSourceBuffer()
      }
      if (sourceBuffer) {
        appendData(data)
      } else {
        // MediaSource not open yet — buffer the init data for sourceopen handler
        pendingInitData = data
        diag.log('initBuffered', { reason: 'mediaSourceNotOpen' })
      }
      expectingBinary = null
      pendingBinaryIntegrity = null
    } else if (expectingBinary === 'partial') {
      verifyBinaryIntegrity(data, 'partial', pendingPartialMetadata)
      if (sourceBuffer) {
        // appendBuffer can fail synchronously and restart the pipeline. Do not
        // write fresh state into a generation that appendData just tore down.
        if (appendData(data, pendingPartialMetadata ?? undefined)) {
          lastFragTime = Date.now()
          diag.stats.totalFragments++
          isActive.value = true
        }
      }
      expectingBinary = null
      pendingPartialMetadata = null
    } else {
      // Binary frame for a skipped partial (no keyframe yet) — discard silently
    }
  }

  function handleDiscontinuity() {
    // Full teardown of media pipeline — removeSourceBuffer alone can leave
    // MediaSource in a corrupted state after camera reconnects
    if (sourceBuffer && mediaSource && mediaSource.readyState === 'open') {
      try {
        mediaSource.removeSourceBuffer(sourceBuffer)
      } catch {
        // May fail if already closed
      }
    }
    sourceBufferGeneration++
    sourceBuffer = null
    appendQueue = []
    sourceBufferOperation = null
    lastTrimmedTo = 0

    const element = videoRef.value
    if (element && mediaSource) {
      URL.revokeObjectURL(element.src)
      element.removeAttribute('src')
      element.load()
      mediaSource = null
      // Re-create MediaSource for fresh start
      setupMediaSource(element)
    }

    waitingForKeyframe = true
    hasInitialBuffer = false
    pendingInitData = null
    pendingPartialMetadata = null
    pendingBinaryIntegrity = null
    consecutiveAppendErrors = 0
    lastFragTime = 0  // Reset so watchdog correctly detects stale connections
    streamStartTime = Date.now()
    lastCurrentTime = -1
    currentTimeStalledSince = 0
    lowReadyStateSince = 0
    highLatencySince = 0
    catchUpActive = false
    keyframeTimes = []
    lastKeyframeSeekTarget = -1
  }

  function connectWebSocket() {
    if (destroyed) return
    const element = videoRef.value
    if (!element) return

    // Prevent leaked WebSocket: close any existing connection before opening a new one
    if (ws) {
      intentionalClose = true
      ws.close()
      ws = null
    }

    if (!mediaSource) {
      setupMediaSource(element)
    }

    const url = getWsUrl()
    diag.log('connecting', { url })

    ws = new WebSocket(url)
    ws.binaryType = 'arraybuffer'

    ws.onopen = () => {
      diag.log('wsOpen')
      streamStartTime = Date.now()
      restartBackoffMs = 3000
    }

    ws.onmessage = (event: MessageEvent) => {
      if (typeof event.data === 'string') {
        handleControlMessage(event.data)
      } else if (event.data instanceof ArrayBuffer) {
        handleBinaryData(event.data)
      }
    }

    ws.onerror = () => {
      diag.stats.errorCount++
      diag.log('wsError')
    }

    ws.onclose = (event: CloseEvent) => {
      diag.log('wsClose', { code: event.code, reason: event.reason })
      ws = null
      isActive.value = false

      if (!destroyed && !intentionalClose) {
        // Full teardown — stale MediaSource/SourceBuffer can't be reused reliably
        if (sourceBuffer && mediaSource && mediaSource.readyState === 'open') {
          try {
            mediaSource.removeSourceBuffer(sourceBuffer)
          } catch {}
        }
        sourceBufferGeneration++
        sourceBuffer = null
        appendQueue = []
        sourceBufferOperation = null
        lastTrimmedTo = 0

        const el = videoRef.value
        if (el) {
          URL.revokeObjectURL(el.src)
          el.removeAttribute('src')
          el.load()
        }

        mediaSource = null
        lastFragTime = 0
        streamStartTime = Date.now()
        expectingBinary = null
        waitingForKeyframe = true
        hasInitialBuffer = false
        pendingInitData = null
        pendingPartialMetadata = null
        pendingBinaryIntegrity = null
        consecutiveAppendErrors = 0
        lastCurrentTime = -1
        currentTimeStalledSince = 0
        lowReadyStateSince = 0
        highLatencySince = 0
        catchUpActive = false
        keyframeTimes = []
        lastKeyframeSeekTarget = -1

        diag.stats.restartCount++
        diag.log('reconnect', { backoffMs: restartBackoffMs })
        reconnectTimer = setTimeout(() => {
          reconnectTimer = null
          connectWebSocket()
        }, restartBackoffMs)
        restartBackoffMs = Math.min(restartBackoffMs * 1.5, 30000)
      }
      intentionalClose = false
    }
  }

  function restartStream(reason: string) {
    if (destroyed) return
    diag.stats.restartCount++
    diag.log('restart', { reason, generation: initGeneration })

    // Cancel any pending reconnect timer to prevent duplicate WebSocket creation
    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }

    // Tear down everything
    intentionalClose = true
    ws?.close()
    ws = null

    if (sourceBuffer && mediaSource && mediaSource.readyState === 'open') {
      try {
        mediaSource.removeSourceBuffer(sourceBuffer)
      } catch {}
    }
    sourceBufferGeneration++
    sourceBuffer = null
    appendQueue = []
    sourceBufferOperation = null
    lastTrimmedTo = 0

    const element = videoRef.value
    if (element) {
      URL.revokeObjectURL(element.src)
      element.removeAttribute('src')
      element.load()
    }

    mediaSource = null
    lastFragTime = 0
    streamStartTime = Date.now()
    expectingBinary = null
    waitingForKeyframe = true
    hasInitialBuffer = false
    pendingInitData = null
    pendingPartialMetadata = null
    pendingBinaryIntegrity = null
    consecutiveAppendErrors = 0
    lastCurrentTime = -1
    currentTimeStalledSince = 0
    lowReadyStateSince = 0
    highLatencySince = 0
    catchUpActive = false
    keyframeTimes = []
    lastKeyframeSeekTarget = -1

    // Reconnect
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null
      connectWebSocket()
    }, restartBackoffMs)
    restartBackoffMs = Math.min(restartBackoffMs * 1.5, 30000)
  }

  // ── Watchdog ──────────────────────────────────────────────────────
  function startWatchdog() {
    watchdog = setInterval(() => {
      const video = videoRef.value
      if (!video || destroyed) return

      const now = Date.now()
      const hasFrags = lastFragTime > 0
      const fragAge = hasFrags ? now - lastFragTime : now - streamStartTime

      // Spinner: show during initial connect only, not during playback
      // (fragments flowing = stream is healthy, readyState dips are normal near live edge)
      showSpinner.value = !hasFrags && fragAge < MSE_INITIAL_TIMEOUT_MS

      // Connection lost: no fragments for extended period
      connectionLost.value = fragAge > MSE_INITIAL_TIMEOUT_MS

      // Initial timeout — never received first fragment
      if (!hasFrags && fragAge > MSE_INITIAL_TIMEOUT_MS) {
        diag.log('initialTimeout')
        restartStream('initialTimeout')
        return
      }

      // Auto-resume videos paused by the browser (e.g. background tab suspension)
      if (video.paused && hasFrags && hasInitialBuffer && !destroyed) {
        video.play().catch(() => {})
      }

      // Stuck detection: readyState low + no recent fragments
      if (hasFrags && video.readyState <= 2 && !video.paused) {
        if (lowReadyStateSince === 0) {
          lowReadyStateSince = now
        } else if (now - lowReadyStateSince > stuckBackoffMs && fragAge > stuckBackoffMs) {
          diag.stats.stallCount++
          diag.log('stuckRestart', { readyState: video.readyState, stuckMs: now - lowReadyStateSince })
          stuckBackoffMs = Math.min(stuckBackoffMs * 2, 10000)
          restartStream('stuck')
          return
        }
      } else if (video.readyState >= 3) {
        lowReadyStateSince = 0
        stuckBackoffMs = 3000
        restartBackoffMs = 3000
      }

      // Frozen-frame detection: currentTime not advancing while frags are flowing
      // Catches SourceBuffer corruption, silent decode failures, frozen video element
      if (hasFrags && !video.paused && video.readyState >= 1) {
        if (video.currentTime === lastCurrentTime && lastCurrentTime >= 0) {
          if (currentTimeStalledSince === 0) {
            currentTimeStalledSince = now
          } else if (now - currentTimeStalledSince > 3000 && fragAge < 2000) {
            diag.stats.stallCount++
            diag.log('frozenRestart', {
              currentTime: video.currentTime,
              stalledMs: now - currentTimeStalledSince,
              fragAge,
            })
            restartStream('frozen')
            return
          }
        } else {
          lastCurrentTime = video.currentTime
          currentTimeStalledSince = 0
        }
      }

      // Live edge tracking. Use one-way catch-up with broad hysteresis: the old
      // 50-100ms band was narrower than one partial and switched between 0.9x
      // and 1.1x too frequently. Never slow below the source rate.
      if (video.readyState >= 3 && !video.paused && sourceBuffer) {
        const buf = video.buffered
        if (buf.length > 0) {
          const rangeStart = buf.start(buf.length - 1)
          const end = buf.end(buf.length - 1)
          keyframeTimes = keyframeTimes.filter((time) => time >= rangeStart - 0.01 && time < end)

          let lag = end - video.currentTime
          const futureKeyframes = keyframeTimes.filter(
            (time) =>
              time > video.currentTime + 0.05 &&
              time >= rangeStart &&
              time < end - 0.01,
          )
          if (!video.seeking && futureKeyframes.length >= 2) {
            const target = futureKeyframes[futureKeyframes.length - 1]!
            if (target > lastKeyframeSeekTarget + 0.01) {
              const from = video.currentTime
              video.currentTime = target
              lastCurrentTime = target
              currentTimeStalledSince = 0
              highLatencySince = 0
              catchUpActive = true
              lastKeyframeSeekTarget = target
              lag = Math.max(0, end - target)
              diag.log('keyframeCatchUpSeek', {
                from,
                to: target,
                skippedKeyframes: futureKeyframes.length - 1,
                remainingLagMs: Math.round(lag * 1000),
              })
            }
          }

          latencyMs.value = Math.round(lag * 1000)
          diag.recordLatency(latencyMs.value)

          if (!catchUpActive && lag > MSE_CATCH_UP_START_SECONDS) {
            catchUpActive = true
            diag.log('catchUpStart', { lag, lagMs: latencyMs.value })
          } else if (catchUpActive && lag < MSE_TARGET_HEADROOM_SECONDS) {
            catchUpActive = false
            diag.log('catchUpEnd', { lag, lagMs: latencyMs.value })
          }

          const targetRate = catchUpActive
            ? (lag > MSE_CATCH_UP_START_SECONDS ? 1.08 : 1.03)
            : 1.0
          if (video.playbackRate !== targetRate) {
            video.playbackRate = targetRate
          }

          // Drift recovery: if latency exceeds 5s for 3+ seconds, full restart
          if (lag > 5) {
            if (highLatencySince === 0) highLatencySince = now
            if (now - highLatencySince > 3000) {
              diag.log('driftRestart', { lag, lagMs: latencyMs.value })
              restartStream('drift')
              return
            }
          } else {
            highLatencySince = 0
          }
        } else {
          highLatencySince = 0
          if (catchUpActive || video.playbackRate !== 1.0) {
            catchUpActive = false
            video.playbackRate = 1.0
            diag.log('catchUpCancelled', { readyState: video.readyState, reason: 'noBuffer' })
          }
        }
      } else {
        // Do not carry an accelerated rate or a partial drift timer through an
        // under-buffered interval; both would turn ordinary jitter into a stall.
        highLatencySince = 0
        if (catchUpActive || video.playbackRate !== 1.0) {
          catchUpActive = false
          video.playbackRate = 1.0
          diag.log('catchUpCancelled', { readyState: video.readyState })
        }
      }
    }, MSE_WATCHDOG_INTERVAL_MS)
  }

  // ── Lifecycle ─────────────────────────────────────────────────────
  function start() {
    const element = videoRef.value
    if (!element) return

    diag.setRef(element)
    diag.setStateGetter(() => ({
      showSpinner: showSpinner.value,
      connectionLost: connectionLost.value,
      isActive: isActive.value,
      latencyMs: latencyMs.value,
      lastFragAge: lastFragTime ? Date.now() - lastFragTime : null,
      restartBackoffMs,
      stuckBackoffMs,
      waitingForKeyframe,
      hasInitialBuffer,
      targetHeadroomSeconds: MSE_TARGET_HEADROOM_SECONDS,
      appendQueueLength: appendQueue.length,
      sourceBufferGeneration,
      sourceBufferUpdating: sourceBuffer?.updating ?? null,
      sourceBufferOperation: sourceBufferOperation?.kind ?? null,
      mediaSourceState: mediaSource?.readyState ?? null,
      lowReadyStateMs: lowReadyStateSince ? Date.now() - lowReadyStateSince : 0,
      initGeneration,
    }))
    element.muted = true
    // Playback begins explicitly once the initial reserve has accumulated.
    element.autoplay = false
    element.playbackRate = 1.0

    connectWebSocket()
    startWatchdog()
    diag.log('start')
  }

  function stop() {
    destroyed = true
    catchUpActive = false
    highLatencySince = 0

    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }

    ws?.close()
    ws = null

    if (watchdog) {
      clearInterval(watchdog)
      watchdog = null
    }

    if (sourceBuffer && mediaSource && mediaSource.readyState === 'open') {
      try {
        mediaSource.removeSourceBuffer(sourceBuffer)
      } catch {}
    }
    sourceBufferGeneration++
    sourceBuffer = null
    sourceBufferOperation = null
    lastTrimmedTo = 0

    const element = videoRef.value
    if (element) {
      URL.revokeObjectURL(element.src)
      element.removeAttribute('src')
      element.load()
    }

    mediaSource = null
    appendQueue = []
    pendingPartialMetadata = null
    pendingBinaryIntegrity = null
    keyframeTimes = []
    // A codec change remounts the player under the same diagnostic ID. Vue may
    // stop the old instance after the replacement has registered itself.
    if (diagMap[diagId] === diag) delete diagMap[diagId]
  }

  // Auto-start when video element is available
  const checkInterval = setInterval(() => {
    if (videoRef.value) {
      clearInterval(checkInterval)
      start()
    }
  }, 100)

  onUnmounted(() => {
    clearInterval(checkInterval)
    stop()
  })

  return { showSpinner, connectionLost, isActive, latencyMs, codecUnsupported }
}
