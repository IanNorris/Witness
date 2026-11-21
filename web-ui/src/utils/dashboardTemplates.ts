// Dashboard templates on the 48x48 grid. Applying a template is a one-off
// starting point: it re-flows the current sources into the camera slots in
// priority order; anything that does not fit goes to the Unplaced tray.
// Import-free for node:test.

export interface TemplateSlot { kind: 'camera' | 'activity' | 'spotlight' | 'rotate' | 'clock'; x: number; y: number; w: number; h: number }
export interface DashboardTemplate {
  id: string
  name: string
  family: 'Grids' | 'Hero' | 'With activity' | 'Smart' | 'Portrait'
  target?: { width: number; height: number }
  slots: TemplateSlot[]
}

const G = 48

function grid(columns: number, rows: number, area = { x: 0, y: 0, w: G, h: G }): TemplateSlot[] {
  const slots: TemplateSlot[] = []
  for (let r = 0; r < rows; r++)
    for (let c = 0; c < columns; c++) {
      const x0 = area.x + Math.round(c * area.w / columns), x1 = area.x + Math.round((c + 1) * area.w / columns)
      const y0 = area.y + Math.round(r * area.h / rows), y1 = area.y + Math.round((r + 1) * area.h / rows)
      slots.push({ kind: 'camera', x: x0, y: y0, w: x1 - x0, h: y1 - y0 })
    }
  return slots
}
const cam = (x: number, y: number, w: number, h: number): TemplateSlot => ({ kind: 'camera', x, y, w, h })

export const DASHBOARD_TEMPLATES: DashboardTemplate[] = [
  { id: 'grid1', name: 'Single', family: 'Grids', slots: grid(1, 1) },
  { id: 'grid2x2', name: '2 × 2', family: 'Grids', slots: grid(2, 2) },
  { id: 'grid3x2', name: '3 × 2', family: 'Grids', slots: grid(3, 2) },
  { id: 'grid3x3', name: '3 × 3', family: 'Grids', slots: grid(3, 3) },
  { id: 'grid4x3', name: '4 × 3', family: 'Grids', slots: grid(4, 3) },
  { id: 'grid4x4', name: '4 × 4', family: 'Grids', slots: grid(4, 4) },
  { id: 'hero5', name: '1 + 5', family: 'Hero', slots: [cam(0, 0, 32, 32), cam(32, 0, 16, 16), cam(32, 16, 16, 16),
    cam(0, 32, 16, 16), cam(16, 32, 16, 16), cam(32, 32, 16, 16)] },
  { id: 'hero7', name: '1 + 2 + 4', family: 'Hero', slots: [cam(0, 0, 32, 32), cam(32, 0, 16, 16), cam(32, 16, 16, 16),
    cam(0, 32, 12, 16), cam(12, 32, 12, 16), cam(24, 32, 12, 16), cam(36, 32, 12, 16)] },
  { id: 'hero12', name: '1 + 12', family: 'Hero', slots: [cam(12, 12, 24, 24),
    ...[0, 12, 24, 36].map(x => cam(x, 0, 12, 12)), ...[0, 12, 24, 36].map(x => cam(x, 36, 12, 12)),
    cam(0, 12, 12, 12), cam(0, 24, 12, 12), cam(36, 12, 12, 12), cam(36, 24, 12, 12)] },
  { id: 'activityRight', name: 'Cameras + activity rail', family: 'With activity',
    slots: [...grid(2, 2, { x: 0, y: 0, w: 38, h: 48 }), { kind: 'activity', x: 38, y: 0, w: 10, h: 48 }] },
  { id: 'activityBottom', name: 'Cameras + activity strip', family: 'With activity',
    slots: [...grid(3, 2, { x: 0, y: 0, w: 48, h: 40 }), { kind: 'activity', x: 0, y: 40, w: 48, h: 8 }] },
  { id: 'spotlightRail', name: 'Spotlight + rail', family: 'Smart',
    slots: [{ kind: 'spotlight', x: 0, y: 0, w: 36, h: 36 }, cam(36, 0, 12, 12), cam(36, 12, 12, 12), cam(36, 24, 12, 12),
      cam(0, 36, 12, 12), cam(12, 36, 12, 12), cam(24, 36, 12, 12), { kind: 'clock', x: 36, y: 36, w: 12, h: 12 }] },
  { id: 'rotationWall', name: 'Rotation wall', family: 'Smart',
    slots: [{ kind: 'rotate', x: 0, y: 0, w: 24, h: 24 }, { kind: 'rotate', x: 24, y: 0, w: 24, h: 24 },
      { kind: 'rotate', x: 0, y: 24, w: 24, h: 24 }, { kind: 'rotate', x: 24, y: 24, w: 24, h: 24 }] },
  { id: 'portrait3', name: 'Portrait 1 × 3', family: 'Portrait', target: { width: 1080, height: 1920 }, slots: grid(1, 3) },
  { id: 'portrait8', name: 'Portrait 2 × 4', family: 'Portrait', target: { width: 1080, height: 1920 }, slots: grid(2, 4) },
]

export interface TemplateFill { slot: TemplateSlot; camera: number | null }

// Assigns cameras (already in priority order) to camera slots. Returns the
// filled slots and the cameras that did not fit.
export function fillTemplate(template: DashboardTemplate, cameras: readonly number[]): { fills: TemplateFill[]; unplaced: number[] } {
  const queue = [...cameras]
  const fills = template.slots.map(slot => ({ slot, camera: slot.kind === 'camera' ? queue.shift() ?? null : null }))
  return { fills, unplaced: queue }
}
