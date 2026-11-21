import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const code = ts.transpileModule(await readFile('src/types/dashboard.ts', 'utf8'), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
const model = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'))

function doc(tiles, extra = {}) {
  return { ...model.createEmptyDocument(), tiles, ...extra }
}
const cameraTile = (id, camera, x = 0) => model.defaultTile('camera', id, { x, y: 0, w: 12, h: 12 }, camera)
function spotlight(id, cameras, overrides = {}) {
  return { ...model.defaultTile('spotlight', id, { x: 0, y: 12, w: 24, h: 24 }), sources: { kind: 'list', cameras }, ...overrides }
}
const resolve = (d, active, runtime = {}) => model.resolveDashboard(d, [1, 2, 3, 4, 5, 6], { active: new Set(active), ...runtime })

test('spotlight shows active sources in priority order and splits up to maxSplit', () => {
  const d = doc([spotlight('s', [3, 2, 1, 4, 5])])
  assert.deepEqual(resolve(d, [1, 2]).spotlights.get('s'), [2, 1])
  assert.deepEqual(resolve(d, [5, 4, 3, 2, 1]).spotlights.get('s'), [3, 2, 1, 4])
  d.tiles[0].maxSplit = 1
  assert.deepEqual(resolve(d, [1, 2]).spotlights.get('s'), [2])
})

test('fixed cameras are never stolen unless the spotlight may borrow them', () => {
  const d = doc([cameraTile('a', 1), spotlight('s', [1, 2])])
  assert.deepEqual(resolve(d, [1]).spotlights.get('s'), [2]) // idle "last" falls back to first free source
  d.tiles[1].idle = { kind: 'blank' }
  assert.deepEqual(resolve(d, [1]).spotlights.get('s'), [])
  d.tiles[1].borrowPlaced = true
  const borrowed = resolve(d, [1])
  assert.deepEqual(borrowed.spotlights.get('s'), [1])
  assert.equal(borrowed.borrowed.get('a'), 's')
})

test('multiple spotlights never show the same camera twice', () => {
  const d = doc([spotlight('s1', [1, 2, 3], { maxSplit: 2 }), { ...spotlight('s2', [1, 2, 3], { maxSplit: 2 }), x: 24 }])
  const r = resolve(d, [1, 2, 3])
  assert.deepEqual(r.spotlights.get('s1'), [1, 2])
  assert.deepEqual(r.spotlights.get('s2'), [3])
})

test('idle rules: camera, rotate, last and blank', () => {
  const d = doc([spotlight('s', [1, 2, 3], { idle: { kind: 'camera', camera: 3 } })])
  assert.deepEqual(resolve(d, []).spotlights.get('s'), [3])
  d.tiles[0].idle = { kind: 'rotate', everySeconds: 20 }
  assert.deepEqual(resolve(d, [], { rotation: new Map([['s', 4]]) }).spotlights.get('s'), [2])
  d.tiles[0].idle = { kind: 'last' }
  assert.deepEqual(resolve(d, [], { lastShown: new Map([['s', [3]]]) }).spotlights.get('s'), [3])
  d.tiles[0].idle = { kind: 'blank' }
  assert.deepEqual(resolve(d, []).spotlights.get('s'), [])
  d.tiles[0].trigger = 'rotate'
  assert.deepEqual(resolve(d, [1], { rotation: new Map([['s', 1]]) }).spotlights.get('s'), [2], 'rotation ignores motion')
})

test('manual pin overrides motion; all-cameras sources honour dashboard priority', () => {
  const d = doc([{ ...spotlight('s', []), sources: { kind: 'all', cameras: [] } }], { behavior: { priority: [4, 2], returnToLiveSeconds: 180, quietControls: true } })
  assert.deepEqual(resolve(d, [1, 2, 4]).spotlights.get('s'), [4, 2, 1])
  assert.deepEqual(resolve(d, [1], { pinned: new Map([['s', 6]]) }).spotlights.get('s'), [6])
})

test('mounted set is stable: warm sources stay mounted as placement changes', () => {
  const d = doc([cameraTile('a', 1), spotlight('s', [2, 3, 4])])
  assert.deepEqual(resolve(d, []).mounted, [1, 2, 3, 4])
  assert.deepEqual(resolve(d, [3]).mounted, [1, 2, 3, 4])
  d.tiles[1].warm = false
  d.tiles[1].idle = { kind: 'blank' }
  assert.deepEqual(resolve(d, []).mounted, [1])
  assert.deepEqual(resolve(d, [3]).mounted, [1, 3])
})

test('unavailable cameras are ignored everywhere', () => {
  const d = doc([cameraTile('a', 9), spotlight('s', [9, 2])])
  const r = resolve(d, [9, 2])
  assert.deepEqual(r.spotlights.get('s'), [2])
  assert.ok(!r.mounted.includes(9))
})

test('split cells follow tile aspect', () => {
  assert.equal(model.splitCells(1, 1).length, 1)
  assert.deepEqual(model.splitCells(2, 2)[1], { x: 0.5, y: 0, w: 0.5, h: 1 })
  assert.deepEqual(model.splitCells(2, 0.5)[1], { x: 0, y: 0.5, w: 1, h: 0.5 })
  assert.equal(model.splitCells(4, 1).length, 4)
  for (const n of [1, 2, 3, 4]) for (const aspect of [0.4, 1, 3]) {
    const area = model.splitCells(n, aspect).reduce((s, c) => s + c.w * c.h, 0)
    assert.ok(Math.abs(area - 1) < 1e-9, `cells cover the tile for ${n}@${aspect}`)
  }
})

test('motion holds release after their own hold, unextended by other cameras', () => {
  const holds = new model.ActiveHolds()
  const hold = () => 15
  assert.deepEqual([...holds.update(new Set([1]), 0, hold)], [1])
  assert.deepEqual([...holds.update(new Set(), 1000, hold)], [1])
  assert.deepEqual([...holds.update(new Set([2]), 10000, hold)].sort(), [1, 2])
  assert.deepEqual([...holds.update(new Set([2]), 16001, hold)], [2], 'camera 1 released despite camera 2 motion')
  assert.equal(holds.nextRelease(), null)
  holds.update(new Set(), 17000, hold)
  assert.equal(holds.nextRelease(), 32000)
})

test('reading order honours mobileOrder then position', () => {
  const d = doc([{ ...cameraTile('b', 2, 24) }, cameraTile('a', 1), spotlight('s', [3, 1])], { mobileOrder: [3] })
  assert.deepEqual(model.readingOrderCameras(d, [1, 2, 3]), [3, 1, 2])
})

test('normalisation fills defaults and drops unknown tile types', () => {
  const d = model.normaliseDocument({ tiles: [{ id: 'x', type: 'iframe', x: 0, y: 0, w: 4, h: 4 },
    { id: 's', type: 'spotlight', x: 0, y: 0, w: 8, h: 8, maxSplit: 99, idle: { kind: 'camera', camera: 3 } }] })
  assert.equal(d.tiles.length, 1)
  assert.equal(d.tiles[0].maxSplit, 4)
  assert.deepEqual(d.tiles[0].idle, { kind: 'camera', camera: 3 })
  assert.equal(d.grid.cols, 48)
})
