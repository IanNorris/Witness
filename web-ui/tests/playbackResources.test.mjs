import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
import * as Vue from 'vue'
const cleanup = []
globalThis.__resourceVue = { ...Vue, onUnmounted: fn => cleanup.push(fn) }
globalThis.window = {}
const source = (await readFile('src/composables/usePlaybackResources.ts', 'utf8'))
  .replace(/import \{[^}]+\} from 'vue'/, 'const { ref, watch, onUnmounted } = globalThis.__resourceVue')
const output = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
const resources = await import('data:text/javascript;base64,' + Buffer.from(output).toString('base64'))
test('decode pressure is suspected only for decode errors with multiple loaded resources', () => {
  assert.equal(resources.suspectDecoderPressure(3, [{}, {}]), true)
  for (const code of [0, 1, 2, 4]) assert.equal(resources.suspectDecoderPressure(code, [{}, {}]), false)
  assert.equal(resources.suspectDecoderPressure(3, [{}]), false)
})
test('counts paused loaded streams, captures context once and releases closed players', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 1_000_000 })
  const makeVideo = () => Object.assign(new EventTarget(), { src: 'blob:synthetic', currentSrc: '', videoWidth: 1920, videoHeight: 1080, readyState: 1, paused: true, error: null })
  const first = Vue.shallowRef(makeVideo()), second = Vue.shallowRef(makeVideo())
  const logs = []
  resources.usePlaybackResources(12, first, 'mse', () => ({ tier: 'main', codec: 'hevc' }), (...args) => logs.push(args))
  resources.usePlaybackResources(10, second, 'dvr', () => ({ tier: 'dvr' }))
  await Vue.nextTick()
  assert.equal(resources.playbackResourceSnapshot().length, 2)
  first.value.error = { code: 2 }; first.value.dispatchEvent(new Event('error'))
  assert.equal(resources.decoderPressureWarning.value, null)
  first.value.error = { code: 3 }; first.value.dispatchEvent(new Event('error'))
  assert.equal(resources.decoderPressureWarning.value, null, 'held while the camera may still recover')
  t.mock.timers.tick(resources.PRESSURE_GRACE_MS)
  const warning = resources.decoderPressureWarning.value
  assert.equal(warning.activeStreams, 2)
  assert.equal(warning.streams[0].codec, 'hevc')
  assert.equal(warning.streams[0].width, 1920)
  resources.decoderPressureWarning.value = null
  first.value.dispatchEvent(new Event('error'))
  assert.equal(resources.decoderPressureWarning.value, null, 'cooldown prevents repeated warning spam')
  const evidence = window._witnessPlaybackResources.snapshot()
  assert.equal(evidence.lastFailure.code, 3)
  assert.equal(evidence.lastFailure.streams.length, 2)
  second.value = null; await Vue.nextTick()
  assert.equal(resources.playbackResourceSnapshot().length, 1)
  cleanup.forEach(fn => fn())
  assert.equal(resources.playbackResourceSnapshot().length, 0)
  assert.equal(logs.length, 3)
})

test('a camera that recovers (e.g. degraded from HEVC to its sub-stream) suppresses the warning', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 5_000_000 })
  const failing = Vue.shallowRef(Object.assign(new EventTarget(), { src: 'blob:main', currentSrc: '', videoWidth: 3840, videoHeight: 2160, readyState: 1, paused: false, error: null }))
  const other = Vue.shallowRef(Object.assign(new EventTarget(), { src: 'blob:other', currentSrc: '', videoWidth: 1920, videoHeight: 1080, readyState: 4, paused: false, error: null }))
  resources.usePlaybackResources(21, failing, 'mse', () => ({ tier: 'main', codec: 'hevc' }))
  resources.usePlaybackResources(22, other, 'mse', () => ({ tier: 'main', codec: 'h264' }))
  await Vue.nextTick()
  resources.decoderPressureWarning.value = null
  failing.value.error = { code: 3 }; failing.value.dispatchEvent(new Event('error'))
  // The tile swaps to a sub-stream player that is presenting frames.
  const replacement = Vue.shallowRef(Object.assign(new EventTarget(), { src: 'blob:sub', currentSrc: '', videoWidth: 640, videoHeight: 360, readyState: 4, paused: false, error: null }))
  resources.usePlaybackResources(21, replacement, 'mse', () => ({ tier: 'sub', codec: 'h264' }))
  await Vue.nextTick()
  t.mock.timers.tick(resources.PRESSURE_GRACE_MS)
  assert.equal(resources.decoderPressureWarning.value, null, 'every camera is playing: no warning')
  assert.equal(resources.cameraRecovered(21, [{ cameraId: 21, readyState: 4, paused: false, errored: true }]), false, 'an errored stream does not count as recovered')
})
