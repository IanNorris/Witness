import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
import { parse, compileScript } from '@vue/compiler-sfc'
import * as Vue from 'vue'

const moduleUrl = source => 'data:text/javascript;base64,' + Buffer.from(ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText).toString('base64')
const movementUrl = moduleUrl(await readFile('src/composables/ptzMovement.ts', 'utf8'))
const { descriptor } = parse(await readFile('src/components/camera/PtzControls.vue', 'utf8'))
const source = compileScript(descriptor, { id: 'ptz-controls-test' }).content
  .replace(/import\s*\{([^}]+)\}\s*from ['"]vue['"]/g, (_, names) =>
    `const { ${names.replace(/\s+as\s+/g, ': ')} } = globalThis.__ptzVue`)
  .replace("import { api } from '../../composables/useApi'", 'const api = (...args) => globalThis.__ptzFixture.api(...args)')
  .replace("'../../composables/ptzMovement'", JSON.stringify(movementUrl))

function deferred() {
  let resolve
  const promise = new Promise(r => { resolve = r })
  return { promise, resolve }
}
async function fixture(api) {
  const unmount = []
  globalThis.__ptzFixture = { api }
  globalThis.__ptzVue = { ...Vue, onMounted() {}, onUnmounted(fn) { unmount.push(fn) } }
  globalThis.window = new EventTarget()
  globalThis.document = Object.assign(new EventTarget(), { visibilityState: 'visible' })
  // Unique import identity binds each compiled component to its own lifecycle hooks.
  const component = (await import(moduleUrl(source) + '#fixture-' + Math.random())).default
  const scope = Vue.effectScope()
  const props = Vue.reactive({ cameraId: 10 })
  const state = scope.run(() => component.setup(props, { expose() {} }))
  return { state, props, async dispose() {
    for (const fn of unmount) fn()
    scope.stop()
    await state.movement.settled()
  } }
}

test('position discovery is read-only and only offers enabled valid saved positions', async () => {
  const requests = []
  const f = await fixture(async (url, options) => {
    requests.push([url, options.method])
    return { presets: [{ id: 1, name: 'Drive', enabled: true },
      { id: 2, name: 'Disabled', enabled: false }, { id: -1, enabled: true }, { id: 3.5, enabled: true }] }
  })
  try {
    await f.state.fetchPresets()
    assert.deepEqual(f.state.presets.value, [{ id: 1, name: 'Drive' }])
    assert.deepEqual(requests, [['/ptz/10/presets', undefined]])
    f.state.presetId.value = 1
    await f.state.goToPreset()
    assert.deepEqual(requests[1], ['/ptz/10/preset', 'POST'])
  } finally { await f.dispose() }
})

test('stale capability replies cannot overwrite a camera revisited after switching away', async () => {
  const old = deferred()
  let first = true
  const f = await fixture(async url => {
    if (url.endsWith('/presets')) {
      if (first) { first = false; return old.promise }
      return { presets: [{ id: 2, name: 'Current', enabled: true }] }
    }
    return { valid: false }
  })
  try {
    const pending = f.state.fetchPresets()
    f.props.cameraId = 12; await Vue.nextTick()
    f.props.cameraId = 10; await Vue.nextTick()
    await Promise.resolve()
    old.resolve({ presets: [{ id: 1, name: 'Stale', enabled: true }] })
    await pending
    assert.deepEqual(f.state.presets.value, [{ id: 2, name: 'Current' }])
  } finally { await f.dispose() }
})

test('absolute zoom is ordered after Stop and stale polls cannot undo a user edit', async () => {
  const poll = deferred(), requests = []
  const f = await fixture(async (url, options = {}) => {
    requests.push(url)
    if (url.endsWith('/zoom') && !options.method) return poll.promise
    return {}
  })
  try {
    f.state.startMove('left'); await f.state.movement.settled()
    // Fetch after ending the gesture; retain a poll while the user edits zoom.
    f.state.stopMove(); await f.state.movement.settled()
    const pendingPoll = f.state.fetchZoomState()
    f.state.onZoomInput({ target: { value: '12' } })
    poll.resolve({ valid: true, zoom: 1, zoomMin: 0, zoomMax: 33 })
    await pendingPoll
    assert.equal(f.state.zoomPos.value, 12)
    assert.equal(f.state.zoomLoaded.value, false)
    // Cancel debounce on disposal; test the direct absolute command ordering.
    f.state.startMove('right'); await f.state.movement.settled()
    await f.state.setZoomAbsolute(10, 12)
    const tail = requests.slice(-3)
    assert.deepEqual(tail, ['/ptz/10/right', '/ptz/10/stop', '/ptz/10/zoom/set'])
  } finally { await f.dispose() }
})
