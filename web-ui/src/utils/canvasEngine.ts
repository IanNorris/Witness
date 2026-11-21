// Pure grid geometry for the dashboard editor. Every operation returns either
// a valid arrangement (in bounds, no overlaps) or null; there is no
// "invalid, try again" intermediate state. Import-free for node:test.

export interface Rect { x: number; y: number; w: number; h: number }
export interface Placed extends Rect { id: string }
export interface MinSize { w: number; h: number }
export type Edge = 'left' | 'right' | 'top' | 'bottom'
export type Side = Edge

export function overlaps(a: Rect, b: Rect): boolean {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h
}

export function inBounds(r: Rect, cols: number, rows: number): boolean {
  return r.x >= 0 && r.y >= 0 && r.w >= 1 && r.h >= 1 && r.x + r.w <= cols && r.y + r.h <= rows
}

export function isValidLayout(tiles: readonly Placed[], cols: number, rows: number): boolean {
  return tiles.every((t, i) => inBounds(t, cols, rows) && tiles.slice(i + 1).every(o => !overlaps(t, o)))
}

export function fits(tiles: readonly Placed[], rect: Rect, cols: number, rows: number, ignore: ReadonlySet<string> = new Set()): boolean {
  return inBounds(rect, cols, rows) && tiles.every(t => ignore.has(t.id) || !overlaps(t, rect))
}

export function snap(value: number, step: number): number {
  return step <= 1 ? Math.round(value) : Math.round(value / step) * step
}

function occupancy(tiles: readonly Placed[], cols: number, rows: number, ignore?: ReadonlySet<string>) {
  const grid = new Uint8Array(cols * rows)
  for (const t of tiles) {
    if (ignore?.has(t.id)) continue
    for (let y = Math.max(0, t.y); y < Math.min(rows, t.y + t.h); y++)
      for (let x = Math.max(0, t.x); x < Math.min(cols, t.x + t.w); x++) grid[y * cols + x] = 1
  }
  return grid
}

// The largest empty rectangle containing cell (cx, cy), or null when the cell
// is occupied. Ties prefer the squarer rectangle (closer to a camera shape).
export function largestEmptyAt(tiles: readonly Placed[], cols: number, rows: number, cx: number, cy: number,
  ignore?: ReadonlySet<string>): Rect | null {
  cx = Math.floor(cx); cy = Math.floor(cy)
  if (cx < 0 || cy < 0 || cx >= cols || cy >= rows) return null
  const grid = occupancy(tiles, cols, rows, ignore)
  if (grid[cy * cols + cx]) return null
  let best: Rect | null = null
  let bestScore = -1
  for (let top = cy; top >= 0 && !grid[top * cols + cx]; top--) {
    for (let bottom = cy; bottom < rows && !grid[bottom * cols + cx]; bottom++) {
      const free = (x: number) => { for (let y = top; y <= bottom; y++) if (grid[y * cols + x]) return false; return true }
      let left = cx, right = cx
      while (left > 0 && free(left - 1)) left--
      while (right < cols - 1 && free(right + 1)) right++
      const w = right - left + 1, h = bottom - top + 1
      const score = w * h * 1000 - Math.abs(w - h)
      if (score > bestScore) { bestScore = score; best = { x: left, y: top, w, h } }
    }
  }
  return best
}

// Slot sizes to offer when dropping into empty space: sizes already used on
// the canvas (most common first), then quarter/sixteenth-style defaults.
export function preferredSlotSizes(tiles: readonly Rect[], cols: number, rows: number): MinSize[] {
  const counts = new Map<string, { size: MinSize; n: number }>()
  for (const t of tiles) {
    const key = `${t.w}x${t.h}`
    const entry = counts.get(key) ?? { size: { w: t.w, h: t.h }, n: 0 }
    entry.n++
    counts.set(key, entry)
  }
  const used = [...counts.values()].sort((a, b) => b.n - a.n || b.size.w * b.size.h - a.size.w * a.size.h).map(e => e.size)
  const defaults = [2, 3, 4].map(d => ({ w: Math.round(cols / d), h: Math.round(rows / d) }))
  const result: MinSize[] = []
  for (const size of [...used, ...defaults]) if (!result.some(s => s.w === size.w && s.h === size.h)) result.push(size)
  return result
}

// A slot inside an empty region under the pointer, aligned to that region in
// multiples of the slot size, so drops line up with a grid of equal tiles.
// The first preferred size that fits wins; tries are bounded by `min`.
export function slotAt(region: Rect, gx: number, gy: number, preferred: readonly MinSize[], min: MinSize): Rect {
  for (const size of preferred) {
    if (size.w < min.w || size.h < min.h || size.w > region.w || size.h > region.h) continue
    const column = Math.max(0, Math.floor((gx - region.x) / size.w))
    const row = Math.max(0, Math.floor((gy - region.y) / size.h))
    const x = Math.min(region.x + column * size.w, region.x + region.w - size.w)
    const y = Math.min(region.y + row * size.h, region.y + region.h - size.h)
    return { x, y, w: size.w, h: size.h }
  }
  return region
}

export function firstEmptySpot(tiles: readonly Placed[], cols: number, rows: number, size: MinSize): Rect | null {
  for (let y = 0; y + size.h <= rows; y++)
    for (let x = 0; x + size.w <= cols; x++) {
      const rect = { x, y, w: size.w, h: size.h }
      if (fits(tiles, rect, cols, rows)) return rect
    }
  return null
}

export function moveTile(tiles: readonly Placed[], id: string, x: number, y: number, cols: number, rows: number): Placed[] | null {
  const tile = tiles.find(t => t.id === id)
  if (!tile) return null
  const next = { ...tile, x: Math.max(0, Math.min(cols - tile.w, x)), y: Math.max(0, Math.min(rows - tile.h, y)) }
  if (!fits(tiles, next, cols, rows, new Set([id]))) return null
  return tiles.map(t => (t.id === id ? next : t))
}

export function swapTiles(tiles: readonly Placed[], a: string, b: string): Placed[] | null {
  const ta = tiles.find(t => t.id === a), tb = tiles.find(t => t.id === b)
  if (!ta || !tb || a === b) return null
  return tiles.map(t => t.id === a ? { ...t, x: tb.x, y: tb.y, w: tb.w, h: tb.h }
    : t.id === b ? { ...t, x: ta.x, y: ta.y, w: ta.w, h: ta.h } : t)
}

// Halves `targetId` and gives the `side` half to a new tile. Returns the
// updated tiles plus the rectangle for the incoming tile.
export function splitTile(tiles: readonly Placed[], targetId: string, side: Side, min: MinSize, incomingMin: MinSize = min):
  { tiles: Placed[]; rect: Rect } | null {
  const target = tiles.find(t => t.id === targetId)
  if (!target) return null
  const horizontal = side === 'left' || side === 'right'
  const extent = horizontal ? target.w : target.h
  const half = Math.floor(extent / 2)
  const keep = extent - half
  if (half < (horizontal ? incomingMin.w : incomingMin.h) || keep < (horizontal ? min.w : min.h)) return null
  let kept: Placed, rect: Rect
  if (side === 'left') { rect = { x: target.x, y: target.y, w: half, h: target.h }; kept = { ...target, x: target.x + half, w: keep } }
  else if (side === 'right') { rect = { x: target.x + keep, y: target.y, w: half, h: target.h }; kept = { ...target, w: keep } }
  else if (side === 'top') { rect = { x: target.x, y: target.y, w: target.w, h: half }; kept = { ...target, y: target.y + half, h: keep } }
  else { rect = { x: target.x, y: target.y + keep, w: target.w, h: half }; kept = { ...target, h: keep } }
  return { tiles: tiles.map(t => (t.id === targetId ? kept : t)), rect }
}

function applyEdges(t: Placed, edges: ReadonlySet<Edge>, dx: number, dy: number): Placed {
  let { x, y, w, h } = t
  if (edges.has('left')) { x += dx; w -= dx }
  if (edges.has('right')) w += dx
  if (edges.has('top')) { y += dy; h -= dy }
  if (edges.has('bottom')) h += dy
  return { ...t, x, y, w, h }
}

// Tiles sharing the moved edge line, found transitively along that line.
function linkedEdgeTiles(tiles: readonly Placed[], tile: Placed, edge: Edge): Map<string, Edge> {
  const vertical = edge === 'left' || edge === 'right'
  const line = edge === 'left' ? tile.x : edge === 'right' ? tile.x + tile.w : edge === 'top' ? tile.y : tile.y + tile.h
  const result = new Map<string, Edge>([[tile.id, edge]])
  const span = (t: Placed): [number, number] => vertical ? [t.y, t.y + t.h] : [t.x, t.x + t.w]
  let [lo, hi] = span(tile)
  let grew = true
  while (grew) {
    grew = false
    for (const t of tiles) {
      if (result.has(t.id)) continue
      const [a, b] = span(t)
      if (b < lo || a > hi) continue // touching segments continue the same divider
      const near = vertical ? t.x : t.y, far = vertical ? t.x + t.w : t.y + t.h
      if (far === line) result.set(t.id, vertical ? 'right' : 'bottom')
      else if (near === line) result.set(t.id, vertical ? 'left' : 'top')
      else continue
      lo = Math.min(lo, a); hi = Math.max(hi, b); grew = true
    }
  }
  return result
}

// Resizes `id` by moving the given edges. With `linked`, a single edge drag
// also moves every tile abutting that edge line, like a split-pane divider.
// The delta is reduced toward zero until the result is valid.
export function resizeTile(tiles: readonly Placed[], id: string, edges: readonly Edge[], dx: number, dy: number,
  cols: number, rows: number, minFor: (id: string) => MinSize, linked: boolean): Placed[] | null {
  const tile = tiles.find(t => t.id === id)
  if (!tile || edges.length === 0) return null
  const edgeSet = new Set(edges)
  const moves = new Map<string, Set<Edge>>([[id, edgeSet]])
  if (linked && edges.length === 1) {
    for (const [other, edge] of linkedEdgeTiles(tiles, tile, edges[0]!)) moves.set(other, new Set([edge]))
  }
  const attempt = (ddx: number, ddy: number) => {
    const next = tiles.map(t => (moves.has(t.id) ? applyEdges(t, moves.get(t.id)!, ddx, ddy) : t))
    const ok = next.every(t => { const m = minFor(t.id); return t.w >= m.w && t.h >= m.h }) && isValidLayout(next, cols, rows)
    return ok ? next : null
  }
  const steps = Math.max(Math.abs(dx), Math.abs(dy))
  for (let i = steps; i >= 1; i--) {
    const result = attempt(Math.round(dx * i / steps), Math.round(dy * i / steps))
    if (result) return result
  }
  return steps === 0 ? [...tiles] : null
}

// Grows tiles into adjacent empty space, one cell at a time, until stable.
export function fillGaps(tiles: readonly Placed[], cols: number, rows: number): Placed[] {
  let current = tiles.map(t => ({ ...t }))
  let changed = true
  while (changed) {
    changed = false
    for (let i = 0; i < current.length; i++) {
      for (const grow of [
        (t: Placed) => ({ ...t, w: t.w + 1 }), (t: Placed) => ({ ...t, h: t.h + 1 }),
        (t: Placed) => ({ ...t, x: t.x - 1, w: t.w + 1 }), (t: Placed) => ({ ...t, y: t.y - 1, h: t.h + 1 }),
      ]) {
        const candidate = grow(current[i]!)
        if (fits(current, candidate, cols, rows, new Set([candidate.id]))) { current[i] = candidate; changed = true }
      }
    }
  }
  return current
}

// Equal grid for n items, bounded and non-overlapping. On the default 48x48
// grid over a 16:9 target every cell is already 16:9, so a square-ish
// column count keeps tiles camera-shaped. A short last row is stretched.
export function equalGrid(count: number, cols: number, rows: number): Rect[] {
  if (count <= 0) return []
  const columns = Math.min(count, Math.ceil(Math.sqrt(count)))
  const lines = Math.ceil(count / columns)
  const result: Rect[] = []
  for (let i = 0; i < count; i++) {
    const r = Math.floor(i / columns), c = i % columns
    const inRow = r === lines - 1 ? count - r * columns : columns
    const x0 = Math.round(c * cols / inRow), x1 = Math.round((c + 1) * cols / inRow)
    const y0 = Math.round(r * rows / lines), y1 = Math.round((r + 1) * rows / lines)
    result.push({ x: x0, y: y0, w: x1 - x0, h: y1 - y0 })
  }
  return result
}

export type DropTarget =
  | { kind: 'place'; rect: Rect }
  | { kind: 'swap'; tileId: string; rect: Rect }
  | { kind: 'split'; tileId: string; side: Side; rect: Rect }
  | { kind: 'none' }

// Interprets a pointer position (fractional grid units) during a drag.
// Over a tile's outer 25% edge zone: split; over its centre: swap/replace;
// over empty space: place into the largest empty rectangle there.
// With `slots`, empty-space drops snap to a slot of one of those sizes
// (see slotAt); without, they fill the whole empty rectangle.
export function findDropTarget(tiles: readonly Placed[], gx: number, gy: number, cols: number, rows: number,
  dragged: string | null, minFor: (id: string) => MinSize, incomingMin: MinSize, slots?: readonly MinSize[]): DropTarget {
  if (gx < 0 || gy < 0 || gx >= cols || gy >= rows) return { kind: 'none' }
  const over = tiles.find(t => t.id !== dragged && gx >= t.x && gx < t.x + t.w && gy >= t.y && gy < t.y + t.h)
  if (over) {
    const fx = (gx - over.x) / over.w, fy = (gy - over.y) / over.h
    const edges: [Side, number][] = [['left', fx], ['right', 1 - fx], ['top', fy], ['bottom', 1 - fy]]
    const [side, distance] = edges.reduce((a, b) => (b[1] < a[1] ? b : a))
    if (distance < 0.25) {
      const others = dragged ? tiles.filter(t => t.id !== dragged) : tiles
      const split = splitTile(others, over.id, side, minFor(over.id), incomingMin)
      if (split) return { kind: 'split', tileId: over.id, side, rect: split.rect }
    }
    return { kind: 'swap', tileId: over.id, rect: { x: over.x, y: over.y, w: over.w, h: over.h } }
  }
  const ignore = dragged ? new Set([dragged]) : undefined
  const rect = largestEmptyAt(tiles, cols, rows, gx, gy, ignore)
  if (!rect || rect.w < incomingMin.w || rect.h < incomingMin.h) return { kind: 'none' }
  return { kind: 'place', rect: slots ? slotAt(rect, gx, gy, slots, incomingMin) : rect }
}

// Proportional rescale onto a new grid (e.g. 12x12 -> 48x48), keeping shared
// edges shared by rounding edge coordinates rather than sizes.
export function rescale(tiles: readonly Placed[], fromCols: number, fromRows: number, toCols: number, toRows: number): Placed[] {
  return tiles.map(t => {
    const x0 = Math.round(t.x * toCols / fromCols), x1 = Math.round((t.x + t.w) * toCols / fromCols)
    const y0 = Math.round(t.y * toRows / fromRows), y1 = Math.round((t.y + t.h) * toRows / fromRows)
    return { ...t, x: x0, y: y0, w: Math.max(1, x1 - x0), h: Math.max(1, y1 - y0) }
  })
}

// Snap guides: other tiles' edges and centres within `tolerance` of `value`.
export function snapToLines(value: number, lines: readonly number[], tolerance: number): { value: number; line: number | null } {
  let best: number | null = null
  for (const line of lines) if (Math.abs(line - value) <= tolerance && (best === null || Math.abs(line - value) < Math.abs(best - value))) best = line
  return best === null ? { value, line: null } : { value: best, line: best }
}
