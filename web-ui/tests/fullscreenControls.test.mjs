// Exercise compiled component setup logic, not a physical browser/camera.
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
import { parse, compileScript } from '@vue/compiler-sfc'
import * as Vue from 'vue'
let componentInstance = 0
const layoutSource = ts.transpileModule(await readFile('src/types/dashboard.ts', 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText
const layoutFunctions = await import('data:text/javascript;base64,' + Buffer.from(layoutSource).toString('base64'))

async function component(name) {
  const path = name === 'DashboardCanvas' ? 'src/components/dashboard/DashboardCanvas.vue' : `src/components/camera/${name}.vue`
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

test('pinned stream choices disable adaptation and legacy snapshot preferences cannot restore JPEG', async () => {
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
    assert.equal(state.effectiveModeWithFallback.value, 'mse')
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

function canvasDocument(tiles) {
  return { ...layoutFunctions.createEmptyDocument(), tiles }
}
const fixedTile = (id, camera, x) => layoutFunctions.defaultTile('camera', id, { x, y: 0, w: 24, h: 24 }, camera)

test('dashboard canvas: Escape unwinds maximise, then selection, then the view; PTZ panels keep selection', async () => {
  const f = fixture()
  globalThis.__fullscreenVue.inject = () => undefined
  const props = Vue.reactive({ document: canvasDocument([fixedTile('a', 10, 0), fixedTile('b', 12, 24)]) })
  const state = f.setup(await component('DashboardCanvas'), props)
  try {
    state.selectControls(10)
    assert.equal(state.selectedControls.value, 10)
    const panelTarget = new Element()
    panelTarget.closest = () => ({ getAttribute: () => '10' })
    state.onOutsidePointer({ target: panelTarget })
    assert.equal(state.selectedControls.value, 10, 'teleported PTZ panel does not dismiss its camera')
    state.toggleMaximize(12)
    assert.equal(state.maximized.value, 12)
    assert.deepEqual([...state.slots.value.keys()], [12], 'maximised camera fills the canvas alone')
    state.onKeydown({ key: 'Escape' })
    assert.equal(state.maximized.value, null)
    assert.equal(state.selectedControls.value, 10)
    state.onKeydown({ key: 'Escape' })
    assert.equal(state.selectedControls.value, null)
    assert.deepEqual(f.events, [])
    state.onKeydown({ key: 'Escape' })
    assert.deepEqual(f.events, [['escape']])
    state.selectControls(12)
    state.onOutsidePointer({ target: {} })
    assert.equal(state.selectedControls.value, null)
  } finally { f.close() }
})

test('dashboard canvas: spotlight promotion never changes the mounted camera set', async t => {
  const f = fixture()
  t.mock.timers.enable({ apis: ['setTimeout', 'setInterval', 'Date'], now: 0 })
  globalThis.__fullscreenVue.inject = () => undefined
  const spotlight = { ...layoutFunctions.defaultTile('spotlight', 's', { x: 24, y: 0, w: 24, h: 24 }), sources: { kind: 'list', cameras: [12] }, idle: { kind: 'blank' }, holdSeconds: 3 }
  const props = Vue.reactive({ document: canvasDocument([fixedTile('a', 10, 0), spotlight]) })
  const state = f.setup(await component('DashboardCanvas'), props)
  try {
    const mounted = state.resolved.value.mounted
    assert.deepEqual(mounted, [10, 12], 'warm sources are mounted while idle')
    assert.equal(state.slots.value.has(12), false, 'idle spotlight parks its source')
    f.cameras[1].motionActive = true
    await Vue.nextTick()
    assert.equal(state.slots.value.get(12).tileId, 's')
    assert.deepEqual(state.resolved.value.mounted, mounted)
    f.cameras[1].motionActive = false
    await Vue.nextTick()
    t.mock.timers.tick(1000)
    f.cameras[0].motionActive = true // unrelated motion must not extend the hold
    await Vue.nextTick()
    assert.equal(state.slots.value.get(12)?.tileId, 's')
    t.mock.timers.tick(2100)
    await Vue.nextTick()
    assert.equal(state.slots.value.has(12), false, 'released after its own hold')
    assert.deepEqual(state.resolved.value.mounted, mounted)
  } finally { f.close(); t.mock.timers.reset() }
})

test('dashboard canvas: replay freezes spotlight changes', async () => {
  const f = fixture()
  const anyReplay = Vue.ref(true)
  globalThis.__fullscreenVue.inject = () => ({ anyReplay, selected: Vue.ref(null), touch() {} })
  const spotlight = { ...layoutFunctions.defaultTile('spotlight', 's', { x: 0, y: 0, w: 24, h: 24 }), sources: { kind: 'list', cameras: [12] }, idle: { kind: 'blank' } }
  const props = Vue.reactive({ document: canvasDocument([spotlight]) })
  const state = f.setup(await component('DashboardCanvas'), props)
  try {
    f.cameras[1].motionActive = true
    await Vue.nextTick()
    assert.equal(state.slots.value.has(12), false, 'no promotion during replay')
    anyReplay.value = false
    await Vue.nextTick()
    assert.equal(state.slots.value.get(12)?.tileId, 's', 'catches up when replay ends')
  } finally { f.close() }
})

test('dashboard camera cards: double-click maximises, tile stream preference yields to the device choice', async () => {
  const f = fixture()
  const previousMediaSource = globalThis.MediaSource
  globalThis.MediaSource = { isTypeSupported: () => true }
  const camera = Vue.reactive({ id: 12, name: 'Fixture', status: 'Connected', codec: 'h264', hasSubStream: true, subCodec: 'h264' })
  try {
    const props = Vue.reactive({ camera, layoutMode: true, doubleClickMaximizes: true, tileStream: 'sub' })
    const state = f.setup(await component('CameraCard'), props)
    assert.equal(state.streamChoice.value, 'sub')
    f.settings.cameraStreamChoice = 'main'
    assert.equal(state.streamChoice.value, 'main', 'per-device override wins')
    state.onDoubleClick()
    assert.deepEqual(f.events.at(-1), ['toggleMaximize', 12])
    assert.equal(f.settings.fullscreenMode, true, 'double-click no longer exits fullscreen on dashboards')
  } finally { globalThis.MediaSource = previousMediaSource; f.close() }
})

test('seamless stream switch keeps the visible player until the new one presents a frame', async t => {
  const f = fixture()
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const previousMediaSource = globalThis.MediaSource
  globalThis.MediaSource = { isTypeSupported: () => true }
  const camera = Vue.reactive({ id: 12, name: 'Fixture', status: 'Connected', codec: 'h264', hasSubStream: true, subCodec: 'h264' })
  try {
    const props = Vue.reactive({ camera, layoutMode: true, tileStream: 'main', seamlessSwitch: true })
    const state = f.setup(await component('CameraCard'), props)
    const original = state.activePlayer.value.key
    assert.equal(state.players.value.length, 1)
    props.tileStream = 'sub'; await Vue.nextTick()
    assert.equal(state.players.value.length, 2, 'new stream prepared alongside the visible one')
    assert.equal(state.activePlayer.value.key, original, 'visible player untouched while preparing')
    assert.equal(state.players.value[1].useSub, true)
    state.onPlayerReady(original) // the old player re-reporting must not swap anything
    assert.equal(state.players.value.length, 2)
    state.onPlayerReady(state.players.value[1].key)
    assert.equal(state.players.value.length, 1)
    assert.equal(state.activePlayer.value.useSub, true, 'swapped once the new stream showed a frame')
    // A stream that never presents a frame is cut over after the timeout.
    props.tileStream = 'main'; await Vue.nextTick()
    assert.equal(state.players.value.length, 2)
    t.mock.timers.tick(8000)
    assert.equal(state.players.value.length, 1)
    assert.equal(state.activePlayer.value.useSub, false)
    // Without seamless switching (or when hidden) the change is immediate.
    props.seamlessSwitch = false
    props.tileStream = 'sub'; await Vue.nextTick()
    assert.equal(state.players.value.length, 1)
    assert.equal(state.activePlayer.value.useSub, true)
  } finally { globalThis.MediaSource = previousMediaSource; f.close(); t.mock.timers.reset() }
})

test('dashboard canvas picks sub-streams for small and parked tiles, main for large ones', async () => {
  const f = fixture()
  globalThis.__fullscreenVue.inject = () => undefined
  globalThis.window.devicePixelRatio = 1
  for (const camera of f.cameras) Object.assign(camera, { hasSubStream: true, subWidth: 640, subHeight: 360, width: 1920, height: 1080 })
  const big = layoutFunctions.defaultTile('camera', 'a', { x: 0, y: 0, w: 36, h: 36 }, 10)
  const small = layoutFunctions.defaultTile('camera', 'b', { x: 36, y: 0, w: 12, h: 12 }, 12)
  const props = Vue.reactive({ document: canvasDocument([big, small]) })
  const state = f.setup(await component('DashboardCanvas'), props)
  try {
    state.canvasSize.value = { width: 1920, height: 1080 }
    assert.equal(state.streamFor(10), 'main', '1440×810 tile needs the main stream')
    assert.equal(state.streamFor(12), 'sub', '480×270 tile uses the sub-stream')
    props.document.tiles[0].stream = 'sub'
    assert.equal(state.streamFor(10), 'sub', 'pinned sub wins')
    props.document.behavior.mainStreams = 1
    props.document.tiles[0].stream = 'auto'
    props.document.tiles[1].stream = 'main'
    assert.equal(state.streamFor(12), 'main', 'pinned main kept')
    assert.equal(state.streamFor(10), 'sub', 'budget of one is spent by the pinned tile')
  } finally { f.close() }
})

test('unplayable main streams fall back to a playable sub-stream unless Main is pinned on the device', async () => {
  const f = fixture()
  const previousMediaSource = globalThis.MediaSource
  // A wall display without HEVC: only H.264 decodes.
  globalThis.MediaSource = { isTypeSupported: mime => !/hev1|hvc1/.test(mime) }
  const camera = Vue.reactive({ id: 12, name: 'Fixture', status: 'Connected', codec: 'hevc', hasSubStream: true, subCodec: 'h264' })
  try {
    const props = Vue.reactive({ camera, layoutMode: true, tileStream: 'main' })
    const state = f.setup(await component('CameraCard'), props)
    assert.equal(state.useSubStream.value, true, 'dashboard asking for Main still degrades to the H.264 sub-stream')
    assert.equal(state.effectiveModeWithFallback.value, 'mse')
    f.settings.cameraStreamChoice = 'main'
    assert.equal(state.useSubStream.value, false, 'an explicit per-device Main pin is honoured')
    assert.equal(state.effectiveModeWithFallback.value, 'unsupported')
    f.settings.cameraStreamChoice = 'auto'
    // Runtime rejection of an H.264 main stream also drops to the sub-stream.
    camera.codec = 'h264'; await Vue.nextTick()
    assert.equal(state.useSubStream.value, false)
    state.hlsPlayerRef.value = { codecUnsupported: true }
    await Vue.nextTick()
    assert.equal(state.useSubStream.value, true)
    assert.equal(state.codecFallback.value, false)
  } finally { globalThis.MediaSource = previousMediaSource; f.close() }
})
