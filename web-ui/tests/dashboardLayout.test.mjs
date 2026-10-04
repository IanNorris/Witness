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
