// Exercise compiled component setup logic, not a physical browser/camera.
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
import { parse, compileScript } from '@vue/compiler-sfc'
import * as Vue from 'vue'

async function component(name) {
  const { descriptor } = parse(await readFile(`src/components/camera/${name}.vue`, 'utf8'))
  const source = compileScript(descriptor, { id: name }).content
    .replace(/import\s*\{([^}]+)\}\s*from ['"]vue['"]/g, (_, names) =>
      `const { ${names.replace(/\s+as\s+/g, ': ')} } = globalThis.__fullscreenVue`)
    .replace(/import\s*\{([^}]+)\}\s*from ['"][^'"]+['"]/g, (_, names) =>
      `const { ${names} } = globalThis.__fullscreenImports`)
    .replace(/import\s+\w+\s+from ['"][^'"]+\.vue['"]/g, match =>
      `const ${match.match(/import\s+(\w+)/)[1]} = {}`)
  const compiled = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
  }).outputText
  return (await import('data:text/javascript;base64,' + Buffer.from(compiled).toString('base64'))).default
}
function fixture() {
  const cleanup = [], recording = [], events = []
  const settings = Vue.reactive({ fullscreenMode: true, streamingMode: 'jpeg',
    toggleFullscreen() { this.fullscreenMode = !this.fullscreenMode } })
  const cameras = [10, 12].map(id => ({ id, name: 'Fixture', status: 'Connected', ptzEnabled: true }))
  globalThis.__fullscreenVue = { ...Vue, onMounted() {}, onUnmounted(fn) { cleanup.push(fn) } }
  globalThis.__fullscreenImports = {
    useRouter: () => ({ push() {} }), useSettingsStore: () => settings,
    useCameraStore: () => ({ cameras, toggleRecording(id) { recording.push(id) } }),
    useLiveAudio: () => ({ mode: Vue.ref('off'), enabled: Vue.ref(false), toggleAudio() {}, toggleMotionAudio() {} }),
  }
  globalThis.localStorage = { getItem() { return null }, setItem() {} }
  globalThis.window = new EventTarget()
  globalThis.document = new EventTarget()
  globalThis.Element = class Element {}
  const scope = Vue.effectScope()
  return { settings, cameras, recording, events,
    setup(component, props) { return scope.run(() => component.setup(props, { expose() {}, emit(...args) { events.push(args) } })) },
    close() { for (const fn of cleanup) fn(); scope.stop() },
  }
}

test('fullscreen selection is independent of layout focus and Escape deselects before exiting', async () => {
  const f = fixture()
  const props = Vue.reactive({ editingLayout: false, focusedCameraId: 12 })
  const state = f.setup(await component('CameraGrid'), props)
  try {
    state.selectControls(10)
    assert.equal(state.selectedControlsCameraId.value, 10)
    assert.equal(props.focusedCameraId, 12)
    const panelTarget = new Element()
    panelTarget.closest = () => ({ getAttribute: () => '10' })
    state.onOutsidePointer({ target: panelTarget })
    assert.equal(state.selectedControlsCameraId.value, 10)
    state.onKeydown({ key: 'Escape' })
    assert.equal(state.selectedControlsCameraId.value, null)
    assert.equal(f.settings.fullscreenMode, true)
    state.onKeydown({ key: 'Escape' })
    assert.equal(f.settings.fullscreenMode, false)
    f.settings.fullscreenMode = true
    state.selectControls(12)
    state.onOutsidePointer({ target: {} })
    assert.equal(state.selectedControlsCameraId.value, null)
    props.editingLayout = true; await Vue.nextTick()
    state.selectControls(10)
    assert.equal(state.selectedControlsCameraId.value, null)
    state.onKeydown({ key: 'Escape' })
    assert.deepEqual(f.events, [['cancelLayout']])
  } finally { f.close() }
})

test('fullscreen camera clicks do not record; normal tile clicks retain their shortcut', async t => {
  const f = fixture()
  const props = Vue.reactive({ camera: f.cameras[0], controlsSelected: true })
  const state = f.setup(await component('CameraCard'), props)
  t.mock.timers.enable({ apis: ['setTimeout'] })
  try {
    state.onSingleClick(); t.mock.timers.tick(300)
    assert.deepEqual(f.recording, [])
    state.showPtzControls.value = true
    props.controlsSelected = false; await Vue.nextTick()
    assert.equal(state.showPtzControls.value, false)
    f.settings.fullscreenMode = false
    state.onSingleClick(); t.mock.timers.tick(300)
    assert.deepEqual(f.recording, [10])
  } finally { f.close(); t.mock.timers.reset() }
})
