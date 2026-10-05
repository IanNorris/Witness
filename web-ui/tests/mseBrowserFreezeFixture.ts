// Test-only: actual composable, MSE, decoder, timers and frame callbacks;
// synthetic in-page transport and an explicitly unsafe removal fault.
import { createApp, onMounted, onUnmounted, ref } from 'vue'
import { useMseStream } from '../src/composables/useMseStream'

const video = document.querySelector<HTMLVideoElement>('#video')!
const status = document.querySelector<HTMLElement>('#status')!
const report = document.querySelector<HTMLElement>('#report')!
const startButton = document.querySelector<HTMLButtonElement>('#start')!
const faultButton = document.querySelector<HTMLButtonElement>('#fault')!
const evidence: Record<string, unknown> = {
  scope: 'Actual production composable and browser MSE; synthetic transport, deliberate unsafe removal; not native server/network validation.',
  observations: [],
  visibilityTransitions: [],
}
const NativeMediaSource = window.MediaSource
let activeBuffer: SourceBuffer | null = null
class TrackedMediaSource extends NativeMediaSource {
  addSourceBuffer(mime: string) {
    activeBuffer = super.addSourceBuffer(mime)
    return activeBuffer
  }
}
let chunks: ArrayBuffer[] = [], sockets: FixtureSocket[] = []
let faultArmed = false
let resumedBaseline: {generation: number; frames: number; mediaTime: number} | null = null
let completed = false
class FixtureSocket {
  static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3
  readyState = FixtureSocket.CONNECTING
  binaryType = 'arraybuffer'
  onopen: (() => void) | null = null
  onmessage: ((event: {data: string | ArrayBuffer}) => void) | null = null
  onclose: ((event: {code: number; reason: string}) => void) | null = null
  timer: ReturnType<typeof setInterval> | null = null
  constructor(_url: string) {
    sockets.push(this)
    setTimeout(() => {
      if (this.readyState !== FixtureSocket.CONNECTING) return
      this.readyState = FixtureSocket.OPEN
      this.onopen?.()
      this.control({type: 'initSegment', generation: 0, audioCodec: 'mp4a.40.2'})
      this.binary(chunks[0]!)
      let index = 1
      const send = () => {
        if (index >= chunks.length || this.readyState !== FixtureSocket.OPEN) return
        const part = index - 1
        this.control({type: 'partial', generation: 0, duration: .25,
          independent: part % 40 === 0, keyframeSeekSafe: false,
          segmentIndex: Math.floor(part / 40), partIndex: part % 40})
        this.binary(chunks[index++]!)
      }
      // Initial reserve, followed by a paced 24fps / six-frame-fragment feed.
      for (let count = 0; count < 6; count++) send()
      this.timer = setInterval(send, 250)
    }, 30)
  }
  control(value: object) { this.onmessage?.({data: JSON.stringify(value)}) }
  binary(value: ArrayBuffer) { this.onmessage?.({data: value.slice(0)}) }
  send() {}
  close() {
    if (this.readyState === FixtureSocket.CLOSED) return
    this.readyState = FixtureSocket.CLOSED
    if (this.timer !== null) clearInterval(this.timer)
    this.onclose?.({code: 1000, reason: 'fixture close'})
  }
}
function split(bytes: ArrayBuffer) {
  const view = new DataView(bytes), starts: number[] = []
  for (let offset = 0; offset < bytes.byteLength;) {
    if (offset + 8 > bytes.byteLength) throw Error('Truncated fixture box')
    const length = view.getUint32(offset)
    const kind = String.fromCharCode(...new Uint8Array(bytes, offset + 4, 4))
    if (length < 8 || offset + length > bytes.byteLength) throw Error('Invalid fixture box')
    if (kind === 'moof') starts.push(offset)
    offset += length
  }
  if (starts.length < 100) throw Error('Expected 250ms fragmented 35-second fixture')
  return [bytes.slice(0, starts[0]), ...starts.map((start, index) => bytes.slice(start, starts[index + 1] ?? bytes.byteLength))]
}
const ranges = (value: TimeRanges) => Array.from({length: value.length}, (_, i) => [value.start(i), value.end(i)])
function observe() {
  if (completed) return evidence.latest
  const diagnostic = (window as any)._witnessMseDiag?.['1_mse']?.snapshot()
  const observation = {t: new Date().toISOString(), currentTime: video.currentTime,
    visibilityState: document.visibilityState,
    readyState: video.readyState, paused: video.paused, seeking: video.seeking,
    buffered: ranges(video.buffered), error: video.error ? {code: video.error.code, message: video.error.message} : null,
    drift: diagnostic?.driftSamples?.at(-1), stats: diagnostic?.stats}
  const observations = evidence.observations as unknown[]
  observations.push(observation)
  if (observations.length > 240) observations.shift()
  evidence.latest = observation
  report.textContent = JSON.stringify(evidence, null, 2)
  if (evidence.injected && diagnostic?.lastDriftRestart?.type === 'presentationFrozenRestart') {
    if (!evidence.recovery) evidence.recovery = diagnostic.lastDriftRestart
    status.textContent = 'Presentation-specific restart observed; checking resumed frames'
    const drift = observation.drift
    const recovery = evidence.recovery as {generation: number}
    if (drift && drift.generation !== recovery.generation && drift.presentedFrames > 0 &&
        video.readyState >= 3 && drift.presentationVisible === true &&
        Math.abs(drift.currentTime - drift.presentedMediaTime) < 1) {
      if (!resumedBaseline || resumedBaseline.generation !== drift.generation)
        resumedBaseline = {generation: drift.generation, frames: drift.presentedFrames, mediaTime: drift.presentedMediaTime}
      else if (drift.presentedFrames >= resumedBaseline.frames + 24 &&
               drift.presentedMediaTime >= resumedBaseline.mediaTime + 1) {
        evidence.completed = {t: observation.t, resumedBaseline,
          resumed: drift, passed: diagnostic.stats.restartCount === 1 &&
            diagnostic.stats.stallCount === 1 && diagnostic.stats.errorCount === 0}
        completed = true
        status.textContent = (evidence.completed as {passed: boolean}).passed
          ? 'Recovery complete: presentation restart and resumed frames verified'
          : 'Recovery observed but unexpected additional counters; inspect report'
        setTimeout(() => app?.unmount(), 0)
      }
    }
  }
  if (faultArmed && video.currentTime >= 11 && video.currentTime <= 14 && activeBuffer && !activeBuffer.updating) {
    faultArmed = false
    injectFault()
  }
  report.textContent = JSON.stringify(evidence, null, 2)
  return observation
}
let app: ReturnType<typeof createApp> | null = null
startButton.onclick = async () => {
  startButton.disabled = true
  try {
    const response = await fetch('/fixture.mp4')
    if (!response.ok) throw Error('Fixture unavailable')
    chunks = split(await response.arrayBuffer())
    // Only this private test page replaces its own constructor bindings.
    window.MediaSource = TrackedMediaSource
    window.WebSocket = FixtureSocket as unknown as typeof WebSocket
    app = createApp({setup() {
      useMseStream(1, ref(video), '', false, 'avc1.42c00a')
      let timer: ReturnType<typeof setInterval>
      onMounted(() => { timer = setInterval(observe, 250) })
      onUnmounted(() => clearInterval(timer))
      return () => null
    }})
    app.mount('#mount')
    status.textContent = 'Playing; wait until media time is between 11 and 14 seconds'
    faultButton.disabled = false
  } catch (error) { evidence.error = String(error); status.textContent = 'Fixture failed'; observe() }
}
function injectFault() {
  try {
    const buffer = activeBuffer
    if (!buffer || buffer.updating || video.currentTime < 11 || video.currentTime > 14)
      throw Error('Fault requires idle SourceBuffer and media time 11–14 seconds')
    const removeTo = video.currentTime - 1
    evidence.before = observe()
    evidence.injected = {t: new Date().toISOString(), removeTo}
    buffer.addEventListener('updateend', () => { evidence.afterRemoval = observe() }, {once: true})
    buffer.remove(0, removeTo)
    faultButton.disabled = true
    status.textContent = 'Unsafe eviction injected; watching presented video independently of clock'
  } catch (error) { evidence.faultError = String(error); observe() }
}
faultButton.onclick = () => {
  faultArmed = true
  faultButton.disabled = true
  status.textContent = 'Fault armed; automatic injection at media time 11 seconds'
}
window.addEventListener('pagehide', () => { app?.unmount(); for (const socket of sockets) socket.close() })
document.addEventListener('visibilitychange', () => {
  const transitions = evidence.visibilityTransitions as unknown[]
  transitions.push({t: new Date().toISOString(), visibilityState: document.visibilityState})
  if (transitions.length > 16) transitions.shift()
  observe()
})
