export interface TimeRange { from: number; to: number }
export interface Highlight extends TimeRange { cameraId: number }
export interface HighlightCamera { id: number; clips: TimeRange[]; ranges: TimeRange[] }

// Manual navigation uses actual events, not the reel's padded lead-in/tail.
export function isVideoActivityAt(clips: TimeRange[], at: number): boolean {
  return Number.isFinite(at) && clips.some(range => Number.isFinite(range.from)
    && Number.isFinite(range.to) && range.from <= at && at < range.to)
}

// Union activity across the selected cameras, but only play intervals for which
// at least one camera has a recording. Split at coverage boundaries so the
// clock driver can change without getting stuck in another camera's gap.
export function buildHighlights(cameras: HighlightCamera[], from: number, to: number): Highlight[] {
  const activity = cameras.flatMap(camera => camera.clips)
    .filter(range => Number.isFinite(range.from) && Number.isFinite(range.to) && range.to > range.from)
    .map(range => ({ from: Math.max(from, range.from - 2), to: Math.min(to, range.to + 2) }))
    .filter(range => Number.isFinite(range.from) && Number.isFinite(range.to) && range.to > range.from)
  const boundaries = [...new Set([
    ...activity.flatMap(range => [range.from, range.to]),
    ...cameras.flatMap(camera => camera.ranges.flatMap(range => [range.from, range.to]))
      .filter(value => Number.isFinite(value) && value >= from && value <= to),
  ])].sort((a, b) => a - b)
  const result: Highlight[] = []
  for (let index = 0; index + 1 < boundaries.length; index++) {
    const start = boundaries[index]!, end = boundaries[index + 1]!
    if (!activity.some(range => range.from <= start && range.to >= end)) continue
    const camera = cameras.find(camera => camera.ranges.some(range => range.from <= start && range.to >= end))
    if (!camera) continue
    const previous = result[result.length - 1]
    if (previous && previous.to === start && previous.cameraId === camera.id) previous.to = end
    else result.push({ from: start, to: end, cameraId: camera.id })
  }
  return result
}

export function nextHighlight(highlights: Highlight[], at: number): Highlight | undefined {
  return highlights.find(range => range.to > at)
}
