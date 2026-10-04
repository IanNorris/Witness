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
  assert.equal(original.players[0].recentEvents.length, 40)
  const many = { ...original, players: Array.from({ length: 64 }, (_, index) => ({
    ...original.players[0], id: `${index}_mse`, driftSamples: Array(60).fill({ t, buffered: Array(8).fill([1, 2]),
      sourceBuffered: Array(8).fill([1, 2]), lagMs: 1234, appendQueueBytes: 12345, schedulingDelayMs: 9999 }),
  })) }
  const bounded = health.compactHealthReport(many)
  assert.equal(bounded.players.length, 64)
  assert.ok(new TextEncoder().encode(JSON.stringify(bounded)).length < 64 * 1024)
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
