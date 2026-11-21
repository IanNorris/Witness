import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

async function load(path) {
  const code = ts.transpileModule(await readFile(path, 'utf8'), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
  return import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'))
}
const engine = await load('src/utils/canvasEngine.ts')
const templates = await load('src/utils/dashboardTemplates.ts')
const min = () => ({ w: 4, h: 4 })

test('largest empty rectangle fills the free region under the pointer', () => {
  const tiles = [{ id: 'a', x: 0, y: 0, w: 24, h: 48 }]
  assert.deepEqual(engine.largestEmptyAt(tiles, 48, 48, 30, 10), { x: 24, y: 0, w: 24, h: 48 })
  assert.equal(engine.largestEmptyAt(tiles, 48, 48, 5, 5), null)
  assert.deepEqual(engine.largestEmptyAt([], 12, 12, 3, 3), { x: 0, y: 0, w: 12, h: 12 })
})

test('drop targets: centre swaps, outer quarter splits, empty space places', () => {
  const tiles = [{ id: 'a', x: 0, y: 0, w: 24, h: 24 }, { id: 'b', x: 24, y: 0, w: 24, h: 24 }]
  assert.equal(engine.findDropTarget(tiles, 12, 12, 48, 48, null, min, min()).kind, 'swap')
  const split = engine.findDropTarget(tiles, 1, 12, 48, 48, null, min, min())
  assert.equal(split.kind, 'split'); assert.equal(split.side, 'left'); assert.deepEqual(split.rect, { x: 0, y: 0, w: 12, h: 24 })
  const place = engine.findDropTarget(tiles, 10, 40, 48, 48, null, min, min())
  assert.deepEqual(place, { kind: 'place', rect: { x: 0, y: 24, w: 48, h: 24 } })
  // A tile too small to halve falls back to swap.
  const small = [{ id: 's', x: 0, y: 0, w: 6, h: 6 }]
  assert.equal(engine.findDropTarget(small, 0.2, 3, 48, 48, null, min, min()).kind, 'swap')
})

test('split halves the target and never overlaps', () => {
  const tiles = [{ id: 'a', x: 0, y: 0, w: 24, h: 24 }]
  const result = engine.splitTile(tiles, 'a', 'bottom', min())
  assert.deepEqual(result.tiles[0], { id: 'a', x: 0, y: 0, w: 24, h: 12 })
  assert.deepEqual(result.rect, { x: 0, y: 12, w: 24, h: 12 })
})

test('linked resize moves a shared divider for every abutting tile', () => {
  const tiles = [
    { id: 'a', x: 0, y: 0, w: 24, h: 24 }, { id: 'b', x: 24, y: 0, w: 24, h: 24 },
    { id: 'c', x: 0, y: 24, w: 24, h: 24 }, { id: 'd', x: 24, y: 24, w: 24, h: 24 },
  ]
  const linked = engine.resizeTile(tiles, 'a', ['right'], 8, 0, 48, 48, min, true)
  assert.deepEqual(linked.map(t => [t.id, t.x, t.w]), [['a', 0, 32], ['b', 32, 16], ['c', 0, 32], ['d', 32, 16]])
  // Unlinked resize into a neighbour is reduced until valid (here: impossible).
  assert.equal(engine.resizeTile(tiles, 'a', ['right'], 8, 0, 48, 48, min, false), null)
  // Minimum sizes cap linked resizes rather than failing them.
  const capped = engine.resizeTile(tiles, 'a', ['right'], 40, 0, 48, 48, min, true)
  assert.equal(capped.find(t => t.id === 'b').w, 4)
})

test('swap exchanges rectangles; move rejects collisions', () => {
  const tiles = [{ id: 'a', x: 0, y: 0, w: 8, h: 8 }, { id: 'b', x: 8, y: 0, w: 16, h: 16 }]
  const swapped = engine.swapTiles(tiles, 'a', 'b')
  assert.deepEqual(swapped[0], { id: 'a', x: 8, y: 0, w: 16, h: 16 })
  assert.equal(engine.moveTile(tiles, 'a', 4, 0, 48, 48), null)
  assert.deepEqual(engine.moveTile(tiles, 'a', 0, 20, 48, 48)[0], { id: 'a', x: 0, y: 20, w: 8, h: 8 })
})

test('fill gaps grows tiles into empty space', () => {
  const filled = engine.fillGaps([{ id: 'a', x: 2, y: 2, w: 4, h: 4 }, { id: 'b', x: 30, y: 30, w: 4, h: 4 }], 48, 48)
  assert.ok(engine.isValidLayout(filled, 48, 48))
  const area = filled.reduce((sum, t) => sum + t.w * t.h, 0)
  assert.equal(area, 48 * 48)
})

test('randomised operation sequences stay bounded and non-overlapping', () => {
  let seed = 7
  const rand = n => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed % n }
  let tiles = engine.equalGrid(6, 48, 48).map((r, i) => ({ id: `t${i}`, ...r }))
  assert.ok(engine.isValidLayout(tiles, 48, 48))
  for (let i = 0; i < 2000; i++) {
    const target = tiles[rand(tiles.length)].id
    const op = rand(5)
    let next = null
    if (op === 0) next = engine.moveTile(tiles, target, rand(48), rand(48), 48, 48)
    else if (op === 1) next = engine.resizeTile(tiles, target, [['left', 'right', 'top', 'bottom'][rand(4)]], rand(17) - 8, rand(17) - 8, 48, 48, min, rand(2) === 0)
    else if (op === 2) next = engine.swapTiles(tiles, target, tiles[rand(tiles.length)].id)
    else if (op === 3 && tiles.length < 20) {
      const split = engine.splitTile(tiles, target, ['left', 'right', 'top', 'bottom'][rand(4)], min())
      if (split) next = [...split.tiles, { id: `n${i}`, ...split.rect }]
    } else if (op === 4) next = engine.fillGaps(tiles, 48, 48)
    if (next) tiles = next
    assert.ok(engine.isValidLayout(tiles, 48, 48), `invalid after op ${op} at ${i}`)
    assert.ok(tiles.every(t => t.w >= 4 && t.h >= 4), `under minimum after op ${op} at ${i}`)
  }
})

test('equal grids and templates are bounded and non-overlapping', () => {
  for (let n = 1; n <= 20; n++) {
    const rects = engine.equalGrid(n, 48, 48).map((r, i) => ({ id: String(i), ...r }))
    assert.equal(rects.length, n)
    assert.ok(engine.isValidLayout(rects, 48, 48), `equal grid ${n}`)
  }
  for (const template of templates.DASHBOARD_TEMPLATES) {
    const slots = template.slots.map((s, i) => ({ id: String(i), ...s }))
    assert.ok(engine.isValidLayout(slots, 48, 48), template.id)
  }
  const { fills, unplaced } = templates.fillTemplate(templates.DASHBOARD_TEMPLATES.find(t => t.id === 'grid2x2'), [5, 6, 7, 8, 9])
  assert.deepEqual(fills.map(f => f.camera), [5, 6, 7, 8])
  assert.deepEqual(unplaced, [9])
})

test('rescale keeps shared edges shared', () => {
  const tiles = [{ id: 'a', x: 0, y: 0, w: 5, h: 12 }, { id: 'b', x: 5, y: 0, w: 7, h: 12 }]
  const scaled = engine.rescale(tiles, 12, 12, 48, 48)
  assert.equal(scaled[0].x + scaled[0].w, scaled[1].x)
  assert.ok(engine.isValidLayout(scaled, 48, 48))
})

test('empty-space drops snap to grid slots instead of filling the whole gap', () => {
  // A 2 x 2 grid with the right column empty: dropping lands in one 24 x 24 slot.
  const tiles = [{ id: 'a', x: 0, y: 0, w: 24, h: 24 }, { id: 'b', x: 0, y: 24, w: 24, h: 24 }]
  const slots = engine.preferredSlotSizes(tiles, 48, 48)
  assert.deepEqual(slots[0], { w: 24, h: 24 })
  assert.deepEqual(engine.findDropTarget(tiles, 30, 40, 48, 48, null, min, min(), slots), { kind: 'place', rect: { x: 24, y: 24, w: 24, h: 24 } })
  assert.deepEqual(engine.findDropTarget(tiles, 30, 5, 48, 48, null, min, min(), slots), { kind: 'place', rect: { x: 24, y: 0, w: 24, h: 24 } })
  // Without slots (Shift) the whole empty column is used.
  assert.deepEqual(engine.findDropTarget(tiles, 30, 40, 48, 48, null, min, min()).rect, { x: 24, y: 0, w: 24, h: 48 })
  // Smaller gaps fall back to the next size that fits, aligned to the gap.
  const gap = { x: 36, y: 0, w: 12, h: 48 }
  assert.deepEqual(engine.slotAt(gap, 40, 30, [{ w: 24, h: 24 }, { w: 12, h: 12 }], min()), { x: 36, y: 24, w: 12, h: 12 })
  // An empty canvas offers a quarter first.
  assert.deepEqual(engine.preferredSlotSizes([], 48, 48)[0], { w: 24, h: 24 })
})

test('unlinked edge resize changes only the dragged tile', () => {
  const tiles = [{ id: 'a', x: 0, y: 0, w: 16, h: 24 }, { id: 'b', x: 16, y: 0, w: 16, h: 24 }, { id: 'c', x: 0, y: 24, w: 16, h: 24 }]
  const result = engine.resizeTile(tiles, 'b', ['right'], 8, 0, 48, 48, min, false)
  assert.deepEqual(result.map(t => [t.id, t.x, t.w]), [['a', 0, 16], ['b', 16, 24], ['c', 0, 16]])
  const shrink = engine.resizeTile(tiles, 'a', ['right'], -6, 0, 48, 48, min, false)
  assert.deepEqual(shrink.map(t => [t.id, t.x, t.w]), [['a', 0, 10], ['b', 16, 16], ['c', 0, 16]])
})
