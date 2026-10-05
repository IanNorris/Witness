import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
import { execFileSync } from 'node:child_process'

const moduleUrl = source => 'data:text/javascript;base64,' + Buffer.from(ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText).toString('base64')
const vueUrl = moduleUrl('export const ref = value => ({value}); export const onUnmounted = fn => { window.fixtureUnmount = fn; };')
const guardUrl = moduleUrl((await readFile(new URL('../src/composables/playbackGuard.ts', import.meta.url), 'utf8')).replace("'vue'", JSON.stringify(vueUrl)))
const visibilityUrl = moduleUrl(await readFile(new URL('../src/composables/msePlaybackVisibility.ts', import.meta.url), 'utf8'))
const baseline = process.env.WITNESS_MSE_WATCHDOG_BASELINE_REV
if (baseline && !/^[a-f0-9]{7,40}$/.test(baseline)) throw Error('Expected a local commit hash')
const originalSource = baseline
  ? execFileSync('git', ['show', `${baseline}:web-ui/src/composables/useMseStream.ts`], {
    encoding: 'utf8', cwd: new URL('../..', import.meta.url),
  })
  : await readFile(new URL('../src/composables/useMseStream.ts', import.meta.url), 'utf8')
const source = originalSource
  .replace("'vue'", JSON.stringify(vueUrl))
  .replace("'./playbackGuard'", JSON.stringify(guardUrl))
  .replace("'./msePlaybackVisibility'", JSON.stringify(visibilityUrl))
let fixtureNumber = 0

async function fixture() {
  // In-memory browser primitives only. No sockets, credentials, media files or
  // browser automation: the production composable handles every control/tick.
  const saved = Object.fromEntries(['window', 'document', 'location', 'WebSocket', 'MediaSource',
    'setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'].map(key => [key, globalThis[key]]))
  const realNow = Date.now, createUrl = URL.createObjectURL, revokeUrl = URL.revokeObjectURL
  let now = 100000, timerId = 0, playCalls = 0, socket, buffer, media, stream
  const intervals = new Map(), timeouts = new Map(), removals = []
  const ranges = { start: 0, end: 0 }
  const timeRanges = { get length() { return ranges.end > ranges.start ? 1 : 0 },
    start: () => ranges.start, end: () => ranges.end }
  class Video extends EventTarget {
    currentTime = 0; readyState = 4; paused = true; playbackRate = 1; src = ''; muted = true
    buffered = timeRanges; ended = false; error = null
    play() { playCalls++; this.paused = false; return Promise.resolve() }
    pause() { this.paused = true }
    removeAttribute() { this.src = '' }
    load() { this.paused = true; this.currentTime = 0 }
  }
  class Buffer extends EventTarget {
    updating = false; holdUpdates = false; buffered = timeRanges; mode = 'sequence'
    appendBuffer(data) {
      this.updating = true
      if (this.holdUpdates) return
      if (data.byteLength === 8) ranges.end += new DataView(data).getFloat64(0)
      this.updating = false
      this.dispatchEvent(new Event('updateend'))
    }
    remove(_start, end) {
      removals.push(end); ranges.start = Math.min(ranges.end, end)
      this.dispatchEvent(new Event('updateend'))
    }
  }
  class Media extends EventTarget {
    static isTypeSupported() { return true }
    readyState = 'open'
    constructor() { super(); media = this }
    addSourceBuffer() { buffer = new Buffer(); return buffer }
    removeSourceBuffer() { ranges.start = 0; ranges.end = 0 }
  }
  class Socket {
    static OPEN = 1; static CONNECTING = 0
    readyState = 1
    constructor() { socket = this }
    send() {}
    close() { this.readyState = 3; this.onclose?.({code: 1000, reason: 'fixture'}) }
    message(data) { this.onmessage({data: typeof data === 'object' && !(data instanceof ArrayBuffer) ? JSON.stringify(data) : data}) }
  }
  const video = new Video()
  try {
    globalThis.window = {}; globalThis.document = new EventTarget()
    document.visibilityState = 'visible'
    globalThis.location = {protocol: 'http:', host: 'fixture.invalid'}
    globalThis.WebSocket = Socket; globalThis.MediaSource = Media
    globalThis.setInterval = (fn, ms) => { intervals.set(++timerId, {fn, ms}); return timerId }
    globalThis.clearInterval = id => intervals.delete(id)
    globalThis.setTimeout = (fn, ms) => { timeouts.set(++timerId, {fn, ms}); return timerId }
    globalThis.clearTimeout = id => timeouts.delete(id)
    Date.now = () => now
    URL.createObjectURL = () => 'blob:fixture'; URL.revokeObjectURL = () => {}
    const mse = await import(moduleUrl(source + `\n// Fixture ${++fixtureNumber}`))
    stream = mse.useMseStream(1, {value: video})
    intervals.values().next().value.fn()
    media.dispatchEvent(new Event('sourceopen')); socket.onopen()
    socket.message({type: 'initSegment', generation: 0})
    socket.message(new ArrayBuffer(1))
    const feed = () => {
      socket.message({type: 'partial', generation: 0, duration: .25, independent: true,
        keyframeSeekSafe: false, segmentIndex: 1, partIndex: 1})
      const bytes = new ArrayBuffer(8); new DataView(bytes).setFloat64(0, .25)
      socket.message(bytes)
    }
    const tick = (deliver = true) => {
      now += 250
      if (deliver && socket.readyState === 1) feed()
      for (const timer of [...intervals.values()]) timer.fn()
    }
    for (let i = 0; i < 8; i++) tick()
    await Promise.resolve()
    const diag = () => window._witnessMseDiag['1_mse'].snapshot()
    const restore = () => {
      try { window.fixtureUnmount() } finally {
        for (const [key, value] of Object.entries(saved)) globalThis[key] = value
        Date.now = realNow; URL.createObjectURL = createUrl; URL.revokeObjectURL = revokeUrl
      }
    }
    return { video, tick, diag, restore, removals, buffer,
      hide() { document.visibilityState = 'hidden'; video.paused = true },
      show() { document.visibilityState = 'visible' },
      playCalls: () => playCalls, socket: () => socket, timers: timeouts }
  } catch (error) {
    for (const [key, value] of Object.entries(saved)) globalThis[key] = value
    Date.now = realNow; URL.createObjectURL = createUrl; URL.revokeObjectURL = revokeUrl
    throw error
  }
}

test('real watchdog preserves ingest during a 90-second hidden frozen-playhead interval', async () => {
  const f = await fixture()
  try {
    f.hide(); const plays = f.playCalls(), fragments = f.diag().stats.totalFragments
    for (let i = 0; i < 360; i++) {
      if (i === 180) f.video.paused = false // Also cover silently frozen, apparently playing media.
      f.tick()
    }
    assert.equal(f.diag().stats.restartCount, 0)
    assert.equal(f.playCalls(), plays)
    assert.equal(f.diag().stats.totalFragments, fragments + 360)
    assert.equal(f.diag().liveState.wsReadyState, 1)
    assert.ok(f.removals.length > 0)
    assert.ok(f.video.buffered.end(0) - f.video.buffered.start(0) <= 30.01)
    f.video.paused = true
    f.show(); f.tick(); await Promise.resolve()
    assert.ok(f.playCalls() > plays)
    assert.equal(f.diag().stats.restartCount, 0, 'returning visible must not inherit a hidden drift timer')
    for (let i = 0; i < 17; i++) { f.video.currentTime += .25; f.tick() }
    assert.equal(f.diag().stats.restartCount, 1, 'visible stale playback must still recover')
  } finally { f.restore() }
})

test('a hidden tab still recovers from a stalled SourceBuffer append', async () => {
  const f = await fixture()
  try {
    f.hide(); f.buffer.holdUpdates = true
    for (let i = 0; i < 45; i++) f.tick()
    assert.equal(f.diag().stats.restartCount, 1)
    assert.ok(f.diag().importantEvents.some(e => e.type === 'appendProgressTimeout'))
  } finally { f.restore() }
})

test('a hidden tab recovers an open socket that stops delivering media', async () => {
  const f = await fixture()
  try {
    f.hide()
    for (let i = 0; i < 45; i++) f.tick(false)
    assert.equal(f.diag().stats.restartCount, 1)
    assert.ok(f.diag().importantEvents.some(e => e.type === 'fragmentTimeout'))
  } finally { f.restore() }
})
