import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const source = await readFile('src/types/dashboardLayout.ts', 'utf8')
const code = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
const layout = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'))

test('legacy custom layouts retain rectangles, docks and visibility without losing new cameras', () => {
  const legacy = layout.createDefaultDashboardLayout([8, 9])
  delete legacy.priorityCameraIds
  delete legacy.preset
  delete legacy.activityPlacement
  legacy.activityDock = 'right'
  legacy.visibleCameraIds = [8]
  const restored = layout.normaliseDashboardLayout(legacy, [8, 9])
  assert.deepEqual(restored.tiles, legacy.tiles)
  assert.deepEqual(restored.visibleCameraIds, [8])
  assert.deepEqual(restored.priorityCameraIds, [8, 9])
  assert.equal(restored.preset, 'custom')
  assert.equal(restored.activityPlacement, 'dock')
  assert.equal(restored.activityDock, 'right')
})

test('priority normalisation removes duplicates and deleted IDs and appends new cameras', () => {
  assert.deepEqual(layout.normaliseCameraPriority([9, 9, 99, '8', 8], [8, 9, 10]), [9, 8, 10])
  assert.deepEqual(layout.normaliseCameraPriority(null, [8, 9]), [8, 9])
})

test('generated presets fill slots in priority order and remain bounded and non-overlapping', () => {
  const cameras = Array.from({ length: 20 }, (_, i) => 20 - i)
  for (const [preset, capacity] of [['hero7', 7], ['balanced8', 8], ['grid16', 16]]) {
    const tiles = layout.generatedDashboardTiles(preset, cameras)
    assert.equal(tiles.length, capacity)
    assert.deepEqual(tiles.map(tile => tile.cameraId), cameras.slice(0, capacity))
    for (const [i, tile] of tiles.entries()) {
      assert.ok(tile.x >= 0 && tile.y >= 0 && tile.width > 0 && tile.height > 0)
      assert.ok(tile.x + tile.width <= 12 && tile.y + tile.height <= 12)
      assert.ok(tiles.slice(i + 1).every(other => !layout.dashboardAreasOverlap(tile, other)))
    }
    assert.deepEqual(layout.generatedDashboardTiles(preset, []), [])
    assert.equal(layout.generatedDashboardTiles(preset, [9, 9, 8]).length, 2)
  }
})

test('generated presets filter hidden cameras before capacity without changing priority', () => {
  const priority = Array.from({ length: 20 }, (_, i) => i + 1)
  const original = [...priority]
  const visible = priority.slice(2).reverse() // visibility is a set, not the ranking
  for (const [preset, capacity] of [['hero7', 7], ['balanced8', 8], ['grid16', 16]]) {
    const tiles = layout.generatedDashboardTiles(preset, [1, ...priority], visible)
    assert.equal(tiles.length, capacity)
    assert.deepEqual(tiles.map(tile => tile.cameraId), priority.slice(2, capacity + 2))
    assert.deepEqual(tiles[0], { ...layout.generatedDashboardTiles(preset, priority)[0], cameraId: 3 })
    assert.deepEqual(layout.generatedDashboardTiles(preset, priority, []), [])
    assert.deepEqual(layout.generatedDashboardTiles(preset, priority, [99]), [])
  }
  assert.deepEqual(priority, original)
})

test('preset restoration preserves custom layout and clamps activity rectangles', () => {
  const custom = layout.createDefaultDashboardLayout([8, 9])
  const restored = layout.normaliseDashboardLayout({ ...custom, preset: 'hero7', priorityCameraIds: [9, 8],
    activityPlacement: 'rectangle', activityRegion: { x: 11, y: -2, width: 50, height: 0 } }, [8, 9])
  assert.deepEqual(restored.tiles, custom.tiles)
  assert.deepEqual(restored.priorityCameraIds, [9, 8])
  assert.deepEqual(restored.activityRegion, { x: 11, y: 0, width: 1, height: 1 })
  assert.equal(restored.activityPlacement, 'rectangle')
  assert.equal(restored.preset, 'hero7')
  assert.equal(layout.normaliseDashboardLayout({ ...custom, preset: 'bad' }, [8, 9]).preset, 'custom')
})

test('automatic slot migration removes stale and duplicate anchors without reordering slots', () => {
  const restored = layout.normaliseDashboardLayout({ ...layout.createDefaultDashboardLayout([1, 2, 3]),
    automaticSlots: [null, { anchorCameraId: 2, fallbackCameraId: 3 }, { anchorCameraId: 2, fallbackCameraId: 1 },
      { anchorCameraId: 99, fallbackCameraId: 1 }, { anchorCameraId: 1, fallbackCameraId: 99 }],
    slotHoldSeconds: 999 }, [1, 2, 3])
  assert.deepEqual(restored.automaticSlots, [{ anchorCameraId: 2, fallbackCameraId: 3 }, { anchorCameraId: 1, fallbackCameraId: null }])
  assert.equal(restored.slotHoldSeconds, 120)
  assert.deepEqual(layout.normaliseDashboardLayout(null, [1]).automaticSlots, [])
})

test('automatic slots rank motion, retain fixed cameras, and use fallbacks without duplicating players', () => {
  const config = layout.createDefaultDashboardLayout([1, 2, 3, 4, 5])
  config.visibleCameraIds = [1, 2, 3]
  config.priorityCameraIds = [5, 4, 3, 2, 1]
  config.automaticSlots = [{ anchorCameraId: 2, fallbackCameraId: 2 }, { anchorCameraId: 3, fallbackCameraId: 3 }]
  const ids = active => layout.resolveAutomaticDashboardTiles(config, new Set(active)).map(tile => tile.cameraId)
  assert.deepEqual(ids([]), [1, 2, 3])
  assert.deepEqual(ids([4]), [1, 4, 3])
  assert.deepEqual(ids([1, 4, 5]), [1, 5, 4]) // fixed 1 is never stolen
  assert.deepEqual(ids([3]), [1, 3, 5]) // fallback 3 already in higher slot
  const promoted = layout.resolveAutomaticDashboardTiles(config, new Set([4, 5]))
  assert.deepEqual(promoted[1], { ...config.tiles[1], cameraId: 5, automaticSlotNumber: 1 })
  assert.equal(new Set(promoted.map(tile => tile.cameraId)).size, promoted.length)
  assert.equal(config.tiles[1].cameraId, 2)
  config.visibleCameraIds = [1, 2]
  assert.deepEqual(ids([4, 5]), [1, 5]) // hidden anchor removes its slot
})

test('overlap with hidden camera storage does not erase a saved visible custom layout', () => {
  const config = layout.createDefaultDashboardLayout([1, 2, 3])
  config.visibleCameraIds = [1, 2]
  config.tiles[0] = { cameraId: 1, x: 0, y: 0, width: 8, height: 12 }
  config.tiles[1] = { cameraId: 2, x: 8, y: 0, width: 4, height: 12 }
  const restored = layout.normaliseDashboardLayout(config, [1, 2, 3])
  assert.deepEqual(restored.tiles, config.tiles)
  const malformed = { ...config, tiles: [...config.tiles, config.tiles[2]] }
  assert.deepEqual(layout.normaliseDashboardLayout(malformed, [1, 2, 3]).tiles,
    layout.createDefaultDashboardLayout([1, 2, 3]).tiles)
})
