export type ActivityDock = 'top' | 'right' | 'bottom' | 'left'

export interface DashboardTileLayout {
  cameraId: number
  x: number
  y: number
  width: number
  height: number
}

export interface DashboardLayout {
  version: 1
  columns: 12
  rows: 12
  tiles: DashboardTileLayout[]
  activityDock: ActivityDock
  activitySize: number
}

export function createDefaultDashboardLayout(cameraIds: number[]): DashboardLayout {
  const count = Math.max(1, cameraIds.length)
  const columns = Math.min(count, Math.ceil(Math.sqrt(count * 16 / 9)))
  const rows = Math.ceil(count / columns)
  const cellWidth = Math.floor(12 / columns)
  const cellHeight = Math.floor(12 / rows)

  return {
    version: 1,
    columns: 12,
    rows: 12,
    tiles: cameraIds.map((cameraId, index) => ({
      cameraId,
      x: (index % columns) * cellWidth,
      y: Math.floor(index / columns) * cellHeight,
      width: index % columns === columns - 1 ? 12 - ((columns - 1) * cellWidth) : cellWidth,
      height: Math.floor(index / columns) === rows - 1 ? 12 - ((rows - 1) * cellHeight) : cellHeight,
    })),
    activityDock: 'bottom',
    activitySize: 108,
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
  const missing = fallback.tiles.filter(tile => !seen.has(tile.cameraId))
  const dock = ['top', 'right', 'bottom', 'left'].includes(candidate.activityDock ?? '')
    ? candidate.activityDock as ActivityDock
    : 'bottom'

  return {
    version: 1,
    columns: 12,
    rows: 12,
    tiles: [...validTiles, ...missing],
    activityDock: dock,
    activitySize: Math.max(72, Math.min(320, Number(candidate.activitySize) || 108)),
  }
}
