// Dashboards v2 document model and spotlight resolver (see DASHBOARDS_V2.md).
// Import-free so node:test can transpile it directly.

export interface Rect { x: number; y: number; w: number; h: number }

export type StreamPreference = 'auto' | 'main' | 'sub'
export type ScopeKind = 'dashboard' | 'all' | 'list'

interface TileBase extends Rect { id: string }

export interface CameraTile extends TileBase {
  type: 'camera'
  camera: number
  stream: StreamPreference
  fit: 'contain' | 'cover'
  label: boolean
  playback: boolean
}

export type IdleRule =
  | { kind: 'blank' }
  | { kind: 'last' }
  | { kind: 'camera'; camera: number }
  | { kind: 'rotate'; everySeconds: number }

export interface SpotlightTile extends TileBase {
  type: 'spotlight'
  trigger: 'motion' | 'rotate'
  sources: { kind: 'list' | 'all'; cameras: number[] }
  borrowPlaced: boolean
  holdSeconds: number
  idle: IdleRule
  warm: boolean
  // Cells shown: at least minSplit (topped up with the highest-priority
  // cameras when fewer have motion), at most maxSplit. 1..4, min <= max.
  minSplit: number
  maxSplit: number
  label: boolean
}

export interface ScopedTileBase extends TileBase { scope: { kind: ScopeKind; cameras?: number[] } }
export interface ActivityTile extends ScopedTileBase {
  type: 'activity'
  direction: 'auto' | 'horizontal' | 'vertical'
  grouping: boolean
  objectFocus: boolean
}
export interface TimelineTile extends ScopedTileBase { type: 'timeline'; hours: number }
export interface DetectionTile extends ScopedTileBase { type: 'detection' }
export interface ClockTile extends TileBase { type: 'clock'; seconds: boolean; date: boolean }
export interface StatusTile extends TileBase { type: 'status'; variant: 'summary' | 'health' }
export interface PtzTile extends TileBase { type: 'ptz'; camera: number; label: boolean }
export interface TextTile extends TileBase { type: 'text'; text: string; size: 's' | 'm' | 'l' }
export interface UnavailableTile extends TileBase { type: 'unavailable'; reason: string; redacted: true }

export type DashboardTile = CameraTile | SpotlightTile | ActivityTile | TimelineTile | DetectionTile
  | ClockTile | StatusTile | PtzTile | TextTile | UnavailableTile
export type TileType = DashboardTile['type']

export interface DashboardBadge {
  id: string
  type: 'clock'
  anchor: 'top-left' | 'top-right' | 'bottom-left' | 'bottom-right'
  size: 's' | 'm' | 'l'
}

export interface DashboardDocument {
  schema: 2
  kind: 'canvas'
  grid: { cols: number; rows: number; gap: number; radius: number }
  target: { width: number; height: number }
  tiles: DashboardTile[]
  badges: DashboardBadge[]
  behavior: {
    priority: number[]
    returnToLiveSeconds: number
    quietControls: boolean
    // Full-quality (main) streams: 0 = automatic decode budget, -1 = no limit
    // (tile-size adaptation only), 1..16 = at most that many.
    mainStreams: number
  }
  mobileOrder: number[]
}

export interface DashboardSummary {
  id: number
  name: string
  scope: 'personal' | 'shared'
  kind: 'canvas'
  revision: number
  updatedAt: number
  owned: boolean
  canEdit: boolean
  tiles?: number
  redacted?: number
  cameras?: number[]
}

export interface DashboardRecord extends DashboardSummary {
  document: DashboardDocument
  shares?: { type: 'everyone' | 'user' | 'group'; id: number }[]
}

export const TILE_LIMIT = 64
export const MIN_SIZE: Record<TileType, { w: number; h: number }> = {
  camera: { w: 4, h: 4 }, spotlight: { w: 4, h: 4 }, activity: { w: 4, h: 4 }, timeline: { w: 8, h: 4 },
  detection: { w: 4, h: 4 }, clock: { w: 6, h: 3 }, status: { w: 6, h: 4 }, ptz: { w: 6, h: 6 },
  text: { w: 4, h: 2 }, unavailable: { w: 1, h: 1 },
}

export function createEmptyDocument(): DashboardDocument {
  return {
    schema: 2, kind: 'canvas',
    grid: { cols: 48, rows: 48, gap: 4, radius: 8 },
    target: { width: 1920, height: 1080 },
    tiles: [], badges: [],
    behavior: { priority: [], returnToLiveSeconds: 180, quietControls: true, mainStreams: 0 },
    mobileOrder: [],
  }
}

const clampInt = (value: unknown, lo: number, hi: number, fallback: number) => {
  const n = Number(value)
  return Number.isFinite(n) ? Math.max(lo, Math.min(hi, Math.round(n))) : fallback
}
const ids = (value: unknown) => Array.isArray(value)
  ? [...new Set(value.map(Number).filter(n => Number.isInteger(n) && n > 0))] : []

export function defaultTile(type: TileType, id: string, rect: Rect, camera = 0): DashboardTile {
  const base = { id, ...rect }
  switch (type) {
    case 'camera': return { ...base, type, camera, stream: 'auto', fit: 'contain', label: true, playback: true }
    case 'spotlight': return { ...base, type, trigger: 'motion', sources: { kind: 'list', cameras: [] }, borrowPlaced: false,
      holdSeconds: 15, idle: { kind: 'last' }, warm: true, minSplit: 1, maxSplit: 4, label: true }
    case 'activity': return { ...base, type, scope: { kind: 'dashboard' }, direction: 'auto', grouping: true, objectFocus: true }
    case 'timeline': return { ...base, type, scope: { kind: 'dashboard' }, hours: 6 }
    case 'detection': return { ...base, type, scope: { kind: 'dashboard' } }
    case 'clock': return { ...base, type, seconds: true, date: true }
    case 'status': return { ...base, type, variant: 'summary' }
    case 'ptz': return { ...base, type, camera, label: true }
    case 'text': return { ...base, type, text: 'Label', size: 'm' }
    case 'unavailable': return { ...base, type, reason: 'no-access', redacted: true }
  }
}

// Fills defaults for anything the server omitted; never throws.
export function normaliseDocument(raw: unknown): DashboardDocument {
  const doc = createEmptyDocument()
  if (!raw || typeof raw !== 'object') return doc
  const r = raw as Record<string, any>
  doc.grid = {
    cols: clampInt(r.grid?.cols, 4, 96, 48), rows: clampInt(r.grid?.rows, 4, 96, 48),
    gap: clampInt(r.grid?.gap, 0, 16, 4), radius: clampInt(r.grid?.radius, 0, 24, 8),
  }
  doc.target = { width: clampInt(r.target?.width, 320, 7680, 1920), height: clampInt(r.target?.height, 320, 4320, 1080) }
  doc.behavior = {
    priority: ids(r.behavior?.priority),
    returnToLiveSeconds: clampInt(r.behavior?.returnToLiveSeconds, 0, 3600, 180),
    quietControls: r.behavior?.quietControls !== false,
    mainStreams: clampInt(r.behavior?.mainStreams, -1, 16, 0),
  }
  doc.mobileOrder = ids(r.mobileOrder)
  doc.badges = Array.isArray(r.badges) ? r.badges.filter((b: any) => b && b.type === 'clock').map((b: any) => ({
    id: String(b.id), type: 'clock' as const,
    anchor: ['top-left', 'top-right', 'bottom-left', 'bottom-right'].includes(b.anchor) ? b.anchor : 'bottom-right',
    size: ['s', 'm', 'l'].includes(b.size) ? b.size : 'm',
  })) : []
  const known = new Set<string>(Object.keys(MIN_SIZE))
  doc.tiles = Array.isArray(r.tiles) ? r.tiles.filter((t: any) => t && known.has(t.type)).map((t: any) => {
    const rect = { x: clampInt(t.x, 0, 96, 0), y: clampInt(t.y, 0, 96, 0), w: clampInt(t.w, 1, 96, 4), h: clampInt(t.h, 1, 96, 4) }
    const tile = defaultTile(t.type, String(t.id), rect, Number(t.camera) || 0) as any
    for (const key of Object.keys(tile)) if (t[key] !== undefined && !['id', 'type', 'x', 'y', 'w', 'h'].includes(key)) tile[key] = t[key]
    if (tile.type === 'spotlight') {
      tile.sources = { kind: t.sources?.kind === 'all' ? 'all' : 'list', cameras: ids(t.sources?.cameras) }
      tile.maxSplit = clampInt(t.maxSplit, 1, 4, 4)
      tile.minSplit = Math.min(tile.maxSplit, clampInt(t.minSplit, 1, 4, 1))
      tile.holdSeconds = clampInt(t.holdSeconds, 3, 120, 15)
      const idle = t.idle ?? {}
      tile.idle = idle.kind === 'camera' && Number(idle.camera) > 0 ? { kind: 'camera', camera: Number(idle.camera) }
        : idle.kind === 'rotate' ? { kind: 'rotate', everySeconds: clampInt(idle.everySeconds, 5, 600, 20) }
          : idle.kind === 'blank' ? { kind: 'blank' } : { kind: 'last' }
    }
    if ('scope' in tile) tile.scope = { kind: ['all', 'list'].includes(t.scope?.kind) ? t.scope.kind : 'dashboard', cameras: ids(t.scope?.cameras) }
    return tile as DashboardTile
  }) : []
  return doc
}

export function cloneDocument(doc: DashboardDocument): DashboardDocument {
  return JSON.parse(JSON.stringify(doc)) as DashboardDocument
}

// ----- Camera references -------------------------------------------------

export function placedCameraIds(doc: DashboardDocument): number[] {
  return doc.tiles.filter((t): t is CameraTile => t.type === 'camera').map(t => t.camera)
}

export function spotlightCandidates(tile: SpotlightTile, doc: DashboardDocument, available: number[]): number[] {
  const availableSet = new Set(available)
  if (tile.sources.kind === 'list') return tile.sources.cameras.filter(id => availableSet.has(id))
  const priority = doc.behavior.priority.filter(id => availableSet.has(id))
  return [...priority, ...available.filter(id => !priority.includes(id))]
}

// Cameras the dashboard shows in some form, in reading order (top-to-bottom,
// left-to-right), honouring mobileOrder first. Used by phones and clip search.
export function readingOrderCameras(doc: DashboardDocument, available: number[]): number[] {
  const availableSet = new Set(available)
  const result: number[] = []
  const add = (id: number) => { if (availableSet.has(id) && !result.includes(id)) result.push(id) }
  doc.mobileOrder.forEach(add)
  const ordered = [...doc.tiles].sort((a, b) => a.y - b.y || a.x - b.x)
  for (const tile of ordered) {
    if (tile.type === 'camera' || tile.type === 'ptz') add(tile.camera)
    else if (tile.type === 'spotlight') spotlightCandidates(tile, doc, available).forEach(add)
  }
  return result
}

// ----- Spotlight resolution ----------------------------------------------

export interface SpotlightRuntime {
  // Cameras currently considered active (motion, extended by each hold).
  active: ReadonlySet<number>
  // Rotation step per spotlight tile id.
  rotation?: ReadonlyMap<string, number>
  // Last cameras shown by each spotlight (for idle "last").
  lastShown?: ReadonlyMap<string, number[]>
  // Manual pins: a camera forced into a spotlight regardless of motion.
  pinned?: ReadonlyMap<string, number>
}

export interface ResolvedDashboard {
  // Cameras shown by each spotlight tile, in sub-cell order.
  spotlights: Map<string, number[]>
  // Placed camera tile ids whose camera is currently shown in a spotlight.
  borrowed: Map<string, string>
  // Every camera that must stay mounted (fixed tiles, shown and warm sources).
  mounted: number[]
}

export function resolveDashboard(doc: DashboardDocument, available: number[], runtime: SpotlightRuntime): ResolvedDashboard {
  const availableSet = new Set(available)
  const placed = new Map<number, string>()
  for (const tile of doc.tiles) if (tile.type === 'camera' && availableSet.has(tile.camera)) placed.set(tile.camera, tile.id)
  const taken = new Set<number>()
  const spotlights = new Map<string, number[]>()
  const borrowed = new Map<string, string>()
  const warm = new Set<number>()
  // Home cameras (a spotlight's idle camera) pushed out by motion. They move
  // down to the next spotlight by size, ahead of other active cameras, so a
  // primary camera such as a front PTZ stays visible in a smaller slot.
  const displaced: number[] = []
  const usable = (tile: SpotlightTile, id: number) => availableSet.has(id) && !taken.has(id) && (tile.borrowPlaced || !placed.has(id))

  for (const tile of spotlightsBySize(doc)) {
    const candidates = spotlightCandidates(tile, doc, available)
      .filter(id => tile.borrowPlaced || !placed.has(id))
    if (tile.warm) {
      candidates.forEach(id => warm.add(id))
      if (tile.idle.kind === 'camera' && usable(tile, tile.idle.camera)) warm.add(tile.idle.camera)
    }
    const free = candidates.filter(id => !taken.has(id))
    let shown: number[] = []
    const pin = runtime.pinned?.get(tile.id)
    const waiting = displaced.filter(id => usable(tile, id))
    if (pin !== undefined && availableSet.has(pin) && !taken.has(pin)) shown = [pin]
    else if (tile.trigger === 'motion') {
      const active = free.filter(id => runtime.active.has(id) && !waiting.includes(id))
      shown = [...waiting, ...active].slice(0, tile.maxSplit)
    } else if (waiting.length) shown = waiting.slice(0, tile.maxSplit)
    if (shown.length > 0 && tile.trigger === 'motion' && tile.idle.kind === 'camera') {
      const home = tile.idle.camera
      if (!shown.includes(home) && usable(tile, home) && !displaced.includes(home)) displaced.push(home)
    }
    if (shown.length === 0) {
      const idle: IdleRule = tile.trigger === 'rotate' ? { kind: 'rotate', everySeconds: 0 } : tile.idle
      if (idle.kind === 'camera') {
        if (free.includes(idle.camera) || (availableSet.has(idle.camera) && !taken.has(idle.camera) && (tile.borrowPlaced || !placed.has(idle.camera))))
          shown = [idle.camera]
      } else if (idle.kind === 'rotate' && free.length > 0) {
        const step = runtime.rotation?.get(tile.id) ?? 0
        shown = [free[((step % free.length) + free.length) % free.length]!]
      } else if (idle.kind === 'last') {
        shown = (runtime.lastShown?.get(tile.id) ?? []).filter(id => free.includes(id)).slice(0, 1)
        if (shown.length === 0 && free.length > 0) shown = [free[0]!]
      }
    }
    // Top up to the minimum with the highest-priority free cameras. "Show
    // nothing" when idle keeps an idle spotlight empty.
    const minimum = Math.min(tile.minSplit ?? 1, tile.maxSplit)
    if (shown.length < minimum && !(shown.length === 0 && tile.trigger === 'motion' && tile.idle.kind === 'blank')) {
      for (const id of [...waiting, ...free]) {
        if (shown.length >= minimum) break
        if (!shown.includes(id) && !taken.has(id) && usable(tile, id)) shown.push(id)
      }
    }
    shown.forEach(id => taken.add(id))
    for (let i = displaced.length - 1; i >= 0; i--) if (taken.has(displaced[i]!)) displaced.splice(i, 1)
    for (const id of shown) {
      const home = placed.get(id)
      if (home) borrowed.set(home, tile.id)
    }
    spotlights.set(tile.id, shown)
  }

  const mounted = new Set<number>([...placed.keys()])
  for (const shown of spotlights.values()) shown.forEach(id => mounted.add(id))
  warm.forEach(id => mounted.add(id))
  return { spotlights, borrowed, mounted: [...mounted].sort((a, b) => a - b) }
}

// Spotlights claim cameras largest first (ties: reading order), so the
// biggest spotlight shows the most important active camera and each smaller
// one acts as the fallback for the next.
export function spotlightsBySize(doc: DashboardDocument): SpotlightTile[] {
  return doc.tiles.filter((t): t is SpotlightTile => t.type === 'spotlight')
    .sort((a, b) => b.w * b.h - a.w * a.h || a.y - b.y || a.x - b.x)
}

// ----- Stream budget -------------------------------------------------------

// About two 4K streams (or eight 1080p) decoded at full quality.
export const MAIN_STREAM_PIXEL_BUDGET = 2 * 3840 * 2160

export interface StreamDemand {
  camera: number
  area: number              // on-screen area (any consistent unit)
  preference: StreamPreference
  pixels: number            // main stream width × height
  hasSubStream: boolean
}

// Decides which visible cameras may use their main stream. Pinned tiles
// (main/sub) keep their choice; pinned main streams count toward the budget.
// Remaining "auto" tiles get the main stream largest-first while the budget
// allows; the rest are switched to their sub-stream. "auto" still lets the
// player adapt to tile size within an allowed main stream.
export function allocateStreams(demands: readonly StreamDemand[], mainStreams: number): Map<number, StreamPreference> {
  const result = new Map<number, StreamPreference>(demands.map(d => [d.camera, d.preference]))
  if (mainStreams < 0) return result
  let pixels = 0, count = 0
  for (const d of demands) if (d.preference === 'main') { pixels += d.pixels; count++ }
  const candidates = demands.filter(d => d.preference === 'auto' && d.hasSubStream)
    .sort((a, b) => b.area - a.area || a.camera - b.camera)
  for (const d of candidates) {
    const allowed = mainStreams > 0 ? count < mainStreams : count === 0 || pixels + d.pixels <= MAIN_STREAM_PIXEL_BUDGET
    if (allowed) { pixels += d.pixels; count++ } else result.set(d.camera, 'sub')
  }
  return result
}

// Sub-cells (fractions of the tile) for n cameras sharing one spotlight.
export function splitCells(count: number, aspect: number): Rect[] {
  const full = { x: 0, y: 0, w: 1, h: 1 }
  if (count <= 1) return [full]
  if (count === 2) return aspect >= 1
    ? [{ x: 0, y: 0, w: 0.5, h: 1 }, { x: 0.5, y: 0, w: 0.5, h: 1 }]
    : [{ x: 0, y: 0, w: 1, h: 0.5 }, { x: 0, y: 0.5, w: 1, h: 0.5 }]
  if (count === 3) return aspect >= 2.4
    ? [0, 1, 2].map(i => ({ x: i / 3, y: 0, w: 1 / 3, h: 1 }))
    : aspect <= 0.6
      ? [0, 1, 2].map(i => ({ x: 0, y: i / 3, w: 1, h: 1 / 3 }))
      : [{ x: 0, y: 0, w: 0.5, h: 0.5 }, { x: 0.5, y: 0, w: 0.5, h: 0.5 }, { x: 0, y: 0.5, w: 1, h: 0.5 }]
  return [{ x: 0, y: 0, w: 0.5, h: 0.5 }, { x: 0.5, y: 0, w: 0.5, h: 0.5 },
    { x: 0, y: 0.5, w: 0.5, h: 0.5 }, { x: 0.5, y: 0.5, w: 0.5, h: 0.5 }]
}

// Motion hold bookkeeping, shared by the renderer and its tests. A camera
// stays active for holdSeconds after its motion ends; unrelated motion never
// extends another camera's hold.
export class ActiveHolds {
  private releases = new Map<number, number>()
  private activeNow = new Set<number>()

  update(motion: ReadonlySet<number>, now: number, holdSecondsFor: (id: number) => number): Set<number> {
    for (const id of motion) { this.activeNow.add(id); this.releases.delete(id) }
    for (const id of [...this.activeNow]) {
      if (motion.has(id)) continue
      if (!this.releases.has(id)) this.releases.set(id, now + holdSecondsFor(id) * 1000)
      if (now >= this.releases.get(id)!) { this.activeNow.delete(id); this.releases.delete(id) }
    }
    return new Set(this.activeNow)
  }

  nextRelease(): number | null {
    let next: number | null = null
    for (const at of this.releases.values()) if (next === null || at < next) next = at
    return next
  }

  clear() { this.releases.clear(); this.activeNow.clear() }
}
