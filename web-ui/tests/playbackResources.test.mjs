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
test('counts paused loaded streams, captures context once and releases closed players', async () => {
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
