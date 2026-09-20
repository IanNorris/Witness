import { ref, onUnmounted, type Ref } from 'vue'

// ── Constants ─────────────────────────────────────────────────────────
const MSE_WATCHDOG_INTERVAL_MS = 250
const MSE_WATCHDOG_SCHEDULING_GRACE_MS = 1000
const MSE_INITIAL_TIMEOUT_MS = 10000
const MSE_APPEND_TIMEOUT_MS = 10000
const MSE_MAX_APPEND_QUEUE_ITEMS = 128
const MSE_MAX_APPEND_QUEUE_BYTES = 32 * 1024 * 1024
const MSE_SOCKET_CLOSE_TIMEOUT_MS = 3000
const MSE_HEALTHY_PLAYBACK_RESET_MS = 5000
const MSE_BACK_BUFFER_SECONDS = 5
const MSE_TARGET_HEADROOM_SECONDS = 1.25
const MSE_CATCH_UP_START_SECONDS = 1.75

// ── Diagnostics ───────────────────────────────────────────────────────
const DIAG_MAX_AGE_MS = 24 * 60 * 60 * 1000
const DIAG_MAX_EVENTS = 1000
const DIAG_MAX_IMPORTANT_EVENTS = 500

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
  decodeCorruptionCount: number
  renderSuppressionCount: number
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
    decodeCorruptionCount: 0,
    renderSuppressionCount: 0,
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
    if (this.events.length > DIAG_MAX_EVENTS) {
      this.events.splice(0, this.events.length - DIAG_MAX_EVENTS)
    }
    if (this.importantEvents.length > DIAG_MAX_IMPORTANT_EVENTS) {
      this.importantEvents.splice(
        0,
        this.importantEvents.length - DIAG_MAX_IMPORTANT_EVENTS,
      )
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
    const result: any = existingDumpAll ? await existingDumpAll() : { client: { timestamp: new Date().toISOString(), cameras: {} }, serverJson: null }
    // Add MSE diagnostics to client cameras
    if (!result.client) result.client = { cameras: {} }
    if (!result.client.cameras) result.client.cameras = {}
    for (const [id, d] of Object.entries(diagMap)) {
      result.client.cameras[id] = d.snapshot()
    }
    // Download the combined dump
    const serverJson = typeof result.serverJson === 'string'
      ? result.serverJson
      : JSON.stringify(result.server ?? null)
    // Blob accepts multiple string parts, so the large server snapshot never
    // needs to be parsed into a JS object or copied into a pretty-printed string.
    const blob = new Blob(
      ['{"client":', JSON.stringify(result.client), ',"server":', serverJson, '}'],
      { type: 'application/json' },
    )
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
  renderSuppressed: Ref<boolean>
	selectedStream: Ref<'main' | 'sub'>
	updateViewport: (width: number, height: number) => void
}

interface PartialAppendMetadata {
  duration: number
  independent: boolean
  keyframeSeekSafe: boolean
  segmentIndex: number
  partIndex: number
  expectedBytes: number | null
  expectedHash?: string
  releaseRendering?: boolean
  renderSuppressionToken?: number
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
  queuedAt: number
}

type SourceBufferOperation =
  | {
      kind: 'append'
      item: AppendQueueItem
      startTime: number | null
      buffer: SourceBuffer
      generation: number
      startedAt: number
      failed: boolean
    }
  | {
      kind: 'remove'
      removeTo: number
      buffer: SourceBuffer
      generation: number
      startedAt: number
      failed: boolean
    }

export function useMseStream(
  cameraId: number,
  videoRef: Ref<HTMLVideoElement | null>,
  suffix: string = '',
  useSubStream: boolean = false,
  codecHint?: string,
  audioEnabled: () => boolean = () => false,
	adaptiveStream: boolean = false,
	viewportSize: () => { width: number; height: number } | null = () => null,
): MseStreamState {
  const showSpinner = ref(false)
  const connectionLost = ref(false)
  const isActive = ref(false)
  const latencyMs = ref(0)
  const codecUnsupported = ref(false)
  const renderSuppressed = ref(false)
	const selectedStream = ref<'main' | 'sub'>(useSubStream ? 'sub' : 'main')

  const diagId = String(cameraId) + (suffix ? '_' + suffix : '_mse')
  const diag = new MseDiagnostics(diagId)
  diagMap[diagId] = diag

  let ws: WebSocket | null = null
  let mediaSource: MediaSource | null = null
  let sourceBuffer: SourceBuffer | null = null
  let appendQueue: AppendQueueItem[] = []
  let appendQueueBytes = 0
  let sourceBufferOperation: SourceBufferOperation | null = null
  let sourceBufferGeneration = 0
  let lastTrimmedTo = 0
  let lastFragTime = 0
  let streamStartTime = Date.now()
  let wsOpenedAt = 0
  let pipelineStartedAt = streamStartTime
  let lastAppendCompletedAt = 0
  let healthyPlaybackSince = 0
  let lastWatchdogTick = Date.now()
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
  let reconnectAt = 0
  let closeFallbackTimer: ReturnType<typeof setTimeout> | null = null
  let pendingRestartDelayMs: number | null = null
  let initGeneration = -1
	let awaitingInit = true
  let expectingBinary: 'init' | 'partial' | null = null
  let pendingBinaryIntegrity: BinaryIntegrityMetadata | null = null
  let pendingPartialMetadata: PartialAppendMetadata | null = null
  let waitingForKeyframe = true  // Skip partials until first independent (keyframe)
  let hasInitialBuffer = false   // Seek to buffered range after first append
  let pendingInitData: ArrayBuffer | null = null  // Buffered init segment when MediaSource isn't open yet
  let consecutiveAppendErrors = 0  // Track consecutive appendBuffer failures for restart
  let keyframeTimes: number[] = []
  let lastKeyframeSeekTarget = -1
  let lastAnomalyReportTime = 0
  let lastAnomalyReason = ''
  let releaseRenderOnIndependent = false
  let renderSuppressionToken = 0
	let activeCodecHint = codecHint
	let hasReceivedStreamSelection = false
	let lastViewportWidth = 0
	let lastViewportHeight = 0

  function clearRenderSuppression() {
    renderSuppressionToken++
    renderSuppressed.value = false
    releaseRenderOnIndependent = false
  }

  function releaseRenderingAfterFrame(targetTime: number, generation: number, suppressionToken: number) {
    const video = videoRef.value
    if (!video) {
      clearRenderSuppression()
      return
    }

    const releaseIfReady = (mediaTime: number) => {
      if (!renderSuppressed.value || destroyed || sourceBufferGeneration !== generation ||
        renderSuppressionToken !== suppressionToken) return true
      if (mediaTime + 0.01 < targetTime) return false
      renderSuppressed.value = false
      diag.log('renderResumed', { targetTime, mediaTime, generation })
      return true
    }

    if (typeof video.requestVideoFrameCallback === 'function') {
      const waitForFrame = (_now: number, metadata: VideoFrameCallbackMetadata) => {
        if (!releaseIfReady(metadata.mediaTime)) video.requestVideoFrameCallback(waitForFrame)
      }
      video.requestVideoFrameCallback(waitForFrame)
    } else {
      const waitForTime = () => {
        if (!releaseIfReady(video.currentTime)) setTimeout(waitForTime, 50)
      }
      setTimeout(waitForTime, 50)
    }
  }

  function getWsUrl(): string {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
    const path = useSubStream ? `/ws/stream/sub/${cameraId}` : `/ws/stream/${cameraId}`
		const viewport = adaptiveStream && !useSubStream ? viewportSize() : null
		const query = viewport && viewport.width > 0 && viewport.height > 0
			? `?width=${viewport.width}&height=${viewport.height}`
			: ''
		return `${proto}//${location.host}${path}${query}`
  }

	function updateViewport(width: number, height: number) {
		if (!adaptiveStream || useSubStream) return
		const roundedWidth = Math.max(0, Math.min(16384, Math.round(width)))
		const roundedHeight = Math.max(0, Math.min(16384, Math.round(height)))
		if (roundedWidth <= 0 || roundedHeight <= 0 ||
			(roundedWidth === lastViewportWidth && roundedHeight === lastViewportHeight)) return
		if (!ws || ws.readyState !== WebSocket.OPEN) return
		ws.send(JSON.stringify({ type: 'viewport', width: roundedWidth, height: roundedHeight }))
		lastViewportWidth = roundedWidth
		lastViewportHeight = roundedHeight
		diag.log('viewportChanged', { width: roundedWidth, height: roundedHeight })
	}

	function codecHintForName(codec: unknown): string | undefined {
		if (typeof codec !== 'string') return undefined
		const name = codec.toLowerCase()
		if (name === 'hevc' || name === 'h265' || name === 'hev1' || name === 'hvc1')
			return 'hev1.1.6.L93.B0'
		if (name === 'h264' || name === 'avc' || name === 'avc1') return 'avc1.42001e'
		return undefined
	}

  function clearAppendQueue() {
    appendQueue = []
    appendQueueBytes = 0
  }

  function scheduleReconnect(delayMs: number) {
    if (destroyed) return 0
    const jitteredDelay = Math.max(250, Math.round(delayMs * (0.85 + Math.random() * 0.3)))
    reconnectAt = Date.now() + jitteredDelay
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null
      reconnectAt = 0
      connectWebSocket()
    }, jitteredDelay)
    return jitteredDelay
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
    appendQueueBytes = Math.max(0, appendQueueBytes - item.data.byteLength)
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
      startedAt: Date.now(),
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
        appendQueueBytes += item.data.byteLength
        setTimeout(() => processAppendQueue(), 100)
      } else {
        diag.stats.errorCount++
        consecutiveAppendErrors++
			if (item.partial?.releaseRendering) {
				waitingForKeyframe = true
				clearAppendQueue()
			}
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
    const nextBytes = appendQueueBytes + data.byteLength
    if (appendQueue.length >= MSE_MAX_APPEND_QUEUE_ITEMS || nextBytes > MSE_MAX_APPEND_QUEUE_BYTES) {
      diag.log('appendQueueLimit', {
        items: appendQueue.length,
        bytes: appendQueueBytes,
        incomingBytes: data.byteLength,
      })
      restartStream('appendQueueLimit')
      return false
    }
    appendQueue.push({ data, partial, queuedAt: Date.now() })
    appendQueueBytes = nextBytes
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
        startedAt: Date.now(),
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
    const instance = new MediaSource()
    mediaSource = instance
    element.src = URL.createObjectURL(instance)

    instance.addEventListener('sourceopen', () => {
      if (destroyed || mediaSource !== instance) return
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

    instance.addEventListener('sourceended', () => {
      if (destroyed || mediaSource !== instance) return
      diag.log('sourceEnded')
    })

    instance.addEventListener('sourceclose', () => {
      if (destroyed || mediaSource !== instance) return
      diag.log('sourceClose')
    })
  }

  let audioCodec: string | undefined

  function createSourceBuffer(codec?: string) {
    if (!mediaSource || mediaSource.readyState !== 'open') return false

    // Use explicit codec if provided, then codecHint from caller, then default H.264 baseline
		const resolvedCodec = codec || activeCodecHint || 'avc1.42001e'
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
			if (operation.item.partial?.releaseRendering) {
				waitingForKeyframe = true
				clearAppendQueue()
			}
          processAppendQueue()
          return
        }

        consecutiveAppendErrors = 0  // Reset only after a successful append
        lastAppendCompletedAt = Date.now()

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

        if (partial?.releaseRendering) {
			releaseRenderOnIndependent = false
          const targetTime = operation.startTime ?? videoRef.value?.currentTime ?? 0
          releaseRenderingAfterFrame(targetTime, generation, partial.renderSuppressionToken ?? -1)
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
		case 'streamSelection': {
			const nextStream: 'main' | 'sub' = msg.stream === 'sub' ? 'sub' : 'main'
			const previousStream = selectedStream.value
			activeCodecHint = codecHintForName(msg.codec) ?? activeCodecHint
			codecUnsupported.value = false
			if (hasReceivedStreamSelection && previousStream !== nextStream) {
				initGeneration = -1
				renderSuppressionToken++
				renderSuppressed.value = true
				releaseRenderOnIndependent = true
				diag.log('streamSwitch', {
					from: previousStream,
					to: nextStream,
					codec: msg.codec,
					width: msg.width,
					height: msg.height,
					requestedWidth: msg.requestedWidth,
					requestedHeight: msg.requestedHeight,
				})
				handleDiscontinuity(true)
			} else {
				diag.log('streamSelected', {
					stream: nextStream,
					codec: msg.codec,
					width: msg.width,
					height: msg.height,
				})
			}
			selectedStream.value = nextStream
			hasReceivedStreamSelection = true
			break
		}

        case 'initSegment':
		  {
			const generation = Number(msg.generation)
			if (!Number.isFinite(generation) || generation < initGeneration ||
				(generation === initGeneration && !awaitingInit)) {
				expectingBinary = null
				pendingBinaryIntegrity = null
				diag.log('staleInitDiscarded', { generation, initGeneration })
				break
			}
			if (initGeneration >= 0 && generation > initGeneration && !awaitingInit) {
				diag.log('initGenerationAdvanced', { from: initGeneration, to: generation })
				handleDiscontinuity()
			}
			initGeneration = generation
			awaitingInit = false
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
		  }

        case 'partial':
		  if (Number(msg.generation) !== initGeneration) {
			expectingBinary = null
			pendingPartialMetadata = null
			diag.log('stalePartialDiscarded', {
				generation: msg.generation,
				initGeneration,
				segmentIndex: msg.segmentIndex,
				partIndex: msg.partIndex,
			})
			break
		  }
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
            releaseRendering: releaseRenderOnIndependent && Boolean(msg.independent),
            renderSuppressionToken,
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

        case 'decodeCorruption':
		  if (Number(msg.generation) !== initGeneration) {
			diag.log('staleDecodeCorruptionDiscarded', { generation: msg.generation, initGeneration })
			break
		  }
          diag.stats.decodeCorruptionCount++
          if (!renderSuppressed.value) diag.stats.renderSuppressionCount++
          renderSuppressionToken++
          renderSuppressed.value = true
          releaseRenderOnIndependent = false
          reportDiagnosticAnomaly('decodeCorruption')
          diag.log('decodeCorruption', {
            generation: msg.generation,
            segmentIndex: msg.segmentIndex,
            partIndex: msg.partIndex,
            decodeErrorFlags: msg.decodeErrorFlags,
          })
          break

        case 'decodeRecovery':
		  if (Number(msg.generation) !== initGeneration) {
			diag.log('staleDecodeRecoveryDiscarded', { generation: msg.generation, initGeneration })
			break
		  }
          if (renderSuppressed.value) {
            releaseRenderOnIndependent = true
            diag.log('decodeRecoveryPending', {
              generation: msg.generation,
              segmentIndex: msg.segmentIndex,
              partIndex: msg.partIndex,
            })
          }
          break

        case 'segment':
          diag.log('segmentComplete', {
            segmentIndex: msg.segmentIndex,
            duration: msg.duration,
          })
          break

        case 'discontinuity':
		  if (Number(msg.generation) <= initGeneration && !awaitingInit) {
			diag.log('staleDiscontinuityDiscarded', {
				generation: msg.generation,
				initGeneration,
			})
			break
		  }
		  initGeneration = Number(msg.generation)
		  awaitingInit = true
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
    reportDiagnosticAnomaly(`${kind}IntegrityMismatch`)
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

  function reportDiagnosticAnomaly(reason: string) {
    const now = Date.now()
    if (!ws || ws.readyState !== WebSocket.OPEN) return
    if (reason === lastAnomalyReason && now - lastAnomalyReportTime < 5000) return

    lastAnomalyReason = reason
    lastAnomalyReportTime = now
    try {
      ws.send(JSON.stringify({
        type: 'diagnosticAnomaly',
        reason,
        generation: initGeneration,
        expectedBinary: expectingBinary,
        segment: pendingPartialMetadata?.segmentIndex ?? null,
        part: pendingPartialMetadata?.partIndex ?? null,
        readyState: videoRef.value?.readyState ?? null,
        currentTime: videoRef.value?.currentTime ?? null,
        appendQueueLength: appendQueue.length,
      }))
      diag.log('diagnosticAnomalyReported', { reason })
    } catch {
      // The stream is already failing; diagnostics must never impede recovery.
    }
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
		} else if (pendingPartialMetadata?.independent) {
			// The MediaSource may still be opening after a stream switch. Do not
			// consume the release keyframe until one can actually be appended.
			waitingForKeyframe = true
      }
      expectingBinary = null
      pendingPartialMetadata = null
    } else {
      // Binary frame for a skipped partial (no keyframe yet) — discard silently
    }
  }

	function handleDiscontinuity(preserveRenderedFrame = false) {
		if (!preserveRenderedFrame) clearRenderSuppression()
		awaitingInit = true
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
    clearAppendQueue()
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
    pipelineStartedAt = streamStartTime
    lastAppendCompletedAt = 0
    healthyPlaybackSince = 0
    latencyMs.value = 0
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

    // A replacement must not overlap the socket it supersedes. Overlapping
    // retry sockets amplify a congested server/client transport path.
    if (ws) {
      diag.log('connectSuppressed', { reason: 'socketStillPresent', readyState: ws.readyState })
      return
    }

    if (!mediaSource) {
      setupMediaSource(element)
    }

    const url = getWsUrl()
    diag.log('connecting', { url })

	initGeneration = -1
	awaitingInit = true
	intentionalClose = false
    wsOpenedAt = 0
	pipelineStartedAt = Date.now()
	const socket = new WebSocket(url)
	ws = socket
		hasReceivedStreamSelection = false
	socket.binaryType = 'arraybuffer'

	socket.onopen = () => {
	  if (ws !== socket || intentionalClose || socket.readyState !== WebSocket.OPEN) return
      diag.log('wsOpen')
      streamStartTime = Date.now()
      wsOpenedAt = streamStartTime
	  pipelineStartedAt = streamStartTime
		const viewport = adaptiveStream && !useSubStream ? viewportSize() : null
		if (viewport) updateViewport(viewport.width, viewport.height)
    }

	socket.onmessage = (event: MessageEvent) => {
	  if (ws !== socket || intentionalClose || socket.readyState !== WebSocket.OPEN) return
      if (typeof event.data === 'string') {
        handleControlMessage(event.data)
      } else if (event.data instanceof ArrayBuffer) {
        handleBinaryData(event.data)
      }
    }

	socket.onerror = () => {
	  if (ws !== socket) return
      diag.stats.errorCount++
      diag.log('wsError')
    }

	socket.onclose = (event: CloseEvent) => {
	  if (ws !== socket) return
      diag.log('wsClose', { code: event.code, reason: event.reason })
      ws = null
      if (closeFallbackTimer) {
        clearTimeout(closeFallbackTimer)
        closeFallbackTimer = null
      }
      isActive.value = false
      clearRenderSuppression()

      if (!destroyed) {
        // Full teardown — stale MediaSource/SourceBuffer can't be reused reliably
        if (sourceBuffer && mediaSource && mediaSource.readyState === 'open') {
          try {
            mediaSource.removeSourceBuffer(sourceBuffer)
          } catch {}
        }
        sourceBufferGeneration++
        sourceBuffer = null
        clearAppendQueue()
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
        wsOpenedAt = 0
		pipelineStartedAt = streamStartTime
		lastAppendCompletedAt = 0
		healthyPlaybackSince = 0
		latencyMs.value = 0
        expectingBinary = null
		awaitingInit = true
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

        const delayMs = pendingRestartDelayMs ?? restartBackoffMs
        pendingRestartDelayMs = null
        if (!intentionalClose) diag.stats.restartCount++
        const scheduledDelayMs = scheduleReconnect(delayMs)
        diag.log('reconnect', { backoffMs: scheduledDelayMs, afterClose: true })
        if (!intentionalClose) restartBackoffMs = Math.min(restartBackoffMs * 1.5, 30000)
      }
      intentionalClose = false
    }
  }

  function restartStream(reason: string) {
    if (destroyed) return
    diag.stats.restartCount++
    diag.log('restart', { reason, generation: initGeneration })
    reportDiagnosticAnomaly(reason)
    clearRenderSuppression()

    // Cancel any pending reconnect timer to prevent duplicate WebSocket creation
    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
      reconnectAt = 0
    }
    if (closeFallbackTimer) {
      clearTimeout(closeFallbackTimer)
      closeFallbackTimer = null
    }
    pendingRestartDelayMs = null

    // Tear down the media pipeline immediately, but do not overlap WebSockets.
    // Reconnect from onclose; use a bounded fallback only if the browser never
    // reports closure.
    const closingSocket = ws
    intentionalClose = true
    pendingRestartDelayMs = restartBackoffMs

    if (sourceBuffer && mediaSource && mediaSource.readyState === 'open') {
      try {
        mediaSource.removeSourceBuffer(sourceBuffer)
      } catch {}
    }
    sourceBufferGeneration++
    sourceBuffer = null
    clearAppendQueue()
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
    wsOpenedAt = 0
	pipelineStartedAt = streamStartTime
	lastAppendCompletedAt = 0
	healthyPlaybackSince = 0
	latencyMs.value = 0
    expectingBinary = null
	awaitingInit = true
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

    restartBackoffMs = Math.min(restartBackoffMs * 1.5, 30000)
    if (!closingSocket || closingSocket.readyState === WebSocket.CLOSED) {
      ws = null
      intentionalClose = false
      const delayMs = pendingRestartDelayMs
      pendingRestartDelayMs = null
      scheduleReconnect(delayMs ?? 0)
      return
    }
    try {
      closingSocket.close()
    } catch (error) {
      diag.log('wsCloseFailed', { message: error instanceof Error ? error.message : String(error) })
    }
    closeFallbackTimer = setTimeout(() => {
      closeFallbackTimer = null
      if (ws !== closingSocket || destroyed) return
      diag.log('wsCloseTimeout', { readyState: closingSocket.readyState })
      ws = null
      intentionalClose = false
      const delayMs = pendingRestartDelayMs
      pendingRestartDelayMs = null
      scheduleReconnect(delayMs ?? 0)
    }, MSE_SOCKET_CLOSE_TIMEOUT_MS)
  }

  // ── Watchdog ──────────────────────────────────────────────────────
  function startWatchdog() {
    watchdog = setInterval(() => {
      const video = videoRef.value
      if (!video || destroyed) return

      const now = Date.now()
      const previousWatchdogTick = lastWatchdogTick
      const schedulingDelay = now - previousWatchdogTick
      lastWatchdogTick = now
      if (schedulingDelay > MSE_WATCHDOG_SCHEDULING_GRACE_MS) {
        // Browser suspension and long maintenance tasks are not evidence that
        // the media pipeline stalled. Preserve elapsed runnable time for all
        // watchdog deadlines, and retain the scheduling gap for diagnosis.
        const suspendedMs = schedulingDelay - MSE_WATCHDOG_INTERVAL_MS
        if (lastFragTime > 0 && lastFragTime <= previousWatchdogTick) lastFragTime += suspendedMs
        if (streamStartTime <= previousWatchdogTick) streamStartTime += suspendedMs
        if (wsOpenedAt > 0 && wsOpenedAt <= previousWatchdogTick) wsOpenedAt += suspendedMs
        if (pipelineStartedAt <= previousWatchdogTick) pipelineStartedAt += suspendedMs
        if (lastAppendCompletedAt > 0 && lastAppendCompletedAt <= previousWatchdogTick)
          lastAppendCompletedAt += suspendedMs
        if (healthyPlaybackSince > 0 && healthyPlaybackSince <= previousWatchdogTick)
          healthyPlaybackSince += suspendedMs
        if (sourceBufferOperation && sourceBufferOperation.startedAt <= previousWatchdogTick)
          sourceBufferOperation.startedAt += suspendedMs
        for (const item of appendQueue) {
          if (item.queuedAt <= previousWatchdogTick) item.queuedAt += suspendedMs
        }
        if (lowReadyStateSince > 0 && lowReadyStateSince <= previousWatchdogTick)
          lowReadyStateSince += suspendedMs
        if (currentTimeStalledSince > 0 && currentTimeStalledSince <= previousWatchdogTick)
          currentTimeStalledSince += suspendedMs
        if (highLatencySince > 0 && highLatencySince <= previousWatchdogTick)
          highLatencySince += suspendedMs
        diag.log('watchdogSchedulingDelay', { schedulingDelayMs: schedulingDelay, suspendedMs })
      }
      const hasFrags = lastFragTime > 0
      const startupReference = wsOpenedAt || streamStartTime
      const fragAge = hasFrags ? now - lastFragTime : now - startupReference
      const pipelineAge = now - pipelineStartedAt
      const appendOperationAge = sourceBufferOperation
        ? now - sourceBufferOperation.startedAt
        : 0
      const oldestQueuedAge = appendQueue.length > 0 ? now - appendQueue[0]!.queuedAt : 0

      // Spinner: show during initial connect only, not during playback
      showSpinner.value = !hasInitialBuffer && pipelineAge < MSE_INITIAL_TIMEOUT_MS

      // Connection lost: no fragments for extended period
      connectionLost.value = !reconnectTimer && fragAge > MSE_INITIAL_TIMEOUT_MS

      if (!reconnectTimer && ws && ws.readyState === WebSocket.CONNECTING &&
          pipelineAge > MSE_INITIAL_TIMEOUT_MS) {
        diag.log('connectTimeout', { waitedMs: pipelineAge })
        restartStream('connectTimeout')
        return
      }

      // A received fragment is not progress until MSE has completed appending
      // enough media to start playback. This also catches a SourceBuffer stuck
      // updating while the element remains intentionally paused at startup.
      if (!hasInitialBuffer && !reconnectTimer && ws && ws.readyState === WebSocket.OPEN &&
          pipelineAge > MSE_INITIAL_TIMEOUT_MS) {
        diag.log('initialTimeout', {
          waitedMs: pipelineAge,
          hasFrags,
          appendOperationAge,
          appendQueueLength: appendQueue.length,
          appendQueueBytes,
        })
        restartStream('initialTimeout')
        return
      }

      if (appendOperationAge > MSE_APPEND_TIMEOUT_MS || oldestQueuedAge > MSE_APPEND_TIMEOUT_MS) {
        diag.log('appendProgressTimeout', {
          appendOperationAge,
          oldestQueuedAge,
          appendQueueLength: appendQueue.length,
          appendQueueBytes,
        })
        restartStream('appendProgressTimeout')
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
      }

      // Frozen-frame detection: currentTime not advancing while frags are flowing
      // Catches SourceBuffer corruption, silent decode failures, frozen video element
      if (hasFrags && !video.paused && video.readyState >= 1) {
        if (video.currentTime === lastCurrentTime && lastCurrentTime >= 0) {
          healthyPlaybackSince = 0
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
          if (video.readyState >= 3) {
            if (healthyPlaybackSince === 0) healthyPlaybackSince = now
            if (now - healthyPlaybackSince >= MSE_HEALTHY_PLAYBACK_RESET_MS)
              restartBackoffMs = 3000
          } else {
            healthyPlaybackSince = 0
          }
          lastCurrentTime = video.currentTime
          currentTimeStalledSince = 0
        }
      } else {
        healthyPlaybackSince = 0
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
      renderSuppressed: renderSuppressed.value,
		selectedStream: selectedStream.value,
      releaseRenderOnIndependent,
      renderSuppressionToken,
      latencyMs: latencyMs.value,
      lastFragAge: lastFragTime ? Date.now() - lastFragTime : null,
      restartBackoffMs,
      stuckBackoffMs,
      waitingForKeyframe,
      hasInitialBuffer,
      targetHeadroomSeconds: MSE_TARGET_HEADROOM_SECONDS,
      appendQueueLength: appendQueue.length,
      appendQueueBytes,
      appendQueueOldestAgeMs: appendQueue.length > 0 ? Date.now() - appendQueue[0]!.queuedAt : 0,
      sourceBufferGeneration,
      sourceBufferUpdating: sourceBuffer?.updating ?? null,
      sourceBufferOperation: sourceBufferOperation?.kind ?? null,
      sourceBufferOperationAgeMs: sourceBufferOperation
        ? Date.now() - sourceBufferOperation.startedAt
        : 0,
      mediaSourceState: mediaSource?.readyState ?? null,
      wsReadyState: ws?.readyState ?? null,
      wsOpenAgeMs: wsOpenedAt ? Date.now() - wsOpenedAt : null,
      reconnectPendingMs: reconnectTimer ? Math.max(0, reconnectAt - Date.now()) : null,
      pipelineAgeMs: Date.now() - pipelineStartedAt,
      lastAppendAgeMs: lastAppendCompletedAt ? Date.now() - lastAppendCompletedAt : null,
      lowReadyStateMs: lowReadyStateSince ? Date.now() - lowReadyStateSince : 0,
      initGeneration,
		awaitingInit,
    }))
    element.muted = !audioEnabled()
    // Playback begins explicitly once the initial reserve has accumulated.
    element.autoplay = false
    element.playbackRate = 1.0

    connectWebSocket()
    startWatchdog()
    diag.log('start')
  }

  function stop() {
    destroyed = true
    clearRenderSuppression()
    catchUpActive = false
    highLatencySince = 0

    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
      reconnectAt = 0
    }
    if (closeFallbackTimer) {
      clearTimeout(closeFallbackTimer)
      closeFallbackTimer = null
    }
    pendingRestartDelayMs = null

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
    clearAppendQueue()
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

	return {
		showSpinner,
		connectionLost,
		isActive,
		latencyMs,
		codecUnsupported,
		renderSuppressed,
		selectedStream,
		updateViewport,
	}
}
