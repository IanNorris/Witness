import { ref, onUnmounted, type Ref } from 'vue'

// ── Constants ─────────────────────────────────────────────────────────
const MSE_WATCHDOG_INTERVAL_MS = 250
const MSE_INITIAL_TIMEOUT_MS = 5000
const MSE_BACK_BUFFER_SECONDS = 5

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
}

class MseDiagnostics {
  cameraID: string
  startTime: number
  events: DiagEvent[] = []
  stats: MseDiagStats = {
    restartCount: 0,
    stallCount: 0,
    errorCount: 0,
    totalFragments: 0,
    maxLatencyMs: 0,
    minLatencyMs: null,
  }
  _element: HTMLVideoElement | null = null

  constructor(cameraID: string) {
    this.cameraID = cameraID
    this.startTime = Date.now()
  }

  setRef(element: HTMLVideoElement) {
    this._element = element
  }

  log(type: string, extra?: Record<string, unknown>) {
    const event: DiagEvent = {
      t: new Date().toISOString(),
      type,
      ...extra,
    }
    this.events.push(event)
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
  }

  snapshot() {
    const el = this._element
    return {
      cameraID: this.cameraID,
      startTime: new Date(this.startTime).toISOString(),
      stats: { ...this.stats },
      currentState: el
        ? {
            readyState: el.readyState,
            currentTime: el.currentTime,
            paused: el.paused,
            ended: el.ended,
            buffered:
              el.buffered.length > 0
                ? Array.from({ length: el.buffered.length }, (_, i) => [
                    el.buffered.start(i),
                    el.buffered.end(i),
                  ])
                : [],
          }
        : null,
      events: this.events.slice(-100),
    }
  }
}

// Global diagnostics map (shared with HLS diagnostics via window._witnessDiag)
const diagMap: Record<string, MseDiagnostics> = {}

// Register global dump function
if (typeof window !== 'undefined') {
  ;(window as any)._witnessMseDiag = diagMap
  const existingDumpAll = (window as any)._witnessDumpAll
  ;(window as any)._witnessDumpAll = async function () {
    const result: any = existingDumpAll ? await existingDumpAll() : {}
    result.mse = {}
    for (const [id, d] of Object.entries(diagMap)) {
      result.mse[id] = d.snapshot()
    }
    return result
  }
}

// ── Composable ────────────────────────────────────────────────────────
export interface MseStreamState {
  showSpinner: Ref<boolean>
  connectionLost: Ref<boolean>
  isActive: Ref<boolean>
  latencyMs: Ref<number>
}

export function useMseStream(
  cameraId: number,
  videoRef: Ref<HTMLVideoElement | null>,
  suffix: string = '',
): MseStreamState {
  const showSpinner = ref(false)
  const connectionLost = ref(false)
  const isActive = ref(false)
  const latencyMs = ref(0)

  const diagId = String(cameraId) + (suffix ? '_' + suffix : '_mse')
  const diag = new MseDiagnostics(diagId)
  diagMap[diagId] = diag

  let ws: WebSocket | null = null
  let mediaSource: MediaSource | null = null
  let sourceBuffer: SourceBuffer | null = null
  let appendQueue: ArrayBuffer[] = []
  let lastFragTime = 0
  let streamStartTime = Date.now()
  let watchdog: ReturnType<typeof setInterval> | null = null
  let restartBackoffMs = 3000
  let stuckBackoffMs = 3000
  let lowReadyStateSince = 0
  let destroyed = false
  let initGeneration = -1
  let expectingBinary: 'init' | 'partial' | null = null
  let waitingForKeyframe = true  // Skip partials until first independent (keyframe)
  let hasInitialBuffer = false   // Seek to buffered range after first append

  function getWsUrl(): string {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
    return `${proto}//${location.host}/ws/stream/${cameraId}`
  }

  function processAppendQueue() {
    if (!sourceBuffer || sourceBuffer.updating || appendQueue.length === 0) return
    const data = appendQueue.shift()!
    try {
      sourceBuffer.appendBuffer(data)
    } catch (e: any) {
      if (e.name === 'QuotaExceededError') {
        diag.log('quotaExceeded')
        trimBuffer(true)
        // Re-queue and retry
        appendQueue.unshift(data)
        setTimeout(() => processAppendQueue(), 100)
      } else {
        diag.stats.errorCount++
        diag.log('appendError', { message: e.message })
      }
    }
  }

  function appendData(data: ArrayBuffer) {
    appendQueue.push(data)
    processAppendQueue()
  }

  function trimBuffer(aggressive = false) {
    if (!sourceBuffer || sourceBuffer.updating) return
    const video = videoRef.value
    if (!video) return

    const trimTo = video.currentTime - (aggressive ? 1 : MSE_BACK_BUFFER_SECONDS)
    if (trimTo > 0 && sourceBuffer.buffered.length > 0) {
      try {
        sourceBuffer.remove(0, trimTo)
      } catch {
        // May throw if already updating
      }
    }
  }

  function setupMediaSource(element: HTMLVideoElement) {
    mediaSource = new MediaSource()
    element.src = URL.createObjectURL(mediaSource)

    mediaSource.addEventListener('sourceopen', () => {
      diag.log('sourceOpen')
      // SourceBuffer is created when we receive the init segment
      // and know the codec from the control message
    })

    mediaSource.addEventListener('sourceended', () => {
      diag.log('sourceEnded')
    })

    mediaSource.addEventListener('sourceclose', () => {
      diag.log('sourceClose')
    })
  }

  function createSourceBuffer(codec?: string) {
    if (!mediaSource || mediaSource.readyState !== 'open') return false

    // Default codec for H.264 baseline — init segment will correct if different
    const mimeType = `video/mp4; codecs="${codec || 'avc1.42001e'}"`

    if (!MediaSource.isTypeSupported(mimeType)) {
      diag.log('unsupportedCodec', { mimeType })
      return false
    }

    try {
      sourceBuffer = mediaSource.addSourceBuffer(mimeType)
      sourceBuffer.mode = 'sequence'
      sourceBuffer.addEventListener('updateend', () => {
        processAppendQueue()

        // After first data is buffered, start playback from keyframe at buffer start
        // The watchdog's seekToLive will catch up to live edge within 250ms
        if (!hasInitialBuffer) {
          const video = videoRef.value
          if (video && video.buffered.length > 0) {
            hasInitialBuffer = true
            video.currentTime = video.buffered.start(0)
            video.play().catch(() => {})
            diag.log('initialSeek', { time: video.currentTime })
          }
        }

        // Periodically trim back buffer
        const video = videoRef.value
        if (video && video.currentTime > MSE_BACK_BUFFER_SECONDS + 2) {
          trimBuffer()
        }
      })
      sourceBuffer.addEventListener('error', () => {
        diag.stats.errorCount++
        diag.log('sourceBufferError')
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
          expectingBinary = 'init'
          diag.log('initSegment', { generation: msg.generation })
          break

        case 'partial':
          // Skip non-independent partials until first keyframe
          if (waitingForKeyframe && !msg.independent) {
            expectingBinary = null // Will discard the binary frame
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
          diag.log('partial', {
            segmentIndex: msg.segmentIndex,
            partIndex: msg.partIndex,
            duration: msg.duration,
            independent: msg.independent,
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

  function handleBinaryData(data: ArrayBuffer) {
    if (expectingBinary === 'init') {
      // Init segment — create or reset source buffer and append
      if (!sourceBuffer) {
        createSourceBuffer()
      }
      if (sourceBuffer) {
        appendData(data)
      }
      expectingBinary = null
    } else if (expectingBinary === 'partial') {
      if (sourceBuffer) {
        appendData(data)
        lastFragTime = Date.now()
        diag.stats.totalFragments++
        isActive.value = true
      }
      expectingBinary = null
    } else {
      // Binary frame for a skipped partial (no keyframe yet) — discard silently
    }
  }

  function handleDiscontinuity() {
    // Clear source buffer for new init segment
    if (sourceBuffer && mediaSource && mediaSource.readyState === 'open') {
      try {
        mediaSource.removeSourceBuffer(sourceBuffer)
      } catch {
        // May fail if already closed
      }
      sourceBuffer = null
      appendQueue = []
    }
    waitingForKeyframe = true
    hasInitialBuffer = false
  }

  function connectWebSocket() {
    if (destroyed) return
    const element = videoRef.value
    if (!element) return

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

      if (!destroyed) {
        // Reconnect with backoff
        diag.stats.restartCount++
        diag.log('reconnect', { backoffMs: restartBackoffMs })
        setTimeout(() => connectWebSocket(), restartBackoffMs)
        restartBackoffMs = Math.min(restartBackoffMs * 1.5, 30000)
      }
    }
  }

  function restartStream(reason: string) {
    if (destroyed) return
    diag.stats.restartCount++
    diag.log('restart', { reason, generation: initGeneration })

    // Tear down everything
    ws?.close()
    ws = null

    if (sourceBuffer && mediaSource && mediaSource.readyState === 'open') {
      try {
        mediaSource.removeSourceBuffer(sourceBuffer)
      } catch {}
    }
    sourceBuffer = null
    appendQueue = []

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

    // Reconnect
    setTimeout(() => connectWebSocket(), restartBackoffMs)
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

      // Live edge tracking — just measure latency (no seeking or rate changes)
      if (video.readyState >= 3 && !video.paused && sourceBuffer) {
        const buf = video.buffered
        if (buf.length > 0) {
          const end = buf.end(buf.length - 1)
          const lag = end - video.currentTime
          latencyMs.value = Math.round(lag * 1000)
          diag.recordLatency(latencyMs.value)
        }
      }
    }, MSE_WATCHDOG_INTERVAL_MS)
  }

  // ── Lifecycle ─────────────────────────────────────────────────────
  function start() {
    const element = videoRef.value
    if (!element) return

    diag.setRef(element)
    element.muted = true
    element.autoplay = true

    connectWebSocket()
    startWatchdog()
    diag.log('start')
  }

  function stop() {
    destroyed = true
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
    sourceBuffer = null

    const element = videoRef.value
    if (element) {
      URL.revokeObjectURL(element.src)
      element.removeAttribute('src')
      element.load()
    }

    mediaSource = null
    appendQueue = []
    delete diagMap[diagId]
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

  return { showSpinner, connectionLost, isActive, latencyMs }
}
