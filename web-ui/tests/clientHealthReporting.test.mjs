import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const moduleUrl = source => 'data:text/javascript;base64,' + Buffer.from(ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText).toString('base64')
const driftUrl = moduleUrl(await readFile('src/composables/driftDiagnostics.ts', 'utf8'))
const source = (await readFile('src/composables/useClientHealth.ts', 'utf8'))
  .replace("'./driftDiagnostics'", JSON.stringify(driftUrl))
  .replace("import { api } from './useApi'", 'const api = (...args) => globalThis.sendHealth(...args)')
globalThis.localStorage = { getItem: () => 'persistent-browser', setItem: () => assert.fail('replaced browser identity') }
const health = await import(moduleUrl(source))

test('compact report bounds event/drift history without losing players or core health', () => {
  const t = '2026-10-04T00:00:00Z'
  const original = { sessionId: 'tab', sampledAtUtc: t, buildHash: 'build', userAgent: 'test', path: '/',
    visibilityState: 'visible', playersTruncated: false, players: [{ id: '8_mse', latencyMs: 1234,
      recentEvents: Array(40).fill({ t, type: 'error', message: 'secret', driftSamples: [{}] }),
      driftSamples: Array(60).fill({ t, lagMs: 1234 }), lastDriftRestart: { t, type: 'restart' } }] }
  const report = health.compactHealthReport(original)
  assert.equal(report.players.length, 1)
  assert.equal(report.players[0].latencyMs, 1234)
  assert.equal(report.players[0].recentEvents.length, 8)
  assert.equal(report.players[0].recentEvents[0].message, undefined)
  assert.equal(report.players[0].driftSamples.length, 4)
  assert.deepEqual(report.players[0].lastDriftRestart, { t, type: 'restart', reason: undefined, lagMs: undefined, drift: undefined })
  assert.equal(original.players[0].recentEvents.length, 40)
  const many = { ...original, players: Array.from({ length: 64 }, (_, index) => ({
    ...original.players[0], id: `${index}_mse`, driftSamples: Array(60).fill({ t, buffered: Array(8).fill([1, 2]),
      sourceBuffered: Array(8).fill([1, 2]), lagMs: 1234, appendQueueBytes: 12345, schedulingDelayMs: 9999 }),
  })) }
  const bounded = health.compactHealthReport(many)
  assert.equal(bounded.players.length, 64)
  assert.ok(new TextEncoder().encode(JSON.stringify(bounded)).length < 64 * 1024)
})

test('a rich 16-player report retains suspension and restart evidence under the POST budget', () => {
  const t = '2026-10-05T01:53:51Z'
  const drift = { t, currentTime: 11.4, bufferedEnd: 152.5, lagMs: 141100,
    visibility: 'hidden', paused: true, readyState: 4, playbackRate: 1.08,
    schedulingDelayMs: 59986, fragmentAgeMs: 158, appendAgeMs: 155,
    bufferedRangeCount: 2, playheadBuffered: false, nextBufferedStartSeconds: 13,
    playheadGapMs: 1600, headroomInCurrentRangeMs: 0, hasAudio: true, sourceBufferMode: 'sequence',
    presentedMediaTime: 8.1, presentedFrames: 73, presentationStallMs: 3250, presentationVisible: true,
    buffered: Array(8).fill([11.4, 152.5]), sourceBuffered: Array(8).fill([11.4, 152.5]),
    // Large but finite JSON numbers exercise compaction rather than only a tiny fixture.
    appendQueueBytes: Number.MAX_VALUE, appendQueueLength: Number.MAX_VALUE,
    timestampOffsetSeconds: Number.MAX_VALUE, maxAppendDurationMs: Number.MAX_VALUE,
    maxFragmentGapMs: Number.MAX_VALUE, sampleIntervalMs: Number.MAX_VALUE,
    playbackAdvanceMs: Number.MAX_VALUE, bufferAdvanceMs: Number.MAX_VALUE }
  const session = { sessionId: 'tab', sampledAtUtc: t, buildHash: 'build', userAgent: 'test', path: '/',
    visibilityState: 'hidden', playersTruncated: false, players: Array.from({length: 16}, (_, index) => ({
      id: `${index + 1}_mse`, latencyMs: 141100, restartCount: 8, stallCount: 5,
      recentEvents: Array(8).fill({ t, type: 'restart', reason: 'drift', message: 'omit', mimeType: 'a'.repeat(120) }),
      driftSamples: Array(60).fill(drift),
      lastDriftRestart: { t, type: 'driftRestart', lagMs: 141100, message: 'omit', drift, driftSamples: Array(20).fill(drift) },
      lastError: { t, type: 'wsError', code: 3, generation: 4, wsReadyState: 3,
        intentionalClose: true, message: 'secret', drift, driftSamples: Array(20).fill(drift) },
    })) }
  const report = health.compactHealthReport(session)
  assert.equal(report.players.length, 16)
  assert.ok(new TextEncoder().encode(JSON.stringify(report)).length < 60 * 1024)
  for (const player of report.players) {
    assert.equal(player.driftSamples.length, 1)
    assert.equal(player.driftSamples[0].schedulingDelayMs, 59986)
    assert.equal(player.driftSamples[0].paused, true)
    assert.equal(player.driftSamples[0].fragmentAgeMs, 158)
    assert.equal(player.driftSamples[0].playheadBuffered, false)
    assert.equal(player.driftSamples[0].playheadGapMs, 1600)
    assert.equal(player.driftSamples[0].nextBufferedStartSeconds, 13)
    assert.equal(player.driftSamples[0].headroomInCurrentRangeMs, 0)
    assert.equal(player.driftSamples[0].hasAudio, true)
    assert.equal(player.driftSamples[0].sourceBufferMode, 'sequence')
    assert.equal(player.driftSamples[0].presentedMediaTime,8.1)
    assert.equal(player.driftSamples[0].presentedFrames,73)
    assert.equal(player.driftSamples[0].presentationStallMs,3250)
    assert.equal(player.driftSamples[0].presentationVisible,true)
    assert.equal(player.driftSamples[0].buffered, undefined)
    assert.equal(player.lastDriftRestart.drift.lagMs, 141100)
    assert.equal(player.lastDriftRestart.message, undefined)
    assert.equal(player.lastDriftRestart.driftSamples, undefined)
    assert.equal(player.lastError.type, 'wsError')
    assert.equal(player.lastError.intentionalClose, true)
    assert.equal(player.lastError.generation, 4)
    assert.equal(player.lastError.drift.paused, true)
    assert.equal(player.lastError.message, undefined)
    assert.equal(player.lastError.driftSamples, undefined)
  }
  assert.equal(session.players[0].driftSamples.length, 60)
  assert.equal(session.players[0].lastDriftRestart.message, 'omit')
})

test('optional real-player export retains compact causal evidence', {
  skip: !process.env.WITNESS_CLIENT_HEALTH_FIXTURE,
}, async () => {
  // Offline JSON only: never opens a browser or retrieves live credentials.
  const fixture = JSON.parse(await readFile(process.env.WITNESS_CLIENT_HEALTH_FIXTURE, 'utf8')).client
  const prior = { window: globalThis.window, document: globalThis.document,
    location: globalThis.location, build: globalThis.__BUILD_HASH__ }
  try {
    globalThis.window = { _witnessMseDiag: Object.fromEntries(Object.entries(fixture.cameras)
      .map(([id, data]) => [id, { snapshot: () => data }])) }
    globalThis.document = { visibilityState: 'visible' }
    globalThis.location = { pathname: '/' }
    globalThis.__BUILD_HASH__ = fixture.buildHash
    const original = health.collectLocalClientHealth()
    const report = health.compactHealthReport(original)
    assert.equal(report.players.length, Object.keys(fixture.cameras).length)
    assert.ok(new TextEncoder().encode(JSON.stringify(report)).length < 60 * 1024)
    for (const player of report.players) {
      assert.ok(player.driftSamples?.length, `${player.id} lost drift evidence`)
      if (fixture.cameras[player.id].lastDriftRestart)
        assert.ok(player.lastDriftRestart, `${player.id} lost last drift recovery`)
    }
    console.info('Offline fixture compaction:', JSON.stringify({ players: report.players.length,
      originalBytes: new TextEncoder().encode(JSON.stringify(original)).length,
      compactBytes: new TextEncoder().encode(JSON.stringify(report)).length }))
  } finally {
    globalThis.window = prior.window; globalThis.document = prior.document
    globalThis.location = prior.location; globalThis.__BUILD_HASH__ = prior.build
  }
})

test('reporter keeps identity, sends hidden pages, retries and stops on logout', async () => {
  const originalTimer = globalThis.setTimeout, originalClear = globalThis.clearTimeout
  const originalPerformance = globalThis.performance
  const timers = new Map(), listeners = new Map()
  let timerId = 0, now = 10000, allowed = true, calls = []
  globalThis.setTimeout = (fn, delay) => { timers.set(++timerId, { fn, delay }); return timerId }
  globalThis.clearTimeout = id => timers.delete(id)
  globalThis.performance = { now: () => now }
  globalThis.__BUILD_HASH__ = 'build'
  globalThis.location = { pathname: '/' }
  globalThis.document = { visibilityState: 'hidden', addEventListener: (name, fn) => listeners.set(name, fn), removeEventListener: name => listeners.delete(name) }
  globalThis.window = { addEventListener: (name, fn) => listeners.set(name, fn), removeEventListener: name => listeners.delete(name) }
  globalThis.sendHealth = async (...args) => { calls.push(args); throw new Error('network down') }
  try {
    const stop = health.startClientHealthReporting(() => allowed)
    const first = [...timers.entries()].find(([, timer]) => timer.delay < 1000)
    timers.delete(first[0]); await first[1].fn()
    assert.equal(calls.length, 1)
    assert.equal(calls[0][0], '/debug/client-health')
    assert.equal(calls[0][1].redirectOnFail, false)
    assert.equal(calls[0][1].body.snapshot.browserId, 'persistent-browser')
    assert.equal(calls[0][1].body.snapshot.visibilityState, 'hidden')
    assert.equal([...timers.values()].some(timer => timer.delay === 15000), true)
    now += 15000
    const retry = [...timers.entries()].find(([, timer]) => timer.delay === 15000)
    timers.delete(retry[0]); await retry[1].fn()
    assert.equal(calls.length, 2)
    allowed = false; stop()
    assert.equal(timers.size, 0)
    assert.equal(listeners.size, 0)
    await retry[1].fn()
    assert.equal(calls.length, 2)
    allowed = true
    globalThis.sendHealth = (...args) => {
      calls.push(args)
      return new Promise((_resolve, reject) => args[1].signal.addEventListener('abort', () => reject(new Error('aborted'))))
    }
    const stopPending = health.startClientHealthReporting(() => allowed)
    const pending = [...timers.entries()].find(([, timer]) => timer.delay < 1000)
    timers.delete(pending[0]); const running = pending[1].fn()
    const timeout = [...timers.entries()].find(([, timer]) => timer.delay === 5000)
    timeout[1].fn(); await running
    assert.equal(calls[2][1].signal.aborted, true)
    assert.equal([...timers.values()].some(timer => timer.delay === 15000), true)
    stopPending()
    assert.equal(timers.size, 0)
  } finally {
    globalThis.setTimeout = originalTimer; globalThis.clearTimeout = originalClear; globalThis.performance = originalPerformance
  }
})
