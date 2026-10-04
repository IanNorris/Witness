export type ActivityDock = 'top' | 'right' | 'bottom' | 'left'
export type DashboardMode = 'tiles' | 'layout' | 'fullscreen'
export type DashboardPreset = 'custom' | 'hero7' | 'balanced8' | 'grid16'

export interface DashboardTileLayout {
  cameraId: number
  x: number
  y: number
  width: number
  height: number
  automaticSlotNumber?: number
}

export type DashboardGridArea = Omit<DashboardTileLayout, 'cameraId'>
export interface DashboardAutomaticSlot {
  anchorCameraId: number
  fallbackCameraId: number | null
}

export interface DashboardLayout {
  version: 1
  columns: 12
  rows: 12
  tiles: DashboardTileLayout[]
  activityDock: ActivityDock
  activitySize: number
  activityPlacement: 'dock' | 'rectangle'
  activityRegion: DashboardGridArea
  activityDirection: 'horizontal' | 'vertical'
  preset: DashboardPreset
  priorityCameraIds: number[]
  automaticSlots: DashboardAutomaticSlot[]
  slotHoldSeconds: number
  autoFocusEnabled: boolean
  focusHoldSeconds: number
  focusEligibleCameraIds: number[]
  visibleCameraIds: number[]
  focusRegion: DashboardGridArea
  focusSlots: DashboardGridArea[]
}

export function createDefaultFocusLayout(cameraCount: number) {
  const slotCount = Math.max(0, cameraCount - 1)
  if (slotCount === 0) {
    return {
      focusRegion: { x: 0, y: 0, width: 12, height: 12 },
      focusSlots: [] as DashboardGridArea[],
    }
  }

  const railColumns = Math.min(4, Math.max(1, Math.ceil(slotCount / 12)))
  const railRows = Math.ceil(slotCount / railColumns)
  const cellWidth = Math.floor(4 / railColumns)
  const cellHeight = Math.floor(12 / railRows)
  return {
    focusRegion: { x: 0, y: 0, width: 8, height: 12 },
    focusSlots: Array.from({ length: slotCount }, (_, index) => {
      const column = index % railColumns
      const row = Math.floor(index / railColumns)
      return {
        x: 8 + column * cellWidth,
        y: row * cellHeight,
        width: column === railColumns - 1 ? 4 - column * cellWidth : cellWidth,
        height: row === railRows - 1 ? 12 - row * cellHeight : cellHeight,
      }
    }),
  }
}

export function createDefaultDashboardLayout(cameraIds: number[]): DashboardLayout {
  const count = Math.max(1, cameraIds.length)
  const columns = Math.min(count, Math.ceil(Math.sqrt(count * 16 / 9)))
  const rows = Math.ceil(count / columns)
  const cellHeight = Math.floor(12 / rows)

  const focusLayout = createDefaultFocusLayout(cameraIds.length)
  return {
    version: 1,
    columns: 12,
    rows: 12,
    tiles: cameraIds.map((cameraId, index) => {
      const row = Math.floor(index / columns)
      const column = index % columns
      const itemsInRow = Math.min(columns, cameraIds.length - row * columns)
      const rowCellWidth = Math.floor(12 / itemsInRow)
      return {
        cameraId,
        x: column * rowCellWidth,
        y: row * cellHeight,
        width: column === itemsInRow - 1 ? 12 - column * rowCellWidth : rowCellWidth,
        height: row === rows - 1 ? 12 - row * cellHeight : cellHeight,
      }
    }),
    activityDock: 'bottom',
    activitySize: 108,
    activityPlacement: 'dock',
    activityRegion: { x: 8, y: 8, width: 4, height: 4 },
    activityDirection: 'horizontal',
    preset: 'custom',
    priorityCameraIds: [...cameraIds],
    automaticSlots: [],
    slotHoldSeconds: 15,
    autoFocusEnabled: false,
    focusHoldSeconds: 15,
    focusEligibleCameraIds: [...cameraIds],
    visibleCameraIds: [...cameraIds],
    ...focusLayout,
  }
}

export function normaliseDashboardLayout(
  candidate: Partial<DashboardLayout> | null,
  cameraIds: number[],
): DashboardLayout {
  const fallback = createDefaultDashboardLayout(cameraIds)
  if (!candidate || candidate.version !== 1 || !Array.isArray(candidate.tiles)) return fallback

  const currentIds = new Set(cameraIds)
  const validTiles = candidate.tiles.filter(tile =>
    currentIds.has(tile.cameraId) &&
    Number.isFinite(tile.x) && Number.isFinite(tile.y) &&
    Number.isFinite(tile.width) && Number.isFinite(tile.height),
  ).map(tile => ({
    cameraId: tile.cameraId,
    x: Math.max(0, Math.min(11, Math.round(tile.x))),
    y: Math.max(0, Math.min(11, Math.round(tile.y))),
    width: Math.max(1, Math.min(12, Math.round(tile.width))),
    height: Math.max(1, Math.min(12, Math.round(tile.height))),
  })).map(tile => ({
    ...tile,
    width: Math.min(tile.width, 12 - tile.x),
    height: Math.min(tile.height, 12 - tile.y),
  }))

  const seen = new Set(validTiles.map(tile => tile.cameraId))
  const hasEveryCamera = seen.size === cameraIds.length && validTiles.length === cameraIds.length
  const visibleTiles = validTiles.filter(tile => !Array.isArray(candidate.visibleCameraIds) || candidate.visibleCameraIds.includes(tile.cameraId))
  const hasOverlap = visibleTiles.some((tile, index) => visibleTiles.slice(index + 1).some(other =>
    tile.x < other.x + other.width && tile.x + tile.width > other.x &&
    tile.y < other.y + other.height && tile.y + tile.height > other.y,
  ))
  const dock = ['top', 'right', 'bottom', 'left'].includes(candidate.activityDock ?? '')
    ? candidate.activityDock as ActivityDock
    : 'bottom'
  const defaultFocus = createDefaultFocusLayout(cameraIds.length)
  const normaliseArea = (area: DashboardGridArea | undefined): DashboardGridArea | null => {
    if (!area || !Number.isFinite(area.x) || !Number.isFinite(area.y) ||
        !Number.isFinite(area.width) || !Number.isFinite(area.height)) return null
    const result = {
      x: Math.max(0, Math.min(11, Math.round(area.x))),
      y: Math.max(0, Math.min(11, Math.round(area.y))),
      width: Math.max(1, Math.min(12, Math.round(area.width))),
      height: Math.max(1, Math.min(12, Math.round(area.height))),
    }
    result.width = Math.min(result.width, 12 - result.x)
    result.height = Math.min(result.height, 12 - result.y)
    return result
  }
  const candidateRegion = normaliseArea(candidate.focusRegion)
  const candidateSlots = Array.isArray(candidate.focusSlots)
    ? candidate.focusSlots.map(area => normaliseArea(area)).filter((area): area is DashboardGridArea => area !== null)
    : []
  const requiredSlots = Math.max(0, cameraIds.length - 1)
  const focusAreas = candidateRegion ? [candidateRegion, ...candidateSlots.slice(0, requiredSlots)] : []
  const focusHasOverlap = focusAreas.some((area, index) => focusAreas.slice(index + 1).some(other =>
    area.x < other.x + other.width && area.x + area.width > other.x &&
    area.y < other.y + other.height && area.y + area.height > other.y,
  ))
  const useCandidateFocus = candidateRegion !== null && candidateSlots.length >= requiredSlots && !focusHasOverlap

  return {
    version: 1,
    columns: 12,
    rows: 12,
    tiles: hasEveryCamera && !hasOverlap ? validTiles : fallback.tiles,
    activityDock: dock,
    activitySize: Math.max(72, Math.min(320, Number(candidate.activitySize) || 108)),
    activityPlacement: candidate.activityPlacement === 'rectangle' ? 'rectangle' : 'dock',
    activityRegion: normaliseArea(candidate.activityRegion) ?? fallback.activityRegion,
    activityDirection: candidate.activityDirection === 'vertical' ? 'vertical' : 'horizontal',
    preset: ['hero7', 'balanced8', 'grid16'].includes(candidate.preset ?? '')
      ? candidate.preset as DashboardPreset : 'custom',
    priorityCameraIds: normaliseCameraPriority(candidate.priorityCameraIds, cameraIds),
    automaticSlots: normaliseAutomaticSlots(candidate.automaticSlots, cameraIds),
    slotHoldSeconds: Math.max(3, Math.min(120, Number(candidate.slotHoldSeconds) || 15)),
    autoFocusEnabled: candidate.autoFocusEnabled === true,
    focusHoldSeconds: Math.max(3, Math.min(120, Number(candidate.focusHoldSeconds) || 15)),
    focusEligibleCameraIds: Array.isArray(candidate.focusEligibleCameraIds)
      ? candidate.focusEligibleCameraIds.filter(id => currentIds.has(id))
      : [...cameraIds],
    visibleCameraIds: Array.isArray(candidate.visibleCameraIds)
      ? candidate.visibleCameraIds.filter(id => currentIds.has(id))
      : [...cameraIds],
    focusRegion: useCandidateFocus ? candidateRegion! : defaultFocus.focusRegion,
    focusSlots: useCandidateFocus ? candidateSlots.slice(0, requiredSlots) : defaultFocus.focusSlots,
  }
}

// Removed cameras disappear, duplicates are discarded, and newly added cameras
// join the end. A preset never replaces the user's saved custom rectangles.
export function normaliseCameraPriority(candidate: unknown, cameraIds: number[]): number[] {
  const allowed = new Set(cameraIds)
  const ranked = Array.isArray(candidate) ? candidate.filter(id => typeof id === 'number' && allowed.has(id)) : []
  return [...new Set([...ranked, ...cameraIds])]
}

export function generatedDashboardTiles(preset: DashboardPreset, cameraIds: number[]): DashboardTileLayout[] {
  let areas: DashboardGridArea[]
  switch (preset) {
    case 'hero7':
      areas = [{ x: 0, y: 0, width: 8, height: 8 },
        { x: 8, y: 0, width: 4, height: 4 }, { x: 8, y: 4, width: 4, height: 4 },
        ...Array.from({ length: 4 }, (_, i) => ({ x: i * 3, y: 8, width: 3, height: 4 }))]
      break
    case 'balanced8':
      areas = Array.from({ length: 4 }, (_, i) => ({ x: i % 2 * 6, y: Math.floor(i / 2) * 4, width: 6, height: 4 }))
        .concat(Array.from({ length: 4 }, (_, i) => ({ x: i * 3, y: 8, width: 3, height: 4 })))
      break
    case 'grid16':
      areas = Array.from({ length: 16 }, (_, i) => ({ x: i % 4 * 3, y: Math.floor(i / 4) * 3, width: 3, height: 3 }))
      break
    default:
      return createDefaultDashboardLayout(cameraIds).tiles
  }
  return [...new Set(cameraIds)].slice(0, areas.length).map((cameraId, index) => ({ cameraId, ...areas[index]! }))
}

export function dashboardAreasOverlap(a: DashboardGridArea, b: DashboardGridArea): boolean {
  return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y
}

export function normaliseAutomaticSlots(candidate: unknown, cameraIds: number[]): DashboardAutomaticSlot[] {
  if (!Array.isArray(candidate)) return []
  const allowed = new Set(cameraIds), anchors = new Set<number>()
  return candidate.flatMap(slot => {
    if (!slot || !allowed.has(slot.anchorCameraId) || anchors.has(slot.anchorCameraId)) return []
    anchors.add(slot.anchorCameraId)
    return [{ anchorCameraId: slot.anchorCameraId,
      fallbackCameraId: allowed.has(slot.fallbackCameraId) ? slot.fallbackCameraId as number : null }]
  })
}

// Slot numbering is array order, independent of rectangle size or source ID.
// Fixed cameras are never stolen or duplicated. Geometry stays tied to the
// anchor while the one existing, keyed player moves into its assigned region.
export function resolveAutomaticDashboardTiles(layout: DashboardLayout, activeIds: ReadonlySet<number>): DashboardTileLayout[] {
  const visible = new Set(layout.visibleCameraIds)
  const slots = layout.automaticSlots.filter(slot => visible.has(slot.anchorCameraId))
  const anchors = new Set(slots.map(slot => slot.anchorCameraId))
  const fixed = layout.tiles.filter(tile => visible.has(tile.cameraId) && !anchors.has(tile.cameraId))
  const used = new Set(fixed.map(tile => tile.cameraId))
  const candidates = layout.priorityCameraIds.filter(id => !used.has(id))
  const active = candidates.filter(id => activeIds.has(id))
  const result = [...fixed]
  for (const slot of slots) {
    const area = layout.tiles.find(tile => tile.cameraId === slot.anchorCameraId)
    if (!area) continue
    const cameraId = active.find(id => !used.has(id)) ??
      (slot.fallbackCameraId !== null && candidates.includes(slot.fallbackCameraId) && !used.has(slot.fallbackCameraId) ? slot.fallbackCameraId : undefined) ??
      candidates.find(id => !used.has(id))
    if (cameraId === undefined) continue
    used.add(cameraId)
    result.push({ ...area, cameraId, automaticSlotNumber: layout.automaticSlots.indexOf(slot) + 1 })
  }
  return result
}
