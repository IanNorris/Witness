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
    getCameraStreamChoice() { return this.cameraStreamChoice ?? 'auto' },
    toggleFullscreen() { this.fullscreenMode = !this.fullscreenMode } })
  const cameras = Vue.reactive([10, 12].map(id => ({ id, name: 'Fixture', status: 'Connected', ptzEnabled: true })))
  globalThis.__fullscreenVue = { ...Vue, onMounted() {}, onUnmounted(fn) { cleanup.push(fn) } }
  globalThis.__fullscreenImports = {
    cameraStreamInfo: (camera, tier) => ({ tier, codec: camera.codec }),
    ...layoutFunctions,
    useRouter: () => ({ push() {} }), useSettingsStore: () => settings,
    useCameraStore: () => ({ cameras, getCameraById: id => cameras.find(camera => camera.id === id), toggleRecording(id) { recording.push(id) } }),
    useGroupStore: () => ({ groups: [], activeGroups: [], camerasInGroup: () => cameras }),
    useClipStore: () => ({ recentClips: [] }),
    useLiveAudio: () => ({ mode: Vue.ref('off'), enabled: Vue.ref(false), toggleAudio() {}, toggleMotionAudio() {} }),
    useDashboardPlayback: () => ({ anyReplay: Vue.ref(false), sync: Vue.ref(false), selected: Vue.ref(null), get: () => ({ requestedAt: null }) }),
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

test('pinned stream choices disable adaptation, while snapshots stay snapshots', async () => {
  const f = fixture()
  const previousMediaSource = globalThis.MediaSource
  globalThis.MediaSource = { isTypeSupported: () => true }
  const camera = Vue.reactive({ id: 12, name: 'Fixture', status: 'Connected', codec: 'hevc', hasSubStream: true, subCodec: 'h264' })
  try {
    f.settings.streamingMode = 'mse'
    const state = f.setup(await component('CameraCard'), { camera })
    assert.equal(state.adaptiveStreamEligible.value, true)
    f.settings.cameraStreamChoice = 'main'
    assert.equal(state.adaptiveStreamEligible.value, false)
    assert.equal(state.useSubStream.value, false)
    f.settings.cameraStreamChoice = 'sub'
    assert.equal(state.adaptiveStreamEligible.value, false)
    assert.equal(state.useSubStream.value, true)
    assert.equal(state.mseCodecHint.value, 'avc1.42001e')
    f.settings.streamingMode = 'jpeg'
    assert.equal(state.effectiveModeWithFallback.value, 'jpeg')
  } finally { globalThis.MediaSource = previousMediaSource; f.close() }
})

test('timeline commits the held slider draft instead of an advancing playback clock', async () => {
  const f = fixture(), requested = []
  const position = Vue.reactive({ requestedAt: 100, viewedAt: 100, timelineEnd: null })
  const playback = { get: () => position, seek: (...args) => requested.push(args),
    live() {}, hold() {}, touch() {} }
  globalThis.__fullscreenVue.inject = () => playback
  globalThis.__fullscreenImports.isSavingAt = () => false
  const state = f.setup(await component('CameraPlaybackBar'), { cameraId: 12 })
  try {
    state.draftTime.value = 75
    position.viewedAt = 103
    // Even if the native control reports the old clock, retain the input draft.
    state.commitSeek({ target: { value: '103' } })
    assert.deepEqual(requested, [[12, 75, false]])
    assert.equal(state.draftTime.value, null)
    state.commitSeek({ target: { value: '80' } })
    assert.deepEqual(requested[1], [12, 80, false])
  } finally { f.close() }
})

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

test('generated layouts respect visibility for display and activity conflicts without replacing custom rectangles', async () => {
  const f = fixture()
  const state = f.setup(await component('DashboardView'), {})
  try {
    const custom = structuredClone(Vue.toRaw(state.dashboardLayout.value.tiles))
    state.dashboardLayout.value.priorityCameraIds = [12, 10]
    state.dashboardLayout.value.visibleCameraIds = [10]
    state.showRecentActivity.value = true
    state.dashboardLayout.value.activityPlacement = 'rectangle'
    // The second hero slot is free when only camera 10 is visible.
    state.dashboardLayout.value.activityRegion = { x: 8, y: 0, width: 4, height: 4 }
    state.changePreset({ target: { value: 'hero7' } })
    assert.equal(state.dashboardLayout.value.preset, 'hero7')
    assert.deepEqual(state.displayedCameraIds.value, [10])
    assert.deepEqual(state.regularTiles.value.map(tile => tile.cameraId), [10])
    assert.equal(state.regularActivityConflict.value, false)
    assert.equal(state.rectangleActive.value, true)
    assert.deepEqual(state.dashboardLayout.value.tiles, custom)
    // Changing the ranking cannot resurrect the hidden camera.
    state.moveCameraPriority(10, -1)
    assert.deepEqual(state.displayedCameraIds.value, [10])
    // Turning it back on now fills the formerly empty slot and docks activity.
    state.dashboardLayout.value.visibleCameraIds = [10, 12]
    assert.deepEqual(state.displayedCameraIds.value, [10, 12])
    assert.equal(state.regularActivityConflict.value, true)
    assert.equal(state.rectangleActive.value, false)
    state.dashboardLayout.value.activityPlacement = 'dock'
    for (const preset of ['balanced8', 'grid16', 'custom']) {
      state.dashboardLayout.value.visibleCameraIds = [10]
      state.changePreset({ target: { value: preset } })
      assert.deepEqual(state.displayedCameraIds.value, [10])
      assert.deepEqual(state.dashboardLayout.value.tiles, custom)
    }
    state.dashboardLayout.value.visibleCameraIds = []
    state.changePreset({ target: { value: 'grid16' } })
    assert.deepEqual(state.displayedCameraIds.value, [])
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
    const mountedCameras = grid.filteredCameras.value
    props.visibleCameraIds = [12]; await Vue.nextTick()
    assert.equal(grid.filteredCameras.value, mountedCameras)
    assert.equal(grid.isCameraVisible(10), false)
    assert.equal(grid.isCameraVisible(12), true)
    assert.equal(grid.selectedControlsCameraId.value, null)
    props.visibleCameraIds = []; await Vue.nextTick()
    assert.equal(grid.filteredCameras.value, mountedCameras)
    assert.equal(grid.isCameraVisible(12), false)
    props.visibleCameraIds = [10]; await Vue.nextTick()
    grid.selectControls(10)
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

test('activity rectangle clamps edits, blocks overlap saves, and falls back to a dock outside editing', async () => {
  const f = fixture()
  const saved = []
  globalThis.localStorage.setItem = (...args) => saved.push(args)
  const state = f.setup(await component('DashboardView'), {})
  try {
    state.beginLayoutEdit()
    state.setActivityPlacement('rectangle')
    state.dashboardLayout.value.tiles = [
      { cameraId: 10, x: 0, y: 0, width: 6, height: 6 },
      { cameraId: 12, x: 6, y: 0, width: 6, height: 6 },
    ]
    state.dashboardLayout.value.activityRegion = { x: 0, y: 6, width: 12, height: 6 }
    assert.equal(state.rectangleActive.value, true)
    assert.equal(state.canSaveLayout.value, true)
    state.setActivityCoordinate('y', { target: { value: '0' } })
    assert.equal(state.canSaveLayout.value, false)
    state.saveLayout()
    assert.equal(state.editingLayout.value, true)
    assert.equal(saved.length, 0)
    state.setActivityCoordinate('x', { target: { value: '99' } })
    assert.deepEqual(state.dashboardLayout.value.activityRegion, { x: 11, y: 0, width: 1, height: 6 })
    state.setActivityCoordinate('y', { target: { value: '6' } })
    state.saveLayout()
    assert.equal(state.editingLayout.value, false)
    assert.equal(saved.length, 1)
    state.dashboardLayout.value.tiles[1].height = 12
    assert.equal(state.rectangleActive.value, false)
    assert.equal(state.regularActivityConflict.value, true)
    state.beginLayoutEdit()
    assert.equal(state.rectangleActive.value, true)
    state.cancelLayoutEdit()
    assert.equal(state.rectangleActive.value, false)
  } finally { f.close() }
})

test('activity drag uses the canvas bounds and cancellation removes listeners', async () => {
  const f = fixture()
  f.settings.fullscreenMode = false
  const state = f.setup(await component('DashboardView'), {})
  try {
    state.selectDashboardMode('layout')
    state.beginLayoutEdit()
    state.stageRef.value = { getBoundingClientRect: () => ({ width: 1200, height: 600 }) }
    state.dashboardLayout.value.activityRegion = { x: 0, y: 0, width: 4, height: 4 }
    state.startActivityDrag({ clientX: 0, clientY: 0, preventDefault() {} }, 'move')
    state.moveActivity({ clientX: 300, clientY: 100 })
    assert.deepEqual(state.dashboardLayout.value.activityRegion, { x: 3, y: 2, width: 4, height: 4 })
    state.stopActivityDrag()
    state.moveActivity({ clientX: 9999, clientY: 9999 })
    assert.equal(state.dashboardLayout.value.activityRegion.x, 3)
    state.startActivityDrag({ clientX: 0, clientY: 0, preventDefault() {} }, 'resize')
    state.moveActivity({ clientX: 9999, clientY: 9999 })
    assert.deepEqual(state.dashboardLayout.value.activityRegion, { x: 3, y: 2, width: 9, height: 10 })
    state.cancelLayoutEdit()
    const restored = JSON.stringify(state.dashboardLayout.value)
    state.moveActivity({ clientX: -999, clientY: -999 })
    assert.equal(JSON.stringify(state.dashboardLayout.value), restored)
  } finally { f.close() }
})

test('activity overlap disables focus and clears existing manual selection', async () => {
  const f = fixture()
  const state = f.setup(await component('DashboardView'), {})
  try {
    state.beginLayoutEdit()
    state.dashboardLayout.value.tiles = [
      { cameraId: 10, x: 0, y: 0, width: 6, height: 6 },
      { cameraId: 12, x: 6, y: 0, width: 6, height: 6 },
    ]
    state.focusCamera(10)
    assert.equal(state.focusedCameraId.value, 10)
    state.dashboardLayout.value.activityRegion = { x: 0, y: 6, width: 12, height: 6 }
    state.setActivityPlacement('rectangle')
    await Vue.nextTick()
    assert.equal(state.focusAllowed.value, false)
    assert.equal(state.focusedCameraId.value, null)
    state.toggleAutoFocus()
    assert.equal(state.dashboardLayout.value.autoFocusEnabled, false)
    state.dashboardLayout.value.focusRegion = { x: 0, y: 0, width: 6, height: 6 }
    state.dashboardLayout.value.focusSlots = [{ x: 6, y: 0, width: 6, height: 6 }]
    await Vue.nextTick()
    assert.equal(state.focusAllowed.value, true)
    state.toggleAutoFocus()
    assert.equal(state.dashboardLayout.value.autoFocusEnabled, true)
    assert.equal(state.canSaveLayout.value, true)
  } finally { f.close() }
})

test('cancelling a camera drag cannot write its stale layout after the editor closes', async () => {
  const f = fixture()
  const props = Vue.reactive({ editingLayout: true, fullscreenLayout: [
    { cameraId: 10, x: 0, y: 0, width: 6, height: 6 },
    { cameraId: 12, x: 6, y: 0, width: 6, height: 6 },
  ] })
  const state = f.setup(await component('CameraGrid'), props)
  try {
    state.gridRef.value = { getBoundingClientRect: () => ({ width: 1200, height: 600 }) }
    state.startPointer({ clientX: 0, clientY: 0, preventDefault() {}, stopPropagation() {} }, 10, 'move')
    state.onPointerMove({ clientX: 600, clientY: 0 })
    assert.equal(state.invalidPlacement.value, true)
    const count = f.events.length
    props.editingLayout = false
    // Pointerup can race Vue's queued watcher; check the synchronous guard too.
    state.finishPointer()
    state.onPointerMove({ clientX: 0, clientY: 0 })
    await Vue.nextTick()
    assert.equal(f.events.length, count)
    assert.equal(state.activeCameraId.value, null)
  } finally { f.close() }
})

test('automatic slots hold stopped motion without unrelated updates extending the deadline', async t => {
  const f = fixture()
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const state = f.setup(await component('DashboardView'), {})
  try {
    state.dashboardLayout.value.visibleCameraIds = [10]
    state.dashboardLayout.value.priorityCameraIds = [12, 10]
    state.dashboardLayout.value.automaticSlots = [{ anchorCameraId: 10, fallbackCameraId: 10 }]
    state.dashboardLayout.value.slotHoldSeconds = 3
    assert.deepEqual(state.displayedCameraIds.value, [10])
    f.cameras[1].motionActive = true
    await Vue.nextTick()
    assert.deepEqual(state.displayedCameraIds.value, [12])
    assert.equal(state.editorTiles.value[0].x, state.dashboardLayout.value.tiles[0].x)
    f.cameras[1].motionActive = false
    await Vue.nextTick()
    t.mock.timers.tick(1000)
    f.cameras[0].motionActive = true
    await Vue.nextTick()
    assert.deepEqual(state.displayedCameraIds.value, [12])
    t.mock.timers.tick(2000)
    assert.deepEqual(state.displayedCameraIds.value, [10])
    assert.equal(f.cameras.length, 2) // assignment never mutates the mounted source set
    state.beginLayoutEdit()
    assert.deepEqual(state.editorTiles.value.map(tile => tile.cameraId), [10, 12])
    state.cancelLayoutEdit()
    state.clearAutomaticSlots()
    assert.equal(state.slotReleaseTimers.size, 0)
    assert.deepEqual(state.heldActiveCameraIds.value, [])
  } finally { f.close(); t.mock.timers.reset() }
})

test('a hidden camera rectangle does not prevent arranging the visible layout', async () => {
  const f = fixture()
  const props = Vue.reactive({ editingLayout: true, visibleCameraIds: [10], fullscreenLayout: [
    { cameraId: 10, x: 0, y: 0, width: 6, height: 6 },
    { cameraId: 12, x: 6, y: 0, width: 6, height: 6 },
  ] })
  const state = f.setup(await component('CameraGrid'), props)
  try {
    state.gridRef.value = { getBoundingClientRect: () => ({ width: 1200, height: 600 }) }
    state.startPointer({ clientX: 0, clientY: 0, preventDefault() {}, stopPropagation() {} }, 10, 'move')
    state.onPointerMove({ clientX: 600, clientY: 0 })
    assert.equal(state.invalidPlacement.value, false)
    state.finishPointer()
    assert.equal(f.events.at(-1)[1][0].x, 6)
  } finally { f.close() }
})
