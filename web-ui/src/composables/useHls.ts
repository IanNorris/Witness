import { ref, onUnmounted, type Ref } from 'vue'
import Hls from 'hls.js'

// ── Constants ─────────────────────────────────────────────────────────
// Segments arrive every ~2s in bursts of partials. Spinner timeout must
// exceed the segment interval to avoid flickering between deliveries.
const HLS_SPINNER_TIMEOUT_MS = 3000
const HLS_RESTART_TIMEOUT_MS = 5000
const HLS_WATCHDOG_INTERVAL_MS = 250

// ── Diagnostics ───────────────────────────────────────────────────────
const DIAG_MAX_AGE_MS = 24 * 60 * 60 * 1000

interface DiagEvent {
  t: string
  type: string
  [key: string]: unknown
}

interface DiagStats {
  restartCount: number
  stallCount: number
  errorCount: number
  maxLatencyMs: number
  minLatencyMs: number | null
  totalFragments: number
}

class StreamDiagnostics {
  cameraID: string
  startTime: number
  events: DiagEvent[] = []
  stats: DiagStats = {
    restartCount: 0,
    stallCount: 0,
    errorCount: 0,
    maxLatencyMs: 0,
    minLatencyMs: null,
    totalFragments: 0,
  }
  _element: HTMLVideoElement | null = null
  _hls: Hls | null = null

  constructor(cameraID: string) {
    this.cameraID = cameraID
    this.startTime = Date.now()
  }

  setRefs(element: HTMLVideoElement, hls: Hls) {
    this._element = element
    this._hls = hls
  }

  log(type: string, extra?: Record<string, unknown>) {
    const entry: DiagEvent = { t: new Date().toISOString(), type }
    if (extra) Object.assign(entry, extra)
    this.events.push(entry)
    const cutoff = Date.now() - DIAG_MAX_AGE_MS
    while (this.events.length > 0 && new Date(this.events[0]!.t).getTime() < cutoff) {
      this.events.shift()
    }
  }

  recordLatency(latencyMs: number) {
    if (latencyMs > this.stats.maxLatencyMs) this.stats.maxLatencyMs = latencyMs
    if (this.stats.minLatencyMs === null || latencyMs < this.stats.minLatencyMs)
      this.stats.minLatencyMs = latencyMs
  }

  snapshot() {
    const el = this._element
    const hls = this._hls
    const bufferedRanges: [number, number][] = []
    if (el?.buffered) {
      for (let i = 0; i < el.buffered.length; i++) {
        bufferedRanges.push([el.buffered.start(i), el.buffered.end(i)])
      }
    }
    return {
      stats: { ...this.stats, uptimeMs: Date.now() - this.startTime },
      currentState: {
        readyState: el?.readyState ?? null,
        currentTime: el?.currentTime ?? null,
        bufferedRanges,
        paused: el?.paused ?? null,
        networkState: el?.networkState ?? null,
        hlsLiveSyncPosition: hls?.liveSyncPosition ?? null,
      },
      events: this.events,
    }
  }
}

// Global diagnostics registry
const diagMap = ((window as unknown as Record<string, unknown>)._witnessDiag ??= {}) as Record<string, StreamDiagnostics>
;(window as unknown as Record<string, unknown>)._witnessDumpDiag = function () {
  const dump = {
    timestamp: new Date().toISOString(),
    userAgent: navigator.userAgent,
    cameras: {} as Record<string, unknown>,
  }
  for (const id in diagMap) {
    dump.cameras[id] = diagMap[id]!.snapshot()
  }
  const blob = new Blob([JSON.stringify(dump, null, 2)], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = 'witness-hls-debug-' + new Date().toISOString().replace(/[:.]/g, '-') + '.json'
  document.body.appendChild(a)
  a.click()
  document.body.removeChild(a)
  URL.revokeObjectURL(url)
}

// ── HLS Config ────────────────────────────────────────────────────────
function createHlsConfig(debug: boolean): Partial<Hls['config']> {
  return {
    debug,
    enableWorker: true,
    lowLatencyMode: true,
    liveDurationInfinity: true,
    backBufferLength: 5,
    liveSyncDuration: 2,
    liveMaxLatencyDuration: 5,
    maxLiveSyncPlaybackRate: 1.05,
  }
}

// ── Composable ────────────────────────────────────────────────────────
export interface HlsStreamState {
  showSpinner: Ref<boolean>
  connectionLost: Ref<boolean>
  isActive: Ref<boolean>
}

export function useHls(
  cameraId: number,
  videoRef: Ref<HTMLVideoElement | null>,
  suffix: string = '',
  debug: boolean = false,
): HlsStreamState {
  const showSpinner = ref(false)
  const connectionLost = ref(false)
  const isActive = ref(false)

  const diagId = String(cameraId) + suffix
  const sourceUrl = `/stream/${cameraId}`
  const diag = new StreamDiagnostics(diagId)
  diagMap[diagId] = diag

  let hls: Hls | null = null
  let lastFragTime = 0
  let streamStartTime = Date.now()
  let restartInProgress = false
  let watchdog: ReturnType<typeof setInterval> | null = null
  let lowReadyStateSince = 0
  let stuckBackoffMs = 3000
  let destroyed = false

  function attachEvents(h: Hls, element: HTMLVideoElement) {
    h.on(Hls.Events.FRAG_BUFFERED, () => {
      lastFragTime = Date.now()
      diag.stats.totalFragments++
      let latencyMs: number | null = null
      if (h.liveSyncPosition != null) {
        latencyMs = Math.round((h.liveSyncPosition - element.currentTime) * 1000)
        diag.recordLatency(latencyMs)
      }
      diag.log('frag', { latencyMs })
      isActive.value = true
    })

    h.on(Hls.Events.LEVEL_UPDATED, () => {
      diag.log('levelUpdated')
      isActive.value = true
    })

    h.on(Hls.Events.ERROR, (_event, data) => {
      diag.stats.errorCount++
      diag.log('error', { detail: data.details, fatal: data.fatal, type: data.type })
      if (data.fatal) {
        isActive.value = false
        if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
          h.recoverMediaError()
        }
      }
    })

    h.on(Hls.Events.MEDIA_ATTACHED, () => {
      element.muted = true
      element.play()
      diag.log('mediaAttached')
      restartInProgress = false
    })
  }

  function restartStream() {
    const element = videoRef.value
    if (restartInProgress || !element || destroyed) return
    restartInProgress = true
    diag.stats.restartCount++
    diag.log('restart', { reason: 'timeout' })
    hls?.destroy()
    hls = new Hls(createHlsConfig(debug))
    diag.setRefs(element, hls)
    attachEvents(hls, element)
    lastFragTime = 0
    streamStartTime = Date.now()
    hls.loadSource(sourceUrl)
    hls.attachMedia(element)
  }

  function start() {
    const element = videoRef.value
    if (!element || !Hls.isSupported() || destroyed) return

    hls = new Hls(createHlsConfig(debug))
    diag.setRefs(element, hls)
    diag.log('start')
    attachEvents(hls, element)
    hls.loadSource(sourceUrl)
    hls.attachMedia(element)

    // Poll-based watchdog: drives spinner, connection-lost, and restart.
    // We do NOT seek the playhead — HLS.js has its own recovery for
    // buffer gaps (bufferSeekOverHole, nudgeOnStall). Seeking from
    // outside fights those mechanisms and causes thrashing loops.
    watchdog = setInterval(() => {
      if (!element) return

      // Detect sustained low readyState with exponential backoff
      if (!element.paused && element.readyState <= 1 && lastFragTime > 0) {
        if (lowReadyStateSince === 0) {
          lowReadyStateSince = Date.now()
        } else if (Date.now() - lowReadyStateSince > stuckBackoffMs) {
          diag.log('stuckRestart', {
            readyState: element.readyState,
            currentTime: element.currentTime,
            liveSyncPosition: hls?.liveSyncPosition,
            backoffMs: stuckBackoffMs,
          })
          lowReadyStateSince = 0
          stuckBackoffMs = Math.min(stuckBackoffMs * 2, 10000)
          restartStream()
          return
        }
      } else {
        lowReadyStateSince = 0
        if (element.readyState >= 3) stuckBackoffMs = 3000
      }

      if (lastFragTime === 0) {
        // Initial connection — show spinner while waiting for first fragment
        const sinceLaunch = Date.now() - streamStartTime
        showSpinner.value = true
        connectionLost.value = false
        if (sinceLaunch > HLS_RESTART_TIMEOUT_MS) {
          connectionLost.value = true
          showSpinner.value = false
          if (!restartInProgress) {
            diag.stats.stallCount++
            diag.log('initialTimeout', { sinceLaunchMs: sinceLaunch })
            restartStream()
          }
        }
        return
      }

      const elapsed = Date.now() - lastFragTime

      if (elapsed <= HLS_SPINNER_TIMEOUT_MS) {
        // Fragments flowing normally — no indicators
        showSpinner.value = false
        connectionLost.value = false
      } else {
        // Fragments stopped — show spinner briefly, then connection lost
        showSpinner.value = elapsed <= HLS_RESTART_TIMEOUT_MS
        connectionLost.value = elapsed > HLS_RESTART_TIMEOUT_MS
        if (elapsed > HLS_RESTART_TIMEOUT_MS && !restartInProgress) {
          diag.stats.stallCount++
          diag.log('stall', { timeSinceLastFragMs: elapsed })
          restartStream()
        }
      }
    }, HLS_WATCHDOG_INTERVAL_MS)
  }

  function stop() {
    destroyed = true
    if (watchdog) clearInterval(watchdog)
    hls?.destroy()
    hls = null
    delete diagMap[diagId]
  }

  onUnmounted(stop)

  // Auto-start when videoRef becomes available
  const checkInterval = setInterval(() => {
    if (videoRef.value) {
      clearInterval(checkInterval)
      start()
    }
  }, 50)

  onUnmounted(() => clearInterval(checkInterval))

  return { showSpinner, connectionLost, isActive }
}
