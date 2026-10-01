import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const moduleUrl = source => 'data:text/javascript;base64,' + Buffer.from(ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText).toString('base64')
const driftUrl = moduleUrl(await readFile('src/composables/driftDiagnostics.ts', 'utf8'))
const { sanitizeDriftSample, sanitizeDriftSamples } = await import(driftUrl)
const healthSource = (await readFile('src/composables/useClientHealth.ts', 'utf8'))
  .replace("'./driftDiagnostics'", JSON.stringify(driftUrl))
const health = await import(moduleUrl(healthSource))
const t = '2026-10-01T01:24:06.933Z'
const sample = { t, lagMs: 5601, currentTime: 10, playbackRate: 1.08,
  buffered: [[5, 15.601]], sourceBuffered: [[4, 16]], generation: 3,
  playbackAdvanceMs: 1080, bufferAdvanceMs: 1000, lagChangeMs: -80,
  hasAudio: true, futureSafeKeyframes: 0, visibility: 'visible',
  fragmentsSinceSample: 6, maxFragmentGapMs: 700, maxAppendDurationMs: 12 }

test('drift evidence is allowlisted, finite, bounded and independently copied', () => {
  const actual = sanitizeDriftSample({ ...sample, currentTime: Infinity, token: 'secret',
    buffered: [[0, 1], [2, 1], ['url', 4], [3, NaN]] })
  assert.equal(actual.currentTime, undefined)
  assert.equal(actual.token, undefined)
  assert.deepEqual(actual.buffered, [[0, 1]])
  assert.equal(actual.lagMs, 5601)
  assert.equal(actual.maxFragmentGapMs, 700)
  assert.equal(sanitizeDriftSample({ t: 'invalid' }), undefined)
  assert.equal(sanitizeDriftSamples(Array(100).fill(sample)).length, 60)
  const copied = sanitizeDriftSample(sample)
  copied.buffered[0][0] = 99
  assert.equal(sample.buffered[0][0], 5)
})

test('local health export retains drift cause after recovery and event-ring eviction', () => {
  globalThis.__BUILD_HASH__ = 'test-build'
  globalThis.location = { pathname: '/' }
  globalThis.document = { visibilityState: 'visible' }
  globalThis.window = { _witnessMseDiag: { '13_mse': { snapshot: () => ({
    currentState: { readyState: 3, playbackQuality: {} }, liveState: { latencyMs: 1300 },
    stats: { restartCount: 1 }, importantEvents: Array(80).fill({ t, type: 'skippedPartial' }),
    driftSamples: [sample], lastDriftRestart: { t, type: 'driftRestart', lagMs: 5601,
      drift: sample, driftSamples: [sample], credential: 'do-not-export' },
  }) } } }
  const player = health.collectLocalClientHealth().players[0]
  assert.equal(player.recentEvents.length, 40)
  assert.equal(player.lastDriftRestart.lagMs, 5601)
  assert.equal(player.lastDriftRestart.drift.playbackRate, 1.08)
  assert.equal(player.lastDriftRestart.credential, undefined)
  assert.deepEqual(player.lastDriftRestart.driftSamples, [sample])
  assert.equal(player.driftSamples[0].lagChangeMs, -80)
})

test('cross-tab validation preserves bounded drift evidence without arbitrary payloads', async () => {
  const session = health.collectLocalClientHealth()
  session.players[0].driftSamples = Array(120).fill({ ...sample, secret: 'discard' })
  const savedChannel = globalThis.BroadcastChannel
  const savedWindow = globalThis.window
  globalThis.window = { ...savedWindow, setTimeout }
  globalThis.BroadcastChannel = class {
    addEventListener(_name, handler) { this.handler = handler }
    postMessage(request) { queueMicrotask(() => this.handler({ data: { type: 'response', requestId: request.requestId, session } })) }
    close() {}
  }
  try {
    const result = await health.collectClientHealthSessions(1)
    assert.equal(result.mode, 'broadcast')
    const player = result.sessions[0].players[0]
    assert.equal(player.driftSamples.length, 60)
    assert.equal(player.driftSamples[0].secret, undefined)
    assert.equal(player.lastDriftRestart.drift.buffered[0][1], 15.601)
  } finally { globalThis.BroadcastChannel = savedChannel; globalThis.window = savedWindow }
})
