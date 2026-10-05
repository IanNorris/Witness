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
const evictionUrl = moduleUrl(await readFile(new URL('../src/composables/mseEviction.ts', import.meta.url), 'utf8'))
const presentationUrl = moduleUrl(await readFile(new URL('../src/composables/msePresentationProgress.ts', import.meta.url), 'utf8'))
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
  .replace("'./mseEviction'", JSON.stringify(evictionUrl))
  .replace("'./msePresentationProgress'", JSON.stringify(presentationUrl))
let fixtureNumber = 0

async function fixture({ gopSeconds = .25, presentationEnabled = false, viewportWrapped = false, keyframeSeekSafe = false } = {}) {
  // In-memory browser primitives only. No sockets, credentials, media files or
  // browser automation: the production composable handles every control/tick.
  const saved = Object.fromEntries(['window', 'document', 'location', 'WebSocket', 'MediaSource',
    'setInterval', 'clearInterval', 'setTimeout', 'clearTimeout', 'IntersectionObserver'].map(key => [key, globalThis[key]]))
  const realNow = Date.now, createUrl = URL.createObjectURL, revokeUrl = URL.revokeObjectURL
  let now = 100000, timerId = 0, playCalls = 0, socket, buffer, media, stream
  const intervals = new Map(), timeouts = new Map(), removals = []
  const frameCallbacks = new Map()
  let observer
  const fixtureViewport = {getBoundingClientRect:()=>({left:0,top:0,right:100,bottom:100}),contains:node=>node===fixtureViewport}
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
    closest() { return viewportWrapped ? fixtureViewport : this }
    getBoundingClientRect() { return {left:0,top:0,right:100,bottom:100} }
    contains(node) { return node===this }
    requestVideoFrameCallback(fn) { frameCallbacks.set(++timerId,fn); return timerId }
    cancelVideoFrameCallback(id) { frameCallbacks.delete(id) }
  }
  class Buffer extends EventTarget {
    updating = false; holdUpdates = false; quotaFailures = 0; buffered = timeRanges; mode = 'sequence'
    appendBuffer(data) {
      if (this.quotaFailures > 0) {
        this.quotaFailures--
        throw Object.assign(Error('fixture quota pressure'), {name:'QuotaExceededError'})
      }
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
    close() { this.readyState = 3; if (this.errorOnClose) this.onerror?.(); this.onclose?.({code: 1000, reason: 'fixture'}) }
    message(data) { this.onmessage({data: typeof data === 'object' && !(data instanceof ArrayBuffer) ? JSON.stringify(data) : data}) }
  }
  const video = new Video()
  try {
    globalThis.window = {innerWidth:200,innerHeight:200}; globalThis.document = new EventTarget()
    document.elementFromPoint=()=>observer?.rendered===false?null:observer?.target??null
    document.visibilityState = 'visible'
    globalThis.location = {protocol: 'http:', host: 'fixture.invalid'}
    globalThis.WebSocket = Socket; globalThis.MediaSource = Media
    globalThis.IntersectionObserver = presentationEnabled ? class {
      constructor(fn) { this.fn=fn; observer=this }
      observe(target) { this.target=target; this.setVisible(true) }
      setVisible(visible, rendered=true) { this.rendered=rendered;this.fn([{target:this.target,isIntersecting:visible,intersectionRatio:visible?1:0}]) }
      disconnect() {}
    } : undefined
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
    let feedIndex = 0
    const feed = () => {
      const partIndex = feedIndex++ % (gopSeconds * 4)
      socket.message({type: 'partial', generation: 0, duration: .25, independent: partIndex === 0,
        keyframeSeekSafe, segmentIndex: 1, partIndex})
      const bytes = new ArrayBuffer(8); new DataView(bytes).setFloat64(0, .25)
      socket.message(bytes)
    }
    const tick = (deliver = true, elapsedMs = 250) => {
      now += elapsedMs
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
    return { video, tick, diag, restore, removals, buffer, stream,
      present(time) { for (const [id,fn] of [...frameCallbacks]) { frameCallbacks.delete(id); fn(0,{mediaTime:time}) } },
      inView(visible, rendered=true) { observer.setVisible(visible,rendered) },
      observedTarget:()=>observer.target, pendingFrames:()=>frameCallbacks.size,
      hide() { document.visibilityState = 'hidden'; video.paused = true },
      show() { document.visibilityState = 'visible' },
      playCalls: () => playCalls, socket: () => socket, timers: timeouts }
  } catch (error) {
    for (const [key, value] of Object.entries(saved)) globalThis[key] = value
    Date.now = realNow; URL.createObjectURL = createUrl; URL.revokeObjectURL = revokeUrl
    throw error
  }
}

test('real composable records bounded playhead buffer-position evidence', async () => {
  const f = await fixture()
  try {
    f.video.currentTime = .5
    for (let i = 0; i < 4; i++) f.tick()
    const sample = f.diag().driftSamples.at(-1)
    assert.equal(sample.playheadBuffered, true)
    assert.equal(sample.bufferedRangeCount, 1)
    assert.equal(sample.playheadGapMs, 0)
    // Drift samples are one-second snapshots, not necessarily the final tick.
    assert.equal(sample.headroomInCurrentRangeMs, (sample.bufferedEnd - sample.currentTime) * 1000)
    assert.ok(sample.headroomInCurrentRangeMs > 0)
    assert.equal(sample.nextBufferedStartSeconds, undefined)
  } finally { f.restore() }
})

test('real watchdog preserves hidden ingest then immediately recovers an evicted unsafe playhead', async () => {
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
    assert.equal(f.diag().stats.restartCount, 1, 'evicted unsafe playback must recover on the first visible tick')
    assert.equal(f.diag().lastDriftRestart.type, 'hiddenBufferEvictedRestart')
    assert.equal(f.diag().lastDriftRestart.drift.playheadBuffered, false)
    assert.ok(f.diag().lastDriftRestart.drift.playheadGapMs > 0)
    for (let i = 0; i < 17; i++) { f.video.currentTime += .25; f.tick() }
    assert.equal(f.diag().stats.restartCount, 1, 'the same eviction must not trigger repeated restarts')
  } finally { f.restore() }
})

test('real watchdog resumes a briefly hidden playable buffer without reconnecting', async () => {
  const f = await fixture()
  try {
    f.video.currentTime = f.video.buffered.end(0) - 1
    f.hide(); const plays = f.playCalls()
    for (let i = 0; i < 4; i++) f.tick()
    f.show(); f.tick(); await Promise.resolve()
    assert.ok(f.playCalls() > plays)
    assert.equal(f.diag().stats.restartCount, 0)
    assert.ok(f.diag().importantEvents.some(e => e.type === 'hiddenPlaybackRecoveryResumed'))
    assert.ok(!f.diag().importantEvents.some(e => e.type === 'hiddenBufferEvictedRestart'))
  } finally { f.restore() }
})

test('real watchdog seeks to a verified retained RAP instead of reconnecting after hidden eviction', async () => {
  // A long GOP leaves only one verified RAP in the retained range. Ordinary
  // two-keyframe catch-up cannot rescue this evicted playhead on resume.
  const f = await fixture({gopSeconds:60, keyframeSeekSafe:true})
  try {
    f.hide()
    for (let i = 0; i < 960; i++) f.tick()
    assert.ok(f.video.buffered.start(0) > f.video.currentTime)
    f.show(); f.tick(); await Promise.resolve()
    assert.equal(f.diag().stats.restartCount, 0)
    assert.ok(f.video.currentTime >= f.video.buffered.start(0),
      'the first visible tick must rescue the evicted playhead with the single retained RAP')
    const seek = f.diag().importantEvents.find(e => e.type === 'hiddenBufferEvictedSeek')
    assert.ok(seek)
    assert.equal(f.video.currentTime, seek.to)
    assert.ok(seek.to >= f.video.buffered.start(0) && seek.to < f.video.buffered.end(0))
    assert.equal(f.diag().liveState.wsReadyState, 1)
    assert.ok(f.video.playbackRate >= 1 && f.video.playbackRate <= 1.08)
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

test('last counted error survives routine-event eviction and retains transport context', async () => {
  const f = await fixture()
  try {
    f.socket().onerror()
    assert.equal(f.diag().stats.errorCount, 1)
    const error = f.diag().lastError
    assert.equal(error.type, 'wsError')
    assert.equal(error.wsReadyState, 1)
    assert.equal(error.intentionalClose, false)
    assert.ok(error.drift)
    for (let i = 0; i < 600; i++) window._witnessMseDiag['1_mse'].log('media:playing')
    assert.equal(f.diag().importantEvents.some(e => e.type === 'wsError'), false)
    assert.deepEqual(f.diag().lastError, error)
    f.buffer.dispatchEvent(new Event('error'))
    assert.equal(f.diag().lastError.type, 'sourceBufferError')
    assert.equal(f.diag().lastError.consecutive, 1)
  } finally { f.restore() }
})

test('transport errors during deliberate teardown remain distinguishable', async () => {
  const f = await fixture()
  try {
    f.socket().errorOnClose = true
    f.hide()
    for (let i = 0; i < 45; i++) f.tick(false)
    assert.equal(f.diag().stats.restartCount, 1)
    assert.equal(f.diag().stats.errorCount, 1)
    assert.equal(f.diag().lastError.type, 'wsError')
    assert.equal(f.diag().lastError.intentionalClose, true)
    assert.equal(f.diag().lastError.wsReadyState, 3)
  } finally { f.restore() }
})

test('a single scheduling suspension defers recovery but does not disable later frozen-playback recovery', async () => {
  const f = await fixture()
  try {
    f.tick(true, 60000)
    assert.equal(f.diag().stats.restartCount, 0, 'elapsed suspended time is not runnable stall evidence')
    assert.ok(f.diag().importantEvents.some(e => e.type === 'watchdogSchedulingDelay'))
    for (let i = 0; i < 17; i++) f.tick()
    assert.equal(f.diag().stats.restartCount, 1)
    assert.ok(f.diag().importantEvents.some(e => e.type === 'frozenRestart'))
  } finally { f.restore() }
})

test('repeated delayed ticks still accumulate runnable frozen-playback time', async () => {
  const f = await fixture()
  try {
    for (let i = 0; i < 17; i++) f.tick(true, 25000)
    assert.equal(f.diag().stats.restartCount, 1,
      'scheduling grace must not reset the entire stall deadline on every tick')
    assert.ok(f.diag().importantEvents.some(e => e.type === 'frozenRestart'))
  } finally { f.restore() }
})

test('short under-buffered pauses recover latency through bounded catch-up without a reconnect', async t => {
  // Feed/clock simulation through the production composable, not a browser or
  // decoder performance claim. This isolates what the existing control policy
  // does when media arrives but presentation pauses briefly.
  for (const pauseSeconds of [.5, 1, 2]) {
    const f = await fixture()
    try {
      f.video.currentTime = f.video.buffered.end(0) - 1.5
      f.video.readyState = 2
      for (let i = 0; i < pauseSeconds * 4; i++) {
        f.tick()
        assert.equal(f.video.playbackRate, 1, 'under-buffered playback must not accelerate')
      }
      const delayedLag = f.video.buffered.end(0) - f.video.currentTime
      assert.ok(Math.abs(delayedLag - (1.5 + pauseSeconds)) < .001)
      f.video.readyState = 4
      let elapsed = 0
      let accelerated = false
      while (elapsed < 60) {
        f.video.currentTime += .25 * f.video.playbackRate
        f.tick()
        elapsed += .25
        accelerated ||= f.video.playbackRate > 1
        assert.ok([1, 1.03, 1.08].includes(f.video.playbackRate))
        if (accelerated && f.video.playbackRate === 1) break
      }
      assert.equal(f.diag().stats.restartCount, 0)
      assert.equal(f.diag().stats.stallCount, 0)
      assert.equal(f.diag().stats.errorCount, 0)
      assert.ok(accelerated)
      assert.ok(f.video.buffered.end(0) - f.video.currentTime < 1.25)
      assert.ok(elapsed < 60, 'catch-up must eventually exit its hysteresis band')
      t.diagnostic(`simulated presentation pause ${pauseSeconds}s: catch-up completed after ${elapsed}s; no restart`)
      if (pauseSeconds === 2) {
        assert.ok(elapsed > 30, 'a two-second interruption takes tens of seconds to repay at these rates')
        assert.ok(elapsed < 45)
      }
    } finally { f.restore() }
  }
})

test('production quota retry protects independent decode intervals even when keyframe seeking is unsafe', async () => {
  const f = await fixture({gopSeconds:5})
  try {
    f.hide()
    for (let i=0;i<40;i++) f.tick()
    f.show(); f.video.currentTime=7; f.video.paused=false
    f.buffer.quotaFailures=1
    f.tick()
    // Independent append ends are .25, 5.25 and 10.25. A one-second
    // aggressive target would remove to 6 and delete the active video GOP.
    assert.deepEqual(f.removals,[.25])
    for (const timer of [...f.timers.values()]) timer.fn()
    assert.equal(f.diag().liveState.appendQueueLength,0)
    assert.equal(f.diag().stats.restartCount,0)
    assert.equal(f.diag().stats.errorCount,0)
    assert.ok(f.diag().importantEvents.some(event=>event.type==='quotaExceeded'))
    assert.ok(!f.diag().importantEvents.some(event=>event.type==='keyframeCatchUpSeek'))
  } finally { f.restore() }
})

test('presentation watchdog recovers frozen video even when the media clock and fragments advance', async () => {
  const f=await fixture({presentationEnabled:true,viewportWrapped:true})
  try {
    assert.notEqual(f.observedTarget(),f.video,'observe the shared control/video viewport')
    f.video.currentTime=.5; f.present(.5); f.present(.5)
    for(let i=0;i<24;i++) { f.video.currentTime+=.25; f.tick() }
    assert.equal(f.diag().stats.restartCount,1)
    assert.equal(f.diag().stats.stallCount,1)
    assert.equal(f.diag().lastDriftRestart.type,'presentationFrozenRestart')
    assert.equal(f.diag().lastDriftRestart.drift.presentedMediaTime,.5)
    assert.ok(f.diag().lastDriftRestart.drift.presentationStallMs>3000)
  } finally { f.restore() }
  assert.equal(f.pendingFrames(),0,'unmount cancels the presentation callback')
})

test('presentation recovery excludes hidden, offscreen, paused, seeking and suppressed players', async () => {
  for(const mode of ['hidden','offscreen','occluded','paused','seeking','suppressed']) {
    const f=await fixture({presentationEnabled:true})
    try {
      f.video.currentTime=.5; f.present(.5); f.present(.5)
      if(mode==='hidden')f.hide()
      if(mode==='offscreen')f.inView(false)
      if(mode==='occluded')f.inView(true,false)
      if(mode==='seeking')f.video.seeking=true
      if(mode==='suppressed')f.stream.renderSuppressed.value=true
      for(let i=0;i<24;i++) {
        if(mode==='paused')f.video.paused=true // Existing playback guard can try to resume.
        f.video.currentTime+=.25; f.tick()
      }
      assert.equal(f.diag().stats.restartCount,0,mode)
    } finally { f.restore() }
  }
})

test('unsupported presentation observation keeps existing recovery without inventing a stall', async () => {
  const f=await fixture()
  try {
    f.video.currentTime=.5
    for(let i=0;i<24;i++) { f.video.currentTime+=.25; f.tick() }
    assert.equal(f.diag().stats.restartCount,0)
  } finally { f.restore() }
})

test('presentation watchdog recovers when no first video frame is ever presented', async () => {
  const f=await fixture({presentationEnabled:true,viewportWrapped:true})
  try {
    f.video.currentTime=.5
    for(let i=0;i<24;i++) { f.video.currentTime+=.25; f.tick() }
    assert.equal(f.diag().stats.restartCount,1)
    assert.equal(f.diag().lastDriftRestart.type,'presentationFrozenRestart')
    assert.equal(f.diag().lastDriftRestart.drift.presentedFrames,0)
    assert.equal(f.diag().lastDriftRestart.drift.presentedMediaTime,undefined)
    assert.ok(f.diag().lastDriftRestart.drift.presentationStallMs>3000)
  } finally { f.restore() }
})

test('missing first frame does not restart hidden, covered, offscreen or suppressed playback', async () => {
  for(const mode of ['hidden','occluded','offscreen','suppressed']) {
    const f=await fixture({presentationEnabled:true,viewportWrapped:true})
    try {
      if(mode==='hidden')f.hide()
      if(mode==='occluded')f.inView(true,false)
      if(mode==='offscreen')f.inView(false)
      if(mode==='suppressed')f.stream.renderSuppressed.value=true
      for(let i=0;i<24;i++) { f.video.currentTime+=.25; f.tick() }
      assert.equal(f.diag().stats.restartCount,0,mode)
    } finally { f.restore() }
  }
})

test('advancing presented timestamps avoid recovery even when image content would be static', async () => {
  const f=await fixture({presentationEnabled:true})
  try {
    f.video.currentTime=.5; f.present(.5); f.present(.5)
    for(let i=0;i<40;i++) { f.video.currentTime+=.25; f.present(f.video.currentTime); f.tick() }
    assert.equal(f.diag().stats.restartCount,0)
    assert.ok(f.diag().driftSamples.at(-1).presentedFrames>30)
  } finally { f.restore() }
})

test('discontinuity clears presentation evidence and ignores an already armed old-generation callback', async () => {
  const f=await fixture({presentationEnabled:true})
  try {
    f.video.currentTime=.5; f.present(.5); f.present(.5)
    f.socket().message({type:'initSegment',generation:1})
    f.socket().message(new ArrayBuffer(1))
    f.present(99)
    for(let i=0;i<4;i++)f.tick(false)
    assert.equal(f.diag().driftSamples.at(-1).presentedFrames,0)
    assert.equal(f.diag().driftSamples.at(-1).presentedMediaTime,undefined)
    f.present(.1)
    for(let i=0;i<4;i++)f.tick(false)
    assert.equal(f.diag().driftSamples.at(-1).presentedMediaTime,.1)
  } finally { f.restore() }
})
