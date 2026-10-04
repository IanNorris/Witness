// Exercise compiled component setup logic, not a physical browser/camera.
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
import { parse, compileScript } from '@vue/compiler-sfc'
import * as Vue from 'vue'
let componentInstance = 0
const layoutSource = ts.transpileModule(await readFile('src/types/dashboardLayout.ts', 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText
const layoutFunctions = await import('data:text/javascript;base64,' + Buffer.from(layoutSource).toString('base64'))

async function component(name) {
  const path = name === 'DashboardView' ? 'src/views/DashboardView.vue' : `src/components/camera/${name}.vue`
  const { descriptor } = parse(await readFile(path, 'utf8'))
  const source = compileScript(descriptor, { id: name }).content
    .replace(/import\s*\{([^}]+)\}\s*from ['"]vue['"]/g, (_, names) =>
      `const { ${names.replace(/\s+as\s+/g, ': ')} } = globalThis.__fullscreenVue`)
    .replace(/import\s*\{([^}]+)\}\s*from ['"][^'"]+['"]/g, (_, names) =>
      `const { ${names.replace(/\btype\s+\w+\s*,?/g, '')} } = globalThis.__fullscreenImports`)
    .replace(/import\s+\w+\s+from ['"][^'"]+\.vue['"]/g, match =>
      `const ${match.match(/import\s+(\w+)/)[1]} = {}`)
  const compiled = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
  }).outputText
  // Each fixture supplies different stores; avoid cached module-level bindings.
  return (await import('data:text/javascript;base64,' + Buffer.from(compiled + `\n// fixture ${componentInstance++}`).toString('base64'))).default
}
function fixture() {
  const cleanup = [], recording = [], events = []
  const settings = Vue.reactive({ fullscreenMode: true, streamingMode: 'jpeg',
    toggleFullscreen() { this.fullscreenMode = !this.fullscreenMode } })
  const cameras = [10, 12].map(id => ({ id, name: 'Fixture', status: 'Connected', ptzEnabled: true }))
  globalThis.__fullscreenVue = { ...Vue, onMounted() {}, onUnmounted(fn) { cleanup.push(fn) } }
  globalThis.__fullscreenImports = {
    ...layoutFunctions,
    useRouter: () => ({ push() {} }), useSettingsStore: () => settings,
    useCameraStore: () => ({ cameras, toggleRecording(id) { recording.push(id) } }),
    useGroupStore: () => ({ groups: [], activeGroups: [], camerasInGroup: () => cameras }),
    useClipStore: () => ({ recentClips: [] }),
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

test('automatic focus hold is not extended by unrelated motion updates', async t => {
  const f = fixture()
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const state = f.setup(await component('DashboardView'), {})
  try {
    state.dashboardLayout.value.autoFocusEnabled = true
    state.dashboardLayout.value.focusHoldSeconds = 3
    f.cameras[0].motionActive = true
    state.updateAutomaticFocus()
    assert.equal(state.focusedCameraId.value, 10)
    f.cameras[0].motionActive = false
    f.cameras[1].motionActive = true
    state.updateAutomaticFocus()
    t.mock.timers.tick(1000)
    state.updateAutomaticFocus()
    t.mock.timers.tick(2000)
    assert.equal(state.focusedCameraId.value, 12)
    state.moveCameraPriority(12, -1)
    f.cameras[0].motionActive = true
    state.updateAutomaticFocus()
    assert.equal(state.focusedCameraId.value, 12)
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

test('windowed canvas keeps every group camera mounted and selects controls without recording', async t => {
  const f = fixture()
  f.settings.fullscreenMode = false
  const props = Vue.reactive({ layoutMode: true, visibleCameraIds: [10] })
  const grid = f.setup(await component('CameraGrid'), props)
  const cardProps = Vue.reactive({ camera: f.cameras[0], layoutMode: true, controlsSelected: true })
  const card = f.setup(await component('CameraCard'), cardProps)
  t.mock.timers.enable({ apis: ['setTimeout'] })
  try {
    assert.equal(grid.filteredCameras.value.length, 2)
    assert.equal(grid.isCameraVisible(12), false)
    grid.selectControls(10)
    assert.equal(grid.selectedControlsCameraId.value, 10)
    card.onSingleClick(); t.mock.timers.tick(300)
    assert.deepEqual(f.recording, [])
    grid.onKeydown({ key: 'Escape' })
    assert.equal(grid.selectedControlsCameraId.value, null)
    assert.equal(f.settings.fullscreenMode, false)
    props.layoutMode = false; await Vue.nextTick()
    assert.equal(grid.filteredCameras.value.length, 2)
    assert.equal(grid.isCameraVisible(12), true)
    cardProps.layoutMode = false
    card.onSingleClick()
    cardProps.layoutMode = true
    t.mock.timers.tick(300)
    assert.deepEqual(f.recording, []) // entering Layout cancels a pending tile shortcut
  } finally { f.close() }
})
