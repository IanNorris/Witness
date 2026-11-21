<script setup lang="ts">
import { computed, onMounted, onUnmounted, provide, ref, watch } from 'vue'
import { onBeforeRouteLeave, useRoute, useRouter } from 'vue-router'
import DashboardCanvas from '../components/dashboard/DashboardCanvas.vue'
import ShareDialog from '../components/dashboard/ShareDialog.vue'
import ClockTile from '../components/dashboard/tiles/ClockTile.vue'
import { useDashboardPlayback, dashboardPlaybackKey } from '../composables/useDashboardPlayback'
import { useFittedBox, isPhoneViewport } from '../composables/useViewport'
import { useCameraStore } from '../stores/cameras'
import { useDashboardStore, type SharePrincipal } from '../stores/dashboards'
import {
  MIN_SIZE, TILE_LIMIT, cloneDocument, defaultTile, readingOrderCameras, spotlightsBySize,
  type DashboardDocument, type DashboardRecord, type DashboardTile, type Rect, type SpotlightTile, type TileType,
} from '../types/dashboard'
import {
  findDropTarget, fillGaps, isValidLayout, largestEmptyAt, firstEmptySpot, preferredSlotSizes, slotAt, moveTile, resizeTile, snap, snapToLines,
  splitTile, swapTiles, equalGrid, type DropTarget, type Edge,
} from '../utils/canvasEngine'
import { DASHBOARD_TEMPLATES, fillTemplate, type DashboardTemplate } from '../utils/dashboardTemplates'
import { EditorHistory } from '../utils/editorHistory'

const route = useRoute()
const router = useRouter()
const cameraStore = useCameraStore()
const store = useDashboardStore()

const dashboardId = computed(() => Number(route.params.id))
const record = ref<DashboardRecord | null>(null)
const draft = ref<DashboardDocument | null>(null)
const name = ref('')
const savedState = ref('')
const loadError = ref('')
const mode = ref<'edit' | 'preview'>('edit')
const selection = ref<string[]>([])
const showGrid = ref(true)
const showTemplates = ref(false)
const showShare = ref(false)
const showHistory = ref(false)
const historyEntries = ref<{ revision: number; updatedAt: number; name: string }[]>([])
const conflict = ref<{ revision: number } | null>(null)
const saveMessage = ref('')
const saving = ref(false)
const recoverable = ref<{ name: string; document: DashboardDocument } | null>(null)
const trayNotice = ref('')
const paletteFilter = ref('')
let history = new EditorHistory<{ name: string; document: DashboardDocument }>('null')
// EditorHistory is plain data; this tick makes undo/redo availability reactive.
const historyTick = ref(0)
const canUndo = computed(() => historyTick.value >= 0 && history.canUndo)
const canRedo = computed(() => historyTick.value >= 0 && history.canRedo)

const available = computed(() => cameraStore.cameras.map(camera => camera.id))
const doc = computed(() => draft.value!)
const cols = computed(() => draft.value?.grid.cols ?? 48)
const rows = computed(() => draft.value?.grid.rows ?? 48)
const dirty = computed(() => draft.value !== null && JSON.stringify({ name: name.value, document: draft.value }) !== savedState.value)
const isShared = computed(() => record.value?.scope === 'shared')
const selectedTile = computed(() => selection.value.length === 1 ? draft.value?.tiles.find(t => t.id === selection.value[0]) ?? null : null)
const placed = computed(() => new Set(draft.value?.tiles.flatMap(t => t.type === 'camera' ? [t.camera] : []) ?? []))
const paletteCameras = computed(() => cameraStore.cameras.filter(camera =>
  !paletteFilter.value || camera.name.toLowerCase().includes(paletteFilter.value.toLowerCase())))
const unplacedCameras = computed(() => cameraStore.cameras.filter(camera => !placed.value.has(camera.id) &&
  !draft.value?.tiles.some(t => t.type === 'spotlight' && (t.sources.kind === 'all' || t.sources.cameras.includes(camera.id)))))

// ----- History ---------------------------------------------------------------
function snapshot() { return { name: name.value, document: cloneDocument(draft.value!) } }
function commit(coalesceKey: string | null = null) {
  if (!draft.value) return
  history.push(snapshot(), coalesceKey)
  historyTick.value++
  saveDraftSoon()
}
function applySnapshot(state: { name: string; document: DashboardDocument } | null) {
  historyTick.value++
  if (!state) return
  name.value = state.name
  draft.value = state.document
  selection.value = selection.value.filter(id => state.document.tiles.some(t => t.id === id))
  saveDraftSoon()
}
const undo = () => applySnapshot(history.undo())
const redo = () => applySnapshot(history.redo())

// Draft recovery survives a crash or closed tab; cleared on save.
const draftKey = computed(() => `witness-dashboard-draft-${dashboardId.value}`)
let draftTimer: ReturnType<typeof setTimeout> | null = null
function saveDraftSoon() {
  if (draftTimer) clearTimeout(draftTimer)
  draftTimer = setTimeout(() => {
    try {
      if (dirty.value && record.value) localStorage.setItem(draftKey.value, JSON.stringify({ revision: record.value.revision, ...snapshot() }))
      else localStorage.removeItem(draftKey.value)
    } catch { /* storage unavailable */ }
  }, 400)
}

// ----- Load / save -----------------------------------------------------------
async function load() {
  loadError.value = ''
  const next = await store.get(dashboardId.value)
  if (!next) { loadError.value = 'This dashboard is not available.'; return }
  if (!next.canEdit) { void router.replace(`/d/${next.id}`); return }
  record.value = next
  name.value = next.name
  draft.value = cloneDocument(next.document)
  savedState.value = JSON.stringify({ name: name.value, document: draft.value })
  history = new EditorHistory(savedState.value)
  historyTick.value++
  selection.value = []
  try {
    const raw = localStorage.getItem(draftKey.value)
    const saved = raw ? JSON.parse(raw) : null
    if (saved && saved.revision === next.revision && JSON.stringify({ name: saved.name, document: saved.document }) !== savedState.value)
      recoverable.value = { name: saved.name, document: saved.document }
  } catch { recoverable.value = null }
  if (draft.value.tiles.length === 0) showTemplates.value = true
}
function restoreDraft() {
  if (!recoverable.value) return
  name.value = recoverable.value.name
  draft.value = recoverable.value.document
  recoverable.value = null
  commit()
}
function discardDraft() {
  recoverable.value = null
  try { localStorage.removeItem(draftKey.value) } catch { /* */ }
}

async function save(baseRevision = record.value?.revision ?? 0) {
  if (!record.value || !draft.value || saving.value) return
  saving.value = true
  saveMessage.value = ''
  try {
    const result = await store.save(record.value.id, baseRevision, { name: name.value.trim(), document: draft.value })
    if (result.ok) {
      record.value = { ...record.value, revision: result.revision, name: name.value.trim() }
      savedState.value = JSON.stringify({ name: name.value, document: draft.value })
      conflict.value = null
      saveMessage.value = isShared.value ? 'Published' : 'Saved'
      try { localStorage.removeItem(draftKey.value) } catch { /* */ }
      setTimeout(() => { if (saveMessage.value === 'Published' || saveMessage.value === 'Saved') saveMessage.value = '' }, 2500)
    } else if (result.conflict) {
      conflict.value = { revision: result.revision }
    } else {
      saveMessage.value = result.message
    }
  } finally { saving.value = false }
}
async function overwrite() { if (conflict.value) await save(conflict.value.revision) }
async function reloadAfterConflict() { conflict.value = null; await load() }
async function saveAsCopy() {
  if (!draft.value) return
  const created = await store.create(`${name.value.trim() || 'Dashboard'} (copy)`.slice(0, 80),
    { ...draft.value, tiles: draft.value.tiles.filter(t => t.type !== 'unavailable') })
  conflict.value = null
  savedState.value = JSON.stringify({ name: name.value, document: draft.value }) // leave without prompting
  void router.replace(`/d/${created.id}/edit`)
}
async function openHistory() {
  if (!record.value) return
  historyEntries.value = await store.history(record.value.id)
  showHistory.value = true
}
async function loadRevision(revision: number) {
  if (!record.value) return
  const result = await store.revision(record.value.id, revision)
  if (!result) return
  draft.value = result.document
  showHistory.value = false
  commit()
}
async function deleteDashboard() {
  if (!record.value || !confirm(`Delete “${record.value.name}”? This cannot be undone.`)) return
  await store.remove(record.value.id)
  savedState.value = JSON.stringify({ name: name.value, document: draft.value })
  void router.replace('/dashboards')
}

onBeforeRouteLeave(() => {
  if (dirty.value && !confirm('Leave without saving your changes?')) return false
})
function onBeforeUnload(event: BeforeUnloadEvent) { if (dirty.value) { event.preventDefault(); event.returnValue = '' } }

// ----- Tile helpers ------------------------------------------------------------
function newTileId() {
  const used = new Set(draft.value?.tiles.map(t => t.id))
  for (let i = 1; ; i++) { const id = `t${i.toString(36)}`; if (!used.has(id)) return id }
}
const minFor = (id: string) => {
  const tile = draft.value?.tiles.find(t => t.id === id)
  return tile ? MIN_SIZE[tile.type] : MIN_SIZE.camera
}
function updateTile(id: string, patch: Partial<DashboardTile>, coalesceKey: string | null = null) {
  if (!draft.value) return
  draft.value.tiles = draft.value.tiles.map(t => (t.id === id ? { ...t, ...patch } as DashboardTile : t))
  commit(coalesceKey)
}
function removeSelected() {
  if (!draft.value || selection.value.length === 0) return
  draft.value.tiles = draft.value.tiles.filter(t => !selection.value.includes(t.id))
  selection.value = []
  commit()
}
function setTiles(tiles: DashboardTile[]) { if (draft.value) draft.value.tiles = tiles }
function applyRects(rects: { id: string; x: number; y: number; w: number; h: number }[]) {
  const byId = new Map(rects.map(r => [r.id, r]))
  setTiles(doc.value.tiles.map(t => { const r = byId.get(t.id); return r ? { ...t, x: r.x, y: r.y, w: r.w, h: r.h } : t }))
}
function addTileAt(type: TileType, rect: Rect, camera = 0) {
  if (!draft.value || draft.value.tiles.length >= TILE_LIMIT) { trayNotice.value = 'Dashboards can have up to 64 tiles.'; return }
  const tile = defaultTile(type, newTileId(), rect, camera)
  if (type === 'ptz' && camera === 0) (tile as { camera: number }).camera = cameraStore.cameras.find(c => c.ptzEnabled)?.id ?? 0
  draft.value.tiles = [...draft.value.tiles, tile]
  selection.value = [tile.id]
  commit()
}
function addTileAnywhere(type: TileType, camera = 0) {
  const size = type === 'camera' || type === 'spotlight' ? { w: 12, h: 12 } : { w: Math.max(MIN_SIZE[type].w, 8), h: Math.max(MIN_SIZE[type].h, 6) }
  const spot = firstEmptySpot(doc.value.tiles, cols.value, rows.value, size) ?? firstEmptySpot(doc.value.tiles, cols.value, rows.value, MIN_SIZE[type])
  if (!spot) { trayNotice.value = 'No room left. Make space, or drop onto a tile edge to split it.'; return }
  addTileAt(type, spot, camera)
}

// ----- Stage gestures ------------------------------------------------------------
const stageRef = ref<HTMLElement | null>(null)
const frameRef = ref<HTMLElement | null>(null)
const { box, measure } = useFittedBox(() => stageRef.value, () => (draft.value?.target.width ?? 16) / (draft.value?.target.height ?? 9))
watch(() => [draft.value?.target.width, draft.value?.target.height, mode.value], () => requestAnimationFrame(measure))
const zoom = computed(() => draft.value ? Math.round(box.value.width / draft.value.target.width * 100) : 100)

type Source = { type: TileType; camera?: number }
type Drag =
  | { kind: 'new'; source: Source; pointerId: number }
  | { kind: 'move'; tileId: string; offsetX: number; offsetY: number; startX: number; startY: number; moved: boolean; pointerId: number }
  | { kind: 'resize'; tileId: string; edges: Edge[]; startGx: number; startGy: number; original: DashboardTile[]; pointerId: number }
const drag = ref<Drag | null>(null)
const ghost = ref<{ rect: Rect; verb: string; valid: boolean } | null>(null)
const guides = ref<{ x: number[]; y: number[] }>({ x: [], y: [] })
let pendingDrop: (() => void) | null = null

function toGrid(event: PointerEvent) {
  const rect = frameRef.value!.getBoundingClientRect()
  return { gx: (event.clientX - rect.left) / rect.width * cols.value, gy: (event.clientY - rect.top) / rect.height * rows.value }
}
const step = (event: PointerEvent | KeyboardEvent) => (event.altKey ? 1 : 2)
function edgeLines(exclude: string, axis: 'x' | 'y') {
  const lines: number[] = [0, axis === 'x' ? cols.value : rows.value]
  for (const t of doc.value.tiles) if (t.id !== exclude) {
    if (axis === 'x') lines.push(t.x, t.x + t.w, t.x + t.w / 2); else lines.push(t.y, t.y + t.h, t.y + t.h / 2)
  }
  return lines
}

function startPaletteDrag(event: PointerEvent, source: Source) {
  if (mode.value !== 'edit' || event.button !== 0) return
  event.preventDefault()
  drag.value = { kind: 'new', source, pointerId: event.pointerId }
  window.addEventListener('pointermove', onDragMove)
  window.addEventListener('pointerup', onDragEnd)
  window.addEventListener('pointercancel', cancelDrag)
}
function startTileDrag(event: PointerEvent, tile: DashboardTile) {
  if (mode.value !== 'edit' || event.button !== 0) return
  event.preventDefault()
  ;(event.currentTarget as HTMLElement).focus({ preventScroll: true })
  if (event.shiftKey) {
    selection.value = selection.value.includes(tile.id) ? selection.value.filter(id => id !== tile.id) : [...selection.value, tile.id]
    return
  }
  if (!selection.value.includes(tile.id) || selection.value.length > 1) selection.value = [tile.id]
  const { gx, gy } = toGrid(event)
  drag.value = { kind: 'move', tileId: tile.id, offsetX: gx - tile.x, offsetY: gy - tile.y, startX: event.clientX, startY: event.clientY, moved: false, pointerId: event.pointerId }
  window.addEventListener('pointermove', onDragMove)
  window.addEventListener('pointerup', onDragEnd)
  window.addEventListener('pointercancel', cancelDrag)
}
function startResize(event: PointerEvent, tile: DashboardTile, edges: Edge[]) {
  event.preventDefault()
  event.stopPropagation()
  selection.value = [tile.id]
  const { gx, gy } = toGrid(event)
  drag.value = { kind: 'resize', tileId: tile.id, edges, startGx: gx, startGy: gy, original: doc.value.tiles.map(t => ({ ...t })), pointerId: event.pointerId }
  window.addEventListener('pointermove', onDragMove)
  window.addEventListener('pointerup', onDragEnd)
  window.addEventListener('pointercancel', cancelDrag)
}

function describeTarget(target: DropTarget, source: Source | DashboardTile, moving: boolean): { verb: string; valid: boolean; apply: (() => void) | null } {
  if (target.kind === 'none') return { verb: 'No room', valid: false, apply: null }
  if (target.kind === 'place') return { verb: moving ? 'Move here' : 'Place', valid: true, apply: null }
  if (target.kind === 'split') return { verb: `Split ${target.side}`, valid: true, apply: null }
  const over = doc.value.tiles.find(t => t.id === target.tileId)!
  if (moving) return { verb: 'Swap', valid: true, apply: null }
  if (source.type === 'camera' && over.type === 'camera') return { verb: 'Replace', valid: true, apply: null }
  if (source.type === 'camera' && over.type === 'spotlight') return { verb: 'Add to spotlight', valid: true, apply: null }
  return { verb: 'Drop on an edge to split', valid: false, apply: null }
}

function onDragMove(event: PointerEvent) {
  const d = drag.value
  if (!d || !draft.value || !frameRef.value) return
  const { gx, gy } = toGrid(event)
  pendingDrop = null
  guides.value = { x: [], y: [] }
  if (d.kind === 'resize') {
    let dx = snap(gx - d.startGx, step(event)), dy = snap(gy - d.startGy, step(event))
    const tile = d.original.find(t => t.id === d.tileId)!
    // Snap the dragged edge to other tiles' edges and centres.
    const xEdge = d.edges.includes('left') ? tile.x : d.edges.includes('right') ? tile.x + tile.w : null
    const yEdge = d.edges.includes('top') ? tile.y : d.edges.includes('bottom') ? tile.y + tile.h : null
    if (xEdge !== null) { const s = snapToLines(xEdge + dx, edgeLines(d.tileId, 'x'), 1); dx = Math.round(s.value) - xEdge; if (s.line !== null) guides.value.x.push(s.line) }
    if (yEdge !== null) { const s = snapToLines(yEdge + dy, edgeLines(d.tileId, 'y'), 1); dy = Math.round(s.value) - yEdge; if (s.line !== null) guides.value.y.push(s.line) }
    // Only this tile by default; Shift moves the shared divider with its neighbours.
    const result = resizeTile(d.original, d.tileId, d.edges, dx, dy, cols.value, rows.value, minFor, event.shiftKey)
    if (result) setTiles(result as DashboardTile[])
    const current = doc.value.tiles.find(t => t.id === d.tileId)!
    ghost.value = { rect: current, verb: `${current.w} × ${current.h} cells · ${Math.round(current.w / cols.value * doc.value.target.width)}×${Math.round(current.h / rows.value * doc.value.target.height)}`, valid: !!result }
    return
  }
  if (d.kind === 'move' && !d.moved) {
    if (Math.hypot(event.clientX - d.startX, event.clientY - d.startY) < 4) return
    d.moved = true
  }
  const source: Source | DashboardTile = d.kind === 'new' ? d.source : doc.value.tiles.find(t => t.id === d.tileId)!
  const incomingMin = MIN_SIZE[source.type]
  const dragged = d.kind === 'move' ? d.tileId : null
  // Empty-space drops land in a grid slot (sizes already on the canvas first);
  // Shift fills the whole empty area instead.
  const others = dragged ? doc.value.tiles.filter(t => t.id !== dragged) : doc.value.tiles
  const slots = event.shiftKey ? undefined : preferredSlotSizes(others, cols.value, rows.value)
  const target = findDropTarget(doc.value.tiles, gx, gy, cols.value, rows.value, dragged, minFor, incomingMin, slots)

  if (d.kind === 'move' && target.kind !== 'swap' && target.kind !== 'split') {
    // Keep the tile's size where it fits; otherwise fill the empty space.
    const moving = source as DashboardTile
    let x = snap(gx - d.offsetX, step(event)), y = snap(gy - d.offsetY, step(event))
    const sx = snapToLines(x, edgeLines(moving.id, 'x'), 1), sxr = snapToLines(x + moving.w, edgeLines(moving.id, 'x'), 1)
    if (sx.line !== null) { x = Math.round(sx.value); guides.value.x.push(sx.line) } else if (sxr.line !== null) { x = Math.round(sxr.value) - moving.w; guides.value.x.push(sxr.line) }
    const sy = snapToLines(y, edgeLines(moving.id, 'y'), 1), syb = snapToLines(y + moving.h, edgeLines(moving.id, 'y'), 1)
    if (sy.line !== null) { y = Math.round(sy.value); guides.value.y.push(sy.line) } else if (syb.line !== null) { y = Math.round(syb.value) - moving.h; guides.value.y.push(syb.line) }
    x = Math.max(0, Math.min(cols.value - moving.w, x)); y = Math.max(0, Math.min(rows.value - moving.h, y))
    const moved = moveTile(doc.value.tiles, moving.id, x, y, cols.value, rows.value)
    if (moved) {
      ghost.value = { rect: { x, y, w: moving.w, h: moving.h }, verb: 'Move', valid: true }
      pendingDrop = () => setTiles(moved as DashboardTile[])
      return
    }
    const region = largestEmptyAt(doc.value.tiles, cols.value, rows.value, gx, gy, new Set([moving.id]))
    const space = region && (event.shiftKey ? region
      : slotAt(region, gx, gy, [{ w: moving.w, h: moving.h }, ...(slots ?? [])], incomingMin))
    if (space && space.w >= incomingMin.w && space.h >= incomingMin.h) {
      ghost.value = { rect: space, verb: 'Fit here', valid: true }
      pendingDrop = () => applyRects([{ id: moving.id, ...space }])
      return
    }
    ghost.value = { rect: { x, y, w: moving.w, h: moving.h }, verb: 'No room', valid: false }
    return
  }

  const described = describeTarget(target, source, d.kind === 'move')
  if (target.kind === 'none') { ghost.value = { rect: { x: Math.floor(gx), y: Math.floor(gy), w: incomingMin.w, h: incomingMin.h }, verb: described.verb, valid: false }; return }
  ghost.value = { rect: target.rect, verb: described.verb, valid: described.valid }
  if (!described.valid) return

  if (d.kind === 'move') {
    if (target.kind === 'swap') { const swapped = swapTiles(doc.value.tiles, d.tileId, target.tileId); pendingDrop = () => { if (swapped) setTiles(swapped as DashboardTile[]) } }
    else if (target.kind === 'split') {
      const others = doc.value.tiles.filter(t => t.id !== d.tileId)
      const split = splitTile(others, target.tileId, target.side, minFor(target.tileId), incomingMin)
      if (split) pendingDrop = () => { applyRects([...split.tiles, { id: d.tileId, ...split.rect }]) }
    }
    return
  }
  const src = d.source
  if (target.kind === 'place') pendingDrop = () => addTileAt(src.type, target.rect, src.camera)
  else if (target.kind === 'split') {
    const split = splitTile(doc.value.tiles, target.tileId, target.side, minFor(target.tileId), incomingMin)
    if (split) pendingDrop = () => { applyRects(split.tiles); addTileAt(src.type, split.rect, src.camera) }
  } else if (target.kind === 'swap') {
    const over = doc.value.tiles.find(t => t.id === target.tileId)!
    if (over.type === 'camera' && src.camera) pendingDrop = () => {
      // Replace the source; drop any other tile already showing that camera.
      setTiles(doc.value.tiles.filter(t => !(t.type === 'camera' && t.camera === src.camera && t.id !== over.id)).map(t => t.id === over.id ? { ...t, camera: src.camera! } as DashboardTile : t))
      selection.value = [over.id]
      commit()
    }
    else if (over.type === 'spotlight' && src.camera) pendingDrop = () => {
      const sources = over.sources.kind === 'all' ? over.sources : { kind: 'list' as const, cameras: [...new Set([...over.sources.cameras, src.camera!])] }
      updateTile(over.id, { sources } as Partial<SpotlightTile>)
      selection.value = [over.id]
    }
  }
}
function clearDrag() {
  window.removeEventListener('pointermove', onDragMove)
  window.removeEventListener('pointerup', onDragEnd)
  window.removeEventListener('pointercancel', cancelDrag)
  drag.value = null
  ghost.value = null
  guides.value = { x: [], y: [] }
  pendingDrop = null
}
function onDragEnd() {
  const d = drag.value
  if (d?.kind === 'resize') { clearDrag(); commit(); return }
  const apply = pendingDrop
  clearDrag()
  if (d?.kind === 'move' && !d.moved) return
  if (apply) {
    apply()
    if (draft.value && !isValidLayout(draft.value.tiles, cols.value, rows.value)) undo() // defensive: never keep an invalid layout
    else commit()
  }
}
function cancelDrag() {
  const d = drag.value
  if (d?.kind === 'resize') setTiles(d.original)
  clearDrag()
}

// ----- Templates & arrangement ----------------------------------------------
function priorityOrder(): number[] {
  const current = readingOrderCameras(doc.value, available.value)
  const ordered = [...doc.value.behavior.priority.filter(id => available.value.includes(id)), ...current]
  return [...new Set([...ordered, ...available.value])]
}
function applyTemplate(template: DashboardTemplate) {
  if (!draft.value) return
  const hasContent = draft.value.tiles.length > 0
  if (hasContent && !confirm(`Apply “${template.name}”? Existing tiles are re-flowed into the template; cameras that do not fit stay in the palette.`)) return
  const sources = hasContent ? readingOrderCameras(draft.value, available.value) : priorityOrder()
  const { fills, unplaced } = fillTemplate(template, sources)
  const tiles: DashboardTile[] = []
  let counter = 0
  const id = () => `t${(++counter).toString(36)}`
  for (const { slot, camera } of fills) {
    const rect = { x: slot.x, y: slot.y, w: slot.w, h: slot.h }
    if (slot.kind === 'camera') { if (camera !== null) tiles.push(defaultTile('camera', id(), rect, camera)) }
    else if (slot.kind === 'spotlight') tiles.push({ ...defaultTile('spotlight', id(), rect), sources: { kind: 'all', cameras: [] } } as DashboardTile)
    else if (slot.kind === 'rotate') tiles.push({ ...defaultTile('spotlight', id(), rect), trigger: 'rotate', sources: { kind: 'all', cameras: [] }, holdSeconds: 20 } as DashboardTile)
    else tiles.push(defaultTile(slot.kind, id(), rect))
  }
  draft.value.tiles = tiles
  if (template.target) draft.value.target = { ...template.target }
  else if (draft.value.target.height > draft.value.target.width) draft.value.target = { width: 1920, height: 1080 }
  draft.value.grid = { ...draft.value.grid, cols: 48, rows: 48 }
  trayNotice.value = unplaced.length ? `${unplaced.length} camera${unplaced.length === 1 ? '' : 's'} did not fit and ${unplaced.length === 1 ? 'is' : 'are'} in the palette.` : ''
  selection.value = []
  showTemplates.value = false
  commit()
}
function autoArrange() {
  if (!draft.value) return
  const cams = readingOrderCameras(draft.value, available.value).filter(id => placed.value.has(id))
  const others = draft.value.tiles.filter(t => t.type !== 'camera')
  const rects = equalGrid(cams.length + others.length, cols.value, rows.value)
  const ordered = [...draft.value.tiles.filter(t => t.type === 'camera').sort((a, b) => cams.indexOf((a as { camera: number }).camera) - cams.indexOf((b as { camera: number }).camera)), ...others]
  draft.value.tiles = ordered.map((t, i) => ({ ...t, ...rects[i]! }))
  commit()
}
function doFillGaps() {
  if (!draft.value) return
  applyRects(fillGaps(draft.value.tiles, cols.value, rows.value))
  commit()
}

// ----- Keyboard ---------------------------------------------------------------
function onKeydown(event: KeyboardEvent) {
  const target = event.target as HTMLElement | null
  if (target && (target.closest('input, select, textarea, [contenteditable="true"]'))) return
  const ctrl = event.ctrlKey || event.metaKey
  if (ctrl && event.key.toLowerCase() === 'z') { event.preventDefault(); if (event.shiftKey) redo(); else undo(); return }
  if (ctrl && event.key.toLowerCase() === 'y') { event.preventDefault(); redo(); return }
  if (ctrl && event.key.toLowerCase() === 's') { event.preventDefault(); void save(); return }
  if (event.key === 'Escape') { if (drag.value) cancelDrag(); else if (showTemplates.value) showTemplates.value = false; else selection.value = []; return }
  if (event.key.toLowerCase() === 'p' && !ctrl) { mode.value = mode.value === 'edit' ? 'preview' : 'edit'; return }
  if (mode.value !== 'edit') return
  if (event.key.toLowerCase() === 'g' && !ctrl) { showGrid.value = !showGrid.value; return }
  if (event.key.toLowerCase() === 'f' && !ctrl) { doFillGaps(); return }
  if (event.key === 'Delete' || event.key === 'Backspace') { event.preventDefault(); removeSelected(); return }
  if (event.key === 'Tab' && draft.value?.tiles.length) {
    event.preventDefault()
    const tiles = [...draft.value.tiles].sort((a, b) => a.y - b.y || a.x - b.x)
    const index = tiles.findIndex(t => t.id === selection.value[0])
    const next = tiles[(index + (event.shiftKey ? -1 : 1) + tiles.length) % tiles.length]!
    selection.value = [next.id]
    document.querySelector<HTMLElement>(`[data-edit-tile="${next.id}"]`)?.focus()
    return
  }
  const arrows: Record<string, [number, number]> = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }
  const arrow = arrows[event.key]
  const tile = selectedTile.value
  if (!arrow || !tile || !draft.value) return
  event.preventDefault()
  const s = step(event)
  if (event.shiftKey) {
    const edges: Edge[] = [arrow[0] !== 0 ? 'right' : 'bottom']
    const result = resizeTile(draft.value.tiles, tile.id, edges, arrow[0] * s, arrow[1] * s, cols.value, rows.value, minFor, false)
    if (result) { setTiles(result as DashboardTile[]); commit(`key-resize-${tile.id}`) }
  } else {
    const result = moveTile(draft.value.tiles, tile.id, tile.x + arrow[0] * s, tile.y + arrow[1] * s, cols.value, rows.value)
    if (result) { setTiles(result as DashboardTile[]); commit(`key-move-${tile.id}`) }
  }
}

// ----- Multi-select alignment ---------------------------------------------------
function alignSelected(kind: 'left' | 'top' | 'size') {
  if (!draft.value || selection.value.length < 2) return
  const chosen = draft.value.tiles.filter(t => selection.value.includes(t.id))
  const anchor = chosen[0]!
  const candidate = draft.value.tiles.map(t => {
    if (!selection.value.includes(t.id) || t.id === anchor.id) return t
    if (kind === 'left') return { ...t, x: anchor.x }
    if (kind === 'top') return { ...t, y: anchor.y }
    return { ...t, w: anchor.w, h: anchor.h }
  })
  if (isValidLayout(candidate, cols.value, rows.value)) { setTiles(candidate); commit() }
  else trayNotice.value = 'That alignment would overlap other tiles.'
}

// ----- Inspector helpers ----------------------------------------------------------
const spotlightOrder = computed(() => draft.value ? spotlightsBySize(draft.value).map(t => t.id) : [])
function spotlightRank(id: string) { return spotlightOrder.value.indexOf(id) + 1 }
function spotlightRankText(id: string) {
  const rank = spotlightRank(id), total = spotlightOrder.value.length
  if (total <= 1) return 'Shows the most important camera with motion.'
  const ordinal = ['1st', '2nd', '3rd'][rank - 1] ?? `${rank}th`
  return rank === 1 ? `Largest spotlight: fills 1st of ${total}. Smaller spotlights take the next cameras.`
    : `Fallback: fills ${ordinal} of ${total} (spotlights fill largest first).`
}
function cameraName(id: number) { return cameraStore.getCameraById(id)?.name ?? `Camera ${id}` }
function moveInList(list: number[], index: number, direction: number) {
  const next = [...list], to = index + direction
  if (to < 0 || to >= next.length) return list
  ;[next[index], next[to]] = [next[to]!, next[index]!]
  return next
}
function toggleInList(list: number[], id: number) { return list.includes(id) ? list.filter(x => x !== id) : [...list, id] }
function setBehavior(patch: Partial<DashboardDocument['behavior']>, key: string | null = null) {
  if (!draft.value) return
  draft.value.behavior = { ...draft.value.behavior, ...patch }
  commit(key)
}
function setGrid(patch: Partial<DashboardDocument['grid']>) { if (draft.value) { draft.value.grid = { ...draft.value.grid, ...patch }; commit() } }
function setTarget(value: string) {
  if (!draft.value) return
  if (value === 'window') draft.value.target = { width: Math.max(320, Math.round(window.screen.width)), height: Math.max(320, Math.round(window.screen.height)) }
  else { const [w, h] = value.split('x').map(Number); draft.value.target = { width: w!, height: h! } }
  commit()
}
const screenSize = `${window.screen.width} × ${window.screen.height}`
const targetKey = computed(() => draft.value ? `${draft.value.target.width}x${draft.value.target.height}` : '1920x1080')
const TARGETS = [['1920x1080', '1920 × 1080 (HD)'], ['2560x1440', '2560 × 1440'], ['3840x2160', '3840 × 2160 (4K)'], ['3440x1440', '3440 × 1440 (ultrawide)'], ['1080x1920', '1080 × 1920 (portrait)']]
const clockBadge = computed(() => draft.value?.badges.find(b => b.type === 'clock') ?? null)
function setClockBadge(anchor: string) {
  if (!draft.value) return
  draft.value.badges = anchor === 'off' ? [] : [{ id: 'b1', type: 'clock', anchor: anchor as 'bottom-right', size: clockBadge.value?.size ?? 'm' }]
  commit()
}
const priorityList = computed(() => draft.value ? [...new Set([...draft.value.behavior.priority.filter(id => available.value.includes(id)), ...available.value])] : [])

// ----- Edit-mode visuals ------------------------------------------------------------
const tick = ref(0)
const tickTimer = setInterval(() => { tick.value++ }, 2000)
function rectStyle(rect: Rect) {
  const half = (draft.value?.grid.gap ?? 4) / 2
  return {
    left: `calc(${rect.x / cols.value * 100}% + ${half}px)`, top: `calc(${rect.y / rows.value * 100}% + ${half}px)`,
    width: `calc(${rect.w / cols.value * 100}% - ${half * 2}px)`, height: `calc(${rect.h / rows.value * 100}% - ${half * 2}px)`,
    borderRadius: `${draft.value?.grid.radius ?? 8}px`,
  }
}
const HANDLES: { edges: Edge[]; cls: string; label: string }[] = [
  { edges: ['top'], cls: 'h-n', label: 'top edge' }, { edges: ['bottom'], cls: 'h-s', label: 'bottom edge' },
  { edges: ['left'], cls: 'h-w', label: 'left edge' }, { edges: ['right'], cls: 'h-e', label: 'right edge' },
  { edges: ['top', 'left'], cls: 'h-nw', label: 'top-left corner' }, { edges: ['top', 'right'], cls: 'h-ne', label: 'top-right corner' },
  { edges: ['bottom', 'left'], cls: 'h-sw', label: 'bottom-left corner' }, { edges: ['bottom', 'right'], cls: 'h-se', label: 'bottom-right corner' },
]
const WIDGETS: { type: TileType; label: string; hint: string }[] = [
  { type: 'spotlight', label: 'Spotlight', hint: 'Shows whichever cameras have motion; splits up to 2 × 2' },
  { type: 'activity', label: 'Activity', hint: 'Recent clips' },
  { type: 'clock', label: 'Clock', hint: 'Server-local time' },
  { type: 'status', label: 'Status', hint: 'Cameras online, recording and motion' },
  { type: 'timeline', label: 'Timeline', hint: 'Clip activity over recent hours' },
  { type: 'detection', label: 'Latest detection', hint: 'Most recent tagged clip' },
  { type: 'ptz', label: 'PTZ pad', hint: 'Pan, tilt and zoom for one camera' },
  { type: 'text', label: 'Text label', hint: 'A short note or location name' },
]
function tileTitle(tile: DashboardTile) {
  switch (tile.type) {
    case 'camera': return cameraName(tile.camera)
    case 'spotlight': return `${tile.trigger === 'rotate' ? 'Rotation' : 'Spotlight'} · ${tile.sources.kind === 'all' ? 'all cameras' : `${tile.sources.cameras.length} source${tile.sources.cameras.length === 1 ? '' : 's'}`}`
    case 'ptz': return `PTZ · ${cameraName(tile.camera)}`
    case 'unavailable': return 'Not available for your account'
    default: return WIDGETS.find(w => w.type === tile.type)?.label ?? tile.type
  }
}
function templateThumb(template: DashboardTemplate) {
  return template.slots.map(s => ({ ...s, fill: s.kind === 'camera' ? '#2b4563' : s.kind === 'activity' ? '#4a3d66' : s.kind === 'clock' ? '#3b4a3a' : '#5a4320' }))
}

// Preview renders the real canvas at target size; provide playback for it.
const previewCameras = computed(() => draft.value ? readingOrderCameras(draft.value, available.value) : [])
const playback = useDashboardPlayback(previewCameras, computed(() => mode.value !== 'preview'), () => undefined)
provide(dashboardPlaybackKey, playback)
const previewScale = computed(() => draft.value ? box.value.width / draft.value.target.width : 1)

onMounted(async () => {
  if (isPhoneViewport()) { void router.replace(`/d/${dashboardId.value}`); return }
  store.listen()
  window.addEventListener('keydown', onKeydown)
  window.addEventListener('beforeunload', onBeforeUnload)
  if (cameraStore.cameras.length === 0) await cameraStore.fetchCameras()
  await load()
  requestAnimationFrame(measure)
})
onUnmounted(() => {
  window.removeEventListener('keydown', onKeydown)
  window.removeEventListener('beforeunload', onBeforeUnload)
  clearDrag()
  clearInterval(tickTimer)
  if (draftTimer) clearTimeout(draftTimer)
})
watch(dashboardId, () => load())
// Someone else published while we edit: only flag it; the save conflict flow decides.
// Only a revision newer than ours (or a delete) is someone else's change; our
// own save announces the revision we already hold.
const dismissedRemote = ref(0)
const remoteRevision = computed(() => store.changes.get(dashboardId.value))
const remoteChanged = computed(() => {
  const announced = remoteRevision.value
  if (announced === undefined || !record.value || saving.value) return false
  return announced === -1 || (announced > record.value.revision && announced !== dismissedRemote.value)
})
</script>

<template>
  <div class="editor" :class="{ dragging: !!drag }">
    <header class="editor-bar">
      <RouterLink class="bar-btn" :to="`/d/${dashboardId}`" title="Back to the dashboard">←</RouterLink>
      <input v-if="draft" v-model="name" class="editor-name" aria-label="Dashboard name" maxlength="80" @input="commit('name')">
      <span v-if="record" class="scope-chip" :class="record.scope">{{ isShared ? 'Shared' : 'Personal' }}</span>
      <span class="bar-gap" />
      <button class="bar-btn" type="button" :disabled="!canUndo" title="Undo (Ctrl+Z)" aria-label="Undo" @click="undo">↶</button>
      <button class="bar-btn" type="button" :disabled="!canRedo" title="Redo (Ctrl+Shift+Z)" aria-label="Redo" @click="redo">↷</button>
      <div class="segmented" role="group" aria-label="Editor mode">
        <button type="button" :class="{ active: mode === 'edit' }" @click="mode = 'edit'">Edit</button>
        <button type="button" :class="{ active: mode === 'preview' }" title="Live preview at target size (P)" @click="mode = 'preview'">Preview</button>
      </div>
      <button class="bar-btn" type="button" @click="showTemplates = true">Templates</button>
      <span class="zoom-readout" v-if="draft">{{ draft.target.width }} × {{ draft.target.height }} · {{ zoom }}%</span>
      <span class="save-message" role="status">{{ saveMessage }}</span>
      <details class="overflow">
        <summary class="bar-btn" aria-label="More actions">⋯</summary>
        <div class="overflow-menu">
          <button v-if="isShared && store.admin" type="button" @click="showShare = true">Share…</button>
          <button type="button" @click="saveAsCopy">Save as a copy</button>
          <button type="button" @click="openHistory">History…</button>
          <button type="button" class="danger" @click="deleteDashboard">Delete</button>
        </div>
      </details>
      <button class="bar-btn primary" type="button" :disabled="!dirty || saving || !name.trim()" @click="save()">{{ isShared ? 'Publish' : 'Save' }}</button>
    </header>

    <div v-if="loadError" class="ui-empty-state m-4"><h2>{{ loadError }}</h2><RouterLink class="btn btn-sm btn-primary" to="/dashboards">All dashboards</RouterLink></div>

    <div v-else-if="draft" class="editor-body">
      <!-- Palette -->
      <aside class="palette" aria-label="Sources">
        <input v-model="paletteFilter" class="form-control form-control-sm" type="search" placeholder="Search cameras" aria-label="Search cameras">
        <h3>Cameras</h3>
        <p class="palette-hint">Drag onto the canvas. Drop on a tile's centre to replace or swap, on its edge to split.</p>
        <div v-for="camera in paletteCameras" :key="camera.id" class="palette-item camera" :class="{ placed: placed.has(camera.id) }"
          tabindex="0" role="button" :aria-label="`Add ${camera.name}`"
          @pointerdown="startPaletteDrag($event, { type: 'camera', camera: camera.id })"
          @keydown.enter.prevent="addTileAnywhere('camera', camera.id)">
          <img :src="`${camera.previewUrl}?t=${tick}`" alt="" loading="lazy" draggable="false">
          <span class="palette-name"><span class="dot" :class="{ ok: camera.status === 'Connected' }" />{{ camera.name }}</span>
          <span v-if="placed.has(camera.id)" class="placed-tick" title="On the canvas">✓</span>
        </div>
        <h3>Smart and widgets</h3>
        <div v-for="widget in WIDGETS" :key="widget.type" class="palette-item widget" tabindex="0" role="button"
          :aria-label="`Add ${widget.label}`" :title="widget.hint"
          @pointerdown="startPaletteDrag($event, { type: widget.type })" @keydown.enter.prevent="addTileAnywhere(widget.type)">
          <span class="widget-icon" :class="widget.type" aria-hidden="true" /><span>{{ widget.label }}</span>
        </div>
        <template v-if="unplacedCameras.length && draft.tiles.length">
          <h3>Not on this dashboard</h3>
          <p class="palette-hint">{{ unplacedCameras.map(c => c.name).join(', ') }}</p>
        </template>
      </aside>

      <!-- Stage -->
      <main class="stage-wrap">
        <div v-if="recoverable" class="banner" role="status">
          Unsaved changes from an earlier session were found.
          <button type="button" @click="restoreDraft">Restore</button><button type="button" @click="discardDraft">Discard</button>
        </div>
        <div v-if="remoteChanged && !conflict" class="banner" role="status">Someone else saved this dashboard. Saving will ask how to combine your changes.<button type="button" @click="dismissedRemote = remoteRevision ?? 0">OK</button></div>
        <div v-if="trayNotice" class="banner" role="status">{{ trayNotice }}<button type="button" @click="trayNotice = ''">×</button></div>
        <div ref="stageRef" class="stage">
          <div ref="frameRef" class="frame" :class="{ 'show-grid': showGrid && mode === 'edit' }"
            :style="{ width: `${box.width}px`, height: `${box.height}px`, '--cols': cols, '--rows': rows }"
            @pointerdown.self="selection = []">
            <template v-if="mode === 'edit'">
              <div v-for="tile in draft.tiles" :key="tile.id" class="edit-tile" :class="[`type-${tile.type}`, { selected: selection.includes(tile.id), dragged: drag?.kind === 'move' && drag.tileId === tile.id && drag.moved }]"
                :style="rectStyle(tile)" tabindex="0" :data-edit-tile="tile.id" role="button" :aria-label="`${tileTitle(tile)}, ${tile.w} by ${tile.h} cells`"
                @pointerdown="startTileDrag($event, tile)" @focus="selection.length === 0 && (selection = [tile.id])">
                <img v-if="tile.type === 'camera' && available.includes(tile.camera)" class="edit-snapshot" :src="`/camera/preview/${tile.camera}?t=${tick}`" alt="" draggable="false">
                <ClockTile v-else-if="tile.type === 'clock'" :seconds="tile.seconds" :date="tile.date" />
                <div v-else class="edit-widget"><span class="widget-icon large" :class="tile.type" aria-hidden="true" /></div>
                <span class="edit-name">{{ tileTitle(tile) }}</span>
                <template v-if="selection.length === 1 && selection[0] === tile.id">
                  <button v-for="handle in HANDLES" :key="handle.cls" type="button" class="handle" :class="handle.cls"
                    :aria-label="`Resize ${handle.label}`" tabindex="-1" @pointerdown="startResize($event, tile, handle.edges)" />
                </template>
              </div>
              <div v-for="x in guides.x" :key="`gx${x}`" class="guide v" :style="{ left: `${x / cols * 100}%` }" />
              <div v-for="y in guides.y" :key="`gy${y}`" class="guide h" :style="{ top: `${y / rows * 100}%` }" />
              <div v-if="ghost" class="ghost" :class="{ invalid: !ghost.valid }" :style="rectStyle(ghost.rect)"><span>{{ ghost.verb }}</span></div>
              <div v-if="draft.tiles.length === 0 && !showTemplates" class="empty-canvas">
                <p>Drag cameras and widgets here, or start from a template.</p>
                <button class="btn btn-sm btn-primary" type="button" @click="showTemplates = true">Choose a template</button>
              </div>
            </template>
            <div v-else class="preview-host" :style="{ width: `${draft.target.width}px`, height: `${draft.target.height}px`, transform: `scale(${previewScale})` }">
              <DashboardCanvas :document="draft" />
            </div>
          </div>
        </div>
        <div class="stage-tools" v-if="mode === 'edit'">
          <button type="button" @click="doFillGaps" title="Grow tiles into empty space (F)">Fill gaps</button>
          <button type="button" @click="autoArrange" title="Arrange all tiles in an equal grid">Auto-arrange</button>
          <label><input v-model="showGrid" type="checkbox"> Grid (G)</label>
          <span class="tools-hint">Alt: fine steps · Shift while resizing: move the shared edge · Shift while dropping: fill the empty area · Shift-click: select several</span>
        </div>
      </main>

      <!-- Inspector -->
      <aside class="inspector" aria-label="Settings">
        <template v-if="selection.length > 1">
          <h3>{{ selection.length }} tiles</h3>
          <div class="btn-row">
            <button type="button" @click="alignSelected('left')">Align left</button>
            <button type="button" @click="alignSelected('top')">Align top</button>
            <button type="button" @click="alignSelected('size')">Same size</button>
          </div>
          <button class="danger-link" type="button" @click="removeSelected">Remove tiles</button>
        </template>

        <template v-else-if="selectedTile">
          <h3>{{ tileTitle(selectedTile) }}</h3>
          <p v-if="selectedTile.type === 'spotlight' && spotlightRank(selectedTile.id) > 0" class="readout">{{ spotlightRankText(selectedTile.id) }}</p>
          <p class="readout">{{ selectedTile.w }} × {{ selectedTile.h }} cells · {{ Math.round(selectedTile.w / cols * draft.target.width) }}×{{ Math.round(selectedTile.h / rows * draft.target.height) }} at target</p>

          <template v-if="selectedTile.type === 'camera'">
            <label>Camera
              <select class="form-select form-select-sm" :value="selectedTile.camera" @change="updateTile(selectedTile.id, { camera: Number(($event.target as HTMLSelectElement).value) })">
                <option v-for="camera in cameraStore.cameras" :key="camera.id" :value="camera.id" :disabled="placed.has(camera.id) && camera.id !== (selectedTile as any).camera">{{ camera.name }}</option>
              </select>
            </label>
            <label>Stream
              <select class="form-select form-select-sm" :value="selectedTile.stream" @change="updateTile(selectedTile.id, { stream: ($event.target as HTMLSelectElement).value as any })">
                <option value="auto">Auto (dashboard stream budget)</option><option value="main">Always main</option><option value="sub">Always sub-stream</option>
              </select>
            </label>
            <label class="check"><input type="checkbox" :checked="selectedTile.label" @change="updateTile(selectedTile.id, { label: ($event.target as HTMLInputElement).checked })"> Show name</label>
            <label class="check"><input type="checkbox" :checked="selectedTile.playback" @change="updateTile(selectedTile.id, { playback: ($event.target as HTMLInputElement).checked })"> Playback bar (DVR scrub-back)</label>
          </template>

          <template v-else-if="selectedTile.type === 'spotlight'">
            <label>Behaviour
              <select class="form-select form-select-sm" :value="selectedTile.trigger" @change="updateTile(selectedTile.id, { trigger: ($event.target as HTMLSelectElement).value as any })">
                <option value="motion">Show cameras with motion</option><option value="rotate">Rotate through cameras</option>
              </select>
            </label>
            <label>Sources
              <select class="form-select form-select-sm" :value="selectedTile.sources.kind" @change="updateTile(selectedTile.id, { sources: { kind: ($event.target as HTMLSelectElement).value as any, cameras: (selectedTile as SpotlightTile).sources.cameras } })">
                <option value="all">All cameras (dashboard priority)</option><option value="list">Chosen cameras, in this order</option>
              </select>
            </label>
            <ol v-if="selectedTile.sources.kind === 'list'" class="rank-list">
              <li v-for="(id, index) in selectedTile.sources.cameras" :key="id">
                <span>{{ cameraName(id) }}</span>
                <button type="button" :aria-label="`Raise ${cameraName(id)}`" :disabled="index === 0" @click="updateTile(selectedTile.id, { sources: { kind: 'list', cameras: moveInList((selectedTile as SpotlightTile).sources.cameras, index, -1) } })">↑</button>
                <button type="button" :aria-label="`Lower ${cameraName(id)}`" :disabled="index === selectedTile.sources.cameras.length - 1" @click="updateTile(selectedTile.id, { sources: { kind: 'list', cameras: moveInList((selectedTile as SpotlightTile).sources.cameras, index, 1) } })">↓</button>
                <button type="button" :aria-label="`Remove ${cameraName(id)}`" @click="updateTile(selectedTile.id, { sources: { kind: 'list', cameras: toggleInList((selectedTile as SpotlightTile).sources.cameras, id) } })">×</button>
              </li>
            </ol>
            <label v-if="selectedTile.sources.kind === 'list'">Add camera
              <select class="form-select form-select-sm" value="" @change="updateTile(selectedTile.id, { sources: { kind: 'list', cameras: toggleInList((selectedTile as SpotlightTile).sources.cameras, Number(($event.target as HTMLSelectElement).value)) } }); ($event.target as HTMLSelectElement).value = ''">
                <option value="" disabled>Choose…</option>
                <option v-for="camera in cameraStore.cameras.filter(c => !(selectedTile as SpotlightTile).sources.cameras.includes(c.id))" :key="camera.id" :value="camera.id">{{ camera.name }}</option>
              </select>
            </label>
            <div class="split-range">
              <label>Show at least
                <select class="form-select form-select-sm" :value="Math.min(selectedTile.minSplit ?? 1, selectedTile.maxSplit)" @change="updateTile(selectedTile.id, { minSplit: Number(($event.target as HTMLSelectElement).value) })">
                  <option v-for="n in [1, 2, 3, 4].filter(n => n <= (selectedTile as SpotlightTile).maxSplit)" :key="n" :value="n">{{ n }}</option>
                </select>
              </label>
              <label>Show up to
                <select class="form-select form-select-sm" :value="selectedTile.maxSplit" @change="updateTile(selectedTile.id, { maxSplit: Number(($event.target as HTMLSelectElement).value), minSplit: Math.min((selectedTile as SpotlightTile).minSplit ?? 1, Number(($event.target as HTMLSelectElement).value)) })">
                  <option :value="1">1 camera</option><option :value="2">2 (split)</option><option :value="3">3</option><option :value="4">4 (2 × 2)</option>
                </select>
              </label>
            </div>
            <p class="readout">Cameras with motion come first; remaining cells show the highest-priority cameras.</p>
            <label>{{ selectedTile.trigger === 'rotate' ? 'Seconds per camera' : 'Hold after motion (seconds)' }}
              <input class="form-control form-control-sm" type="number" min="3" max="120" :value="selectedTile.holdSeconds" @change="updateTile(selectedTile.id, { holdSeconds: Math.max(3, Math.min(120, Number(($event.target as HTMLInputElement).value) || 15)) })">
            </label>
            <label v-if="selectedTile.trigger === 'motion'">When nothing moves
              <select class="form-select form-select-sm" :value="selectedTile.idle.kind" @change="updateTile(selectedTile.id, { idle: (($event.target as HTMLSelectElement).value === 'camera' ? { kind: 'camera', camera: (selectedTile as SpotlightTile).sources.cameras[0] ?? available[0] ?? 0 } : ($event.target as HTMLSelectElement).value === 'rotate' ? { kind: 'rotate', everySeconds: 20 } : { kind: ($event.target as HTMLSelectElement).value }) as any })">
                <option value="last">Keep the last camera</option><option value="camera">Show a home camera</option><option value="rotate">Rotate through sources</option><option value="blank">Show nothing</option>
              </select>
            </label>
            <p v-if="selectedTile.trigger === 'motion' && selectedTile.idle.kind === 'camera'" class="readout">When motion elsewhere takes this spotlight, the home camera moves to the next smaller spotlight instead of disappearing.</p>
            <label v-if="selectedTile.trigger === 'motion' && selectedTile.idle.kind === 'camera'">Home camera
              <select class="form-select form-select-sm" :value="selectedTile.idle.camera" @change="updateTile(selectedTile.id, { idle: { kind: 'camera', camera: Number(($event.target as HTMLSelectElement).value) } })">
                <option v-for="camera in cameraStore.cameras" :key="camera.id" :value="camera.id">{{ camera.name }}</option>
              </select>
            </label>
            <label v-if="selectedTile.trigger === 'motion' && selectedTile.idle.kind === 'rotate'">Rotate every (seconds)
              <input class="form-control form-control-sm" type="number" min="5" max="600" :value="selectedTile.idle.everySeconds" @change="updateTile(selectedTile.id, { idle: { kind: 'rotate', everySeconds: Math.max(5, Math.min(600, Number(($event.target as HTMLInputElement).value) || 20)) } })">
            </label>
            <label class="check"><input type="checkbox" :checked="selectedTile.borrowPlaced" @change="updateTile(selectedTile.id, { borrowPlaced: ($event.target as HTMLInputElement).checked })"> May show cameras that have their own tile</label>
            <label class="check" title="Keeps hidden sources connected so they appear instantly. Each costs a decoder."><input type="checkbox" :checked="selectedTile.warm" @change="updateTile(selectedTile.id, { warm: ($event.target as HTMLInputElement).checked })"> Keep sources warm (instant switch)</label>
          </template>

          <template v-else-if="selectedTile.type === 'activity' || selectedTile.type === 'timeline' || selectedTile.type === 'detection'">
            <label>Cameras
              <select class="form-select form-select-sm" :value="selectedTile.scope.kind" @change="updateTile(selectedTile.id, { scope: { kind: ($event.target as HTMLSelectElement).value as any, cameras: (selectedTile as any).scope.cameras ?? [] } })">
                <option value="dashboard">This dashboard's cameras</option><option value="all">All my cameras</option><option value="list">Chosen cameras</option>
              </select>
            </label>
            <div v-if="selectedTile.scope.kind === 'list'" class="check-list">
              <label v-for="camera in cameraStore.cameras" :key="camera.id" class="check">
                <input type="checkbox" :checked="(selectedTile.scope.cameras ?? []).includes(camera.id)" @change="updateTile(selectedTile.id, { scope: { kind: 'list', cameras: toggleInList((selectedTile as any).scope.cameras ?? [], camera.id) } })"> {{ camera.name }}
              </label>
            </div>
            <label v-if="selectedTile.type === 'activity'">Direction
              <select class="form-select form-select-sm" :value="selectedTile.direction" @change="updateTile(selectedTile.id, { direction: ($event.target as HTMLSelectElement).value as any })">
                <option value="auto">Automatic (from shape)</option><option value="horizontal">Horizontal</option><option value="vertical">Vertical</option>
              </select>
            </label>
            <label v-if="selectedTile.type === 'timeline'">Hours shown
              <select class="form-select form-select-sm" :value="selectedTile.hours" @change="updateTile(selectedTile.id, { hours: Number(($event.target as HTMLSelectElement).value) })">
                <option :value="1">1 hour</option><option :value="6">6 hours</option><option :value="12">12 hours</option><option :value="24">24 hours</option><option :value="48">48 hours</option>
              </select>
            </label>
          </template>

          <template v-else-if="selectedTile.type === 'clock'">
            <label class="check"><input type="checkbox" :checked="selectedTile.seconds" @change="updateTile(selectedTile.id, { seconds: ($event.target as HTMLInputElement).checked })"> Seconds</label>
            <label class="check"><input type="checkbox" :checked="selectedTile.date" @change="updateTile(selectedTile.id, { date: ($event.target as HTMLInputElement).checked })"> Date</label>
          </template>
          <template v-else-if="selectedTile.type === 'status'">
            <label>Show
              <select class="form-select form-select-sm" :value="selectedTile.variant" @change="updateTile(selectedTile.id, { variant: ($event.target as HTMLSelectElement).value as any })">
                <option value="summary">Summary counts</option><option value="health">Per-camera status</option>
              </select>
            </label>
          </template>
          <template v-else-if="selectedTile.type === 'ptz'">
            <label>Camera
              <select class="form-select form-select-sm" :value="selectedTile.camera" @change="updateTile(selectedTile.id, { camera: Number(($event.target as HTMLSelectElement).value) })">
                <option v-for="camera in cameraStore.cameras.filter(c => c.ptzEnabled)" :key="camera.id" :value="camera.id">{{ camera.name }}</option>
              </select>
            </label>
            <label class="check"><input type="checkbox" :checked="selectedTile.label" @change="updateTile(selectedTile.id, { label: ($event.target as HTMLInputElement).checked })"> Show name</label>
          </template>
          <template v-else-if="selectedTile.type === 'text'">
            <label>Text<input class="form-control form-control-sm" maxlength="200" :value="selectedTile.text" @input="updateTile(selectedTile.id, { text: ($event.target as HTMLInputElement).value }, `text-${selectedTile.id}`)"></label>
            <label>Size
              <select class="form-select form-select-sm" :value="selectedTile.size" @change="updateTile(selectedTile.id, { size: ($event.target as HTMLSelectElement).value as any })">
                <option value="s">Small</option><option value="m">Medium</option><option value="l">Large</option>
              </select>
            </label>
          </template>
          <p v-else-if="selectedTile.type === 'unavailable'" class="readout">This tile references a camera you cannot access. It is kept for other viewers; you can move or remove it.</p>
          <button class="danger-link" type="button" @click="removeSelected">Remove tile (Delete)</button>
        </template>

        <template v-else>
          <h3>Canvas</h3>
          <label>Target display
            <select class="form-select form-select-sm" :value="TARGETS.some(t => t[0] === targetKey) ? targetKey : 'custom'" @change="setTarget(($event.target as HTMLSelectElement).value)">
              <option v-for="t in TARGETS" :key="t[0]" :value="t[0]">{{ t[1] }}</option>
              <option value="window">This screen ({{ screenSize }})</option>
              <option v-if="!TARGETS.some(t => t[0] === targetKey)" value="custom" disabled>{{ draft.target.width }} × {{ draft.target.height }}</option>
            </select>
          </label>
          <label>Gap between tiles
            <select class="form-select form-select-sm" :value="draft.grid.gap" @change="setGrid({ gap: Number(($event.target as HTMLSelectElement).value) })">
              <option :value="0">None</option><option :value="2">2 px</option><option :value="4">4 px</option><option :value="8">8 px</option>
            </select>
          </label>
          <label>Corner radius
            <select class="form-select form-select-sm" :value="draft.grid.radius" @change="setGrid({ radius: Number(($event.target as HTMLSelectElement).value) })">
              <option :value="0">Square</option><option :value="4">4 px</option><option :value="8">8 px</option><option :value="12">12 px</option>
            </select>
          </label>
          <label>Clock badge
            <select class="form-select form-select-sm" :value="clockBadge?.anchor ?? 'off'" @change="setClockBadge(($event.target as HTMLSelectElement).value)">
              <option value="off">Off</option><option value="top-left">Top left</option><option value="top-right">Top right</option><option value="bottom-left">Bottom left</option><option value="bottom-right">Bottom right</option>
            </select>
          </label>
          <label>Return to live after inactivity
            <select class="form-select form-select-sm" :value="draft.behavior.returnToLiveSeconds" @change="setBehavior({ returnToLiveSeconds: Number(($event.target as HTMLSelectElement).value) })">
              <option :value="0">Never</option><option :value="60">1 minute</option><option :value="180">3 minutes</option><option :value="300">5 minutes</option><option :value="600">10 minutes</option>
            </select>
          </label>
          <label>Full-quality streams
            <select class="form-select form-select-sm" :value="draft.behavior.mainStreams ?? 0" @change="setBehavior({ mainStreams: Number(($event.target as HTMLSelectElement).value) })">
              <option :value="0">Automatic (largest tiles, within a decode budget)</option>
              <option v-for="n in [1, 2, 3, 4, 6, 8]" :key="n" :value="n">Largest {{ n }} tile{{ n === 1 ? '' : 's' }}</option>
              <option :value="-1">No limit (size-based only)</option>
            </select>
          </label>
          <p class="readout">Other cameras use their sub-stream. Pin a camera tile to Main or Sub in its settings.</p>
          <label class="check"><input type="checkbox" :checked="draft.behavior.quietControls" @change="setBehavior({ quietControls: ($event.target as HTMLInputElement).checked })"> Quiet controls on unselected cameras</label>
          <h3>Camera priority</h3>
          <p class="readout">Spotlights using "all cameras" pick active cameras in this order.</p>
          <ol class="rank-list">
            <li v-for="(id, index) in priorityList" :key="id">
              <span>{{ cameraName(id) }}</span>
              <button type="button" :aria-label="`Raise ${cameraName(id)}`" :disabled="index === 0" @click="setBehavior({ priority: moveInList(priorityList, index, -1) })">↑</button>
              <button type="button" :aria-label="`Lower ${cameraName(id)}`" :disabled="index === priorityList.length - 1" @click="setBehavior({ priority: moveInList(priorityList, index, 1) })">↓</button>
            </li>
          </ol>
        </template>
      </aside>
    </div>

    <!-- Templates -->
    <div v-if="showTemplates && draft" class="modal-backdrop-lite" @click.self="showTemplates = false">
      <div class="templates" role="dialog" aria-modal="true" aria-label="Templates">
        <header><h2>Start from a template</h2><button class="btn-close btn-close-white" aria-label="Close" @click="showTemplates = false" /></header>
        <p class="readout">Cameras fill the slots in priority order. You can change everything afterwards.</p>
        <template v-for="family in ['Grids', 'Hero', 'With activity', 'Smart', 'Portrait']" :key="family">
          <h3>{{ family }}</h3>
          <div class="template-grid">
            <button v-for="template in DASHBOARD_TEMPLATES.filter(t => t.family === family)" :key="template.id" type="button" class="template" @click="applyTemplate(template)">
              <svg :viewBox="template.target && template.target.height > template.target.width ? '0 0 27 48' : '0 0 48 27'" aria-hidden="true">
                <g :transform="template.target && template.target.height > template.target.width ? 'scale(0.5625 1)' : 'scale(1 0.5625)'">
                  <rect v-for="(slot, i) in templateThumb(template)" :key="i" :x="slot.x + 0.6" :y="slot.y + 0.6" :width="slot.w - 1.2" :height="slot.h - 1.2" rx="1.2" :fill="slot.fill" />
                </g>
              </svg>
              <span>{{ template.name }}</span>
            </button>
          </div>
        </template>
      </div>
    </div>

    <!-- Save conflict -->
    <div v-if="conflict" class="modal-backdrop-lite">
      <div class="templates small-dialog" role="alertdialog" aria-modal="true" aria-labelledby="conflict-title">
        <h2 id="conflict-title">Someone else saved this dashboard</h2>
        <p class="readout">It was changed after you started editing. Choose how to continue.</p>
        <div class="btn-row">
          <button type="button" @click="reloadAfterConflict">Reload theirs</button>
          <button type="button" @click="overwrite">Overwrite with mine</button>
          <button type="button" class="primary" @click="saveAsCopy">Save mine as a copy</button>
        </div>
      </div>
    </div>

    <!-- History -->
    <div v-if="showHistory" class="modal-backdrop-lite" @click.self="showHistory = false">
      <div class="templates small-dialog" role="dialog" aria-modal="true" aria-label="Version history">
        <header><h2>Version history</h2><button class="btn-close btn-close-white" aria-label="Close" @click="showHistory = false" /></header>
        <p class="readout">Loading a version replaces the canvas; nothing is saved until you save.</p>
        <ul class="history-list">
          <li v-for="entry in historyEntries" :key="entry.revision">
            <span>Version {{ entry.revision }} · {{ new Date(entry.updatedAt * 1000).toLocaleString() }}</span>
            <button type="button" :disabled="entry.revision === record?.revision" @click="loadRevision(entry.revision)">{{ entry.revision === record?.revision ? 'Current' : 'Load' }}</button>
          </li>
        </ul>
      </div>
    </div>

    <ShareDialog v-if="showShare && record" :dashboard-id="record.id" :shares="(record.shares ?? []) as SharePrincipal[]"
      @close="showShare = false" @saved="shares => { if (record) record.shares = shares }" />
  </div>
</template>

<style scoped>
.editor { position: fixed; inset: 0; z-index: 1000; display: flex; flex-direction: column; background: #0b0f16; color: var(--witness-ink, #edf3fc); }
.editor.dragging { cursor: grabbing; user-select: none; }
.editor-bar { display: flex; align-items: center; gap: 6px; height: 48px; padding: 0 10px; border-bottom: 1px solid #1d2838; background: #0f1520; flex: none; }
.bar-btn { display: inline-flex; align-items: center; justify-content: center; min-width: 32px; height: 30px; padding: 0 10px; border: 1px solid #2c3a4d; border-radius: 6px; background: #141d2a; color: #dceafa; font-size: .8rem; text-decoration: none; cursor: pointer; }
.bar-btn:disabled { opacity: .4; cursor: default; }
.bar-btn.primary { background: #4a90d9; border-color: #4a90d9; color: #fff; font-weight: 600; }
.editor-name { width: min(320px, 30vw); height: 30px; padding: 0 8px; border: 1px solid transparent; border-radius: 6px; background: transparent; color: inherit; font-weight: 600; }
.editor-name:hover, .editor-name:focus { border-color: #2c3a4d; background: #141d2a; outline: none; }
.scope-chip { font-size: .68rem; padding: 2px 8px; border-radius: 99px; background: #1d2a3b; color: #a5c7ea; }
.scope-chip.shared { background: #2a2140; color: #cdb6f5; }
.bar-gap { flex: 1; }
.segmented { display: inline-flex; border: 1px solid #2c3a4d; border-radius: 6px; overflow: hidden; }
.segmented button { border: 0; background: #141d2a; color: #b8c9dc; padding: 0 12px; height: 28px; font-size: .78rem; }
.segmented button.active { background: #24364d; color: #fff; }
.zoom-readout { font-size: .72rem; color: var(--witness-muted, #a5b6cc); font-variant-numeric: tabular-nums; }
.save-message { font-size: .75rem; color: #9ad5a8; min-width: 4rem; }
.overflow { position: relative; }
.overflow summary { list-style: none; }
.overflow summary::-webkit-details-marker { display: none; }
.overflow-menu { position: absolute; right: 0; top: 36px; z-index: 20; display: flex; flex-direction: column; min-width: 160px; padding: 4px; border: 1px solid #2c3a4d; border-radius: 8px; background: #141d2a; box-shadow: 0 8px 24px #0009; }
.overflow-menu button { border: 0; background: transparent; color: #dceafa; text-align: left; padding: 6px 10px; border-radius: 5px; font-size: .8rem; }
.overflow-menu button:hover { background: #1f2c3e; }
.overflow-menu .danger { color: #ff9ba6; }
.editor-body { flex: 1; min-height: 0; display: grid; grid-template-columns: 280px minmax(0, 1fr) 320px; }
.palette, .inspector { overflow: auto; padding: 12px; border-right: 1px solid #1d2838; background: #0f1520; }
.inspector { border-right: 0; border-left: 1px solid #1d2838; }
.palette h3, .inspector h3 { font-size: .7rem; text-transform: uppercase; letter-spacing: .1em; color: #8fa6bf; margin: 16px 0 6px; }
.inspector h3:first-child { margin-top: 0; text-transform: none; letter-spacing: 0; font-size: .95rem; color: var(--witness-ink, #edf3fc); }
.palette-hint, .readout { font-size: .72rem; color: var(--witness-muted, #a5b6cc); margin: 0 0 8px; }
.palette-item { position: relative; display: flex; align-items: center; gap: 8px; padding: 5px; margin-bottom: 4px; border: 1px solid transparent; border-radius: 7px; cursor: grab; touch-action: none; user-select: none; font-size: .8rem; }
.palette-item:hover, .palette-item:focus-visible { border-color: #2c3a4d; background: #141d2a; outline: none; }
.palette-item img { width: 96px; height: 54px; flex: none; object-fit: cover; border-radius: 4px; background: #000; }
.palette-item.placed { opacity: .65; }
.palette-name { display: flex; align-items: center; gap: 6px; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.placed-tick { position: absolute; right: 8px; color: #79beef; }
.dot { width: 7px; height: 7px; border-radius: 50%; flex: none; background: #e05a62; }
.dot.ok { background: #4bc77a; }
.widget-icon { width: 22px; height: 22px; flex: none; border-radius: 5px; border: 1.5px solid #79beef; background: #142235; }
.widget-icon.spotlight { border-color: #f5b54a; background: radial-gradient(circle, #f5b54a55 30%, #142235 32%); }
.widget-icon.activity { border-color: #b39ae8; }
.widget-icon.clock { border-radius: 50%; border-color: #9ad5a8; }
.widget-icon.status { border-color: #7fd0c2; }
.widget-icon.timeline { border-color: #79beef; background: linear-gradient(transparent 45%, #79beef 45% 55%, transparent 55%), #142235; }
.widget-icon.detection { border-color: #ef9a79; }
.widget-icon.ptz { border-color: #d5d07a; border-radius: 50%; }
.widget-icon.text { border-color: #c7d6e6; }
.widget-icon.large { width: 40px; height: 40px; }
.stage-wrap { position: relative; display: flex; flex-direction: column; min-width: 0; min-height: 0; padding: 12px; }
.stage { position: relative; flex: 1; min-height: 0; display: flex; align-items: center; justify-content: center; }
.frame { position: relative; flex: none; background: #05070b; box-shadow: 0 12px 40px #0008; border-radius: 6px; overflow: hidden; }
.frame.show-grid { background-image: radial-gradient(circle, #ffffff1c 1px, transparent 1.2px); background-size: calc(100% / var(--cols) * 4) calc(100% / var(--rows) * 4); background-position: 0 0; }
.edit-tile { position: absolute; overflow: hidden; background: #0b1119; cursor: grab; touch-action: none; outline: none; box-shadow: inset 0 0 0 1px #24344a; }
.edit-tile:focus-visible { box-shadow: inset 0 0 0 2px #79beef; }
.edit-tile.selected { box-shadow: inset 0 0 0 2px #79beef; overflow: visible; z-index: 3; }
.edit-tile.dragged { opacity: .35; }
.edit-tile.type-spotlight { background: repeating-linear-gradient(135deg, #0d131c 0 10px, #101824 10px 20px); }
.edit-tile.type-unavailable { background: #0b1017; }
.edit-snapshot { width: 100%; height: 100%; object-fit: contain; pointer-events: none; background: #000; border-radius: inherit; }
.edit-widget { width: 100%; height: 100%; display: flex; align-items: center; justify-content: center; }
.edit-name { position: absolute; left: 8px; bottom: 8px; max-width: calc(100% - 16px); padding: 2px 7px; border-radius: 5px; background: rgba(8, 12, 18, .72); color: #edf3fc; font-size: 12px; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; pointer-events: none; }
.handle { position: absolute; z-index: 4; width: 20px; height: 20px; padding: 0; border: 0; background: transparent; touch-action: none; }
.handle::after { content: ''; position: absolute; left: 5px; top: 5px; width: 10px; height: 10px; background: #79beef; border: 1px solid #0b0f16; }
.h-n { top: -10px; left: calc(50% - 10px); cursor: ns-resize; } .h-s { bottom: -10px; left: calc(50% - 10px); cursor: ns-resize; }
.h-w { left: -10px; top: calc(50% - 10px); cursor: ew-resize; } .h-e { right: -10px; top: calc(50% - 10px); cursor: ew-resize; }
.h-nw { left: -10px; top: -10px; cursor: nwse-resize; } .h-se { right: -10px; bottom: -10px; cursor: nwse-resize; }
.h-ne { right: -10px; top: -10px; cursor: nesw-resize; } .h-sw { left: -10px; bottom: -10px; cursor: nesw-resize; }
.guide { position: absolute; z-index: 5; background: #79beef; pointer-events: none; }
.guide.v { top: 0; bottom: 0; width: 1px; }
.guide.h { left: 0; right: 0; height: 1px; }
.ghost { position: absolute; z-index: 6; display: flex; align-items: center; justify-content: center; border: 2px dashed #79beef; background: #79beef33; pointer-events: none; }
.ghost.invalid { border-color: #ff6474; background: #ff647426; }
.ghost span { padding: 2px 8px; border-radius: 4px; background: #0b0f16dd; color: #e8f3ff; font-size: 12px; font-weight: 600; }
.empty-canvas { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 10px; color: var(--witness-muted, #a5b6cc); font-size: .85rem; }
.preview-host { position: absolute; left: 0; top: 0; transform-origin: 0 0; }
.stage-tools { display: flex; align-items: center; gap: 8px; padding-top: 10px; font-size: .75rem; color: #b8c9dc; flex-wrap: wrap; }
.stage-tools button, .btn-row button { border: 1px solid #2c3a4d; border-radius: 6px; background: #141d2a; color: #dceafa; padding: 4px 10px; font-size: .75rem; }
.btn-row { display: flex; gap: 6px; flex-wrap: wrap; margin: 8px 0; }
.btn-row button.primary { background: #4a90d9; border-color: #4a90d9; color: #fff; }
.tools-hint { color: #7d91a8; }
.banner { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; padding: 6px 10px; border: 1px solid #3d4f66; border-radius: 7px; background: #142235; font-size: .78rem; }
.banner button { border: 1px solid #456; border-radius: 5px; background: #1b2b40; color: #dceafa; padding: 2px 9px; font-size: .75rem; }
.inspector label { display: block; margin: 0 0 10px; font-size: .78rem; color: #c3d2e2; }
.inspector label > select, .inspector label > input:not([type='checkbox']) { margin-top: 4px; }
.inspector label.check { display: flex; align-items: center; gap: 7px; }
.split-range { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.check-list { max-height: 200px; overflow: auto; margin-bottom: 10px; }
.rank-list { padding-left: 1.2rem; margin: 0 0 10px; font-size: .78rem; }
.rank-list li { display: flex; align-items: center; gap: 4px; padding: 2px 0; }
.rank-list li span { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.rank-list button { min-width: 26px; border: 1px solid #2c3a4d; border-radius: 4px; background: #141d2a; color: #dceafa; font-size: .72rem; }
.danger-link { border: 0; background: none; color: #ff9ba6; padding: 0; font-size: .78rem; margin-top: 6px; }
.modal-backdrop-lite { position: fixed; inset: 0; z-index: 2500; display: flex; align-items: center; justify-content: center; padding: 16px; background: rgba(0, 0, 0, .6); }
.templates { width: min(900px, 100%); max-height: calc(100dvh - 32px); overflow: auto; padding: 1rem 1.2rem; border: 1px solid #2d3b4e; border-radius: .8rem; background: #141c28; }
.templates header { display: flex; align-items: center; justify-content: space-between; }
.templates h2 { font-size: 1.05rem; margin: 0 0 .25rem; }
.templates h3 { font-size: .7rem; text-transform: uppercase; letter-spacing: .1em; color: #8fa6bf; margin: 14px 0 6px; }
.small-dialog { width: min(480px, 100%); }
.template-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 10px; }
.template { display: flex; flex-direction: column; gap: 6px; padding: 8px; border: 1px solid #2c3a4d; border-radius: 8px; background: #0f1520; color: #dceafa; font-size: .75rem; }
.template:hover, .template:focus-visible { border-color: #79beef; outline: none; }
.template svg { width: 100%; aspect-ratio: 16 / 9; background: #05070b; border-radius: 4px; }
.history-list { list-style: none; padding: 0; margin: 0; font-size: .8rem; }
.history-list li { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 6px 0; border-bottom: 1px solid #1d2838; }
.history-list button { border: 1px solid #2c3a4d; border-radius: 5px; background: #141d2a; color: #dceafa; padding: 2px 10px; font-size: .75rem; }
@media (max-width: 1200px) { .editor-body { grid-template-columns: 220px minmax(0, 1fr) 260px; } .palette-item img { width: 64px; height: 36px; } }
</style>
