export type ActivityDock = 'top' | 'right' | 'bottom' | 'left'

export interface DashboardTileLayout {
  cameraId: number
  x: number
  y: number
  width: number
  height: number
}

export type DashboardGridArea = Omit<DashboardTileLayout, 'cameraId'>

export interface DashboardLayout {
  version: 1
  columns: 12
  rows: 12
  tiles: DashboardTileLayout[]
  activityDock: ActivityDock
  activitySize: number
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
  const hasEveryCamera = seen.size === cameraIds.length
  const hasOverlap = validTiles.some((tile, index) => validTiles.slice(index + 1).some(other =>
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
