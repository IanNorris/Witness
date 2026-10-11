export interface PlaybackPosition {
  requestedAt: number | null
  viewedAt: number | null
  paused: boolean
  revision: number
  lastInteraction: number
  timelineEnd: number | null
}

export function createPlaybackPosition(now: number): PlaybackPosition {
  return { requestedAt: null, viewedAt: null, paused: false, revision: 0, lastInteraction: now, timelineEnd: null }
}

export function returnToLive(position: PlaybackPosition, now: number) {
  position.requestedAt = position.viewedAt = null
  position.paused = false
  position.timelineEnd = null
  position.lastInteraction = now
  ++position.revision
}

export function seekPosition(position: PlaybackPosition, timestamp: number, now: number) {
  position.requestedAt = position.viewedAt = timestamp
  position.lastInteraction = now
  ++position.revision
}

export function inactivitySeconds(value: unknown): number {
  const numeric = Number(value)
  return Number.isFinite(numeric) ? Math.max(0, Math.min(3600, numeric)) : 180
}

export function isSavingAt(timestamp: number, ranges: { from: number; to: number }[]) {
  return ranges.some(range => timestamp >= range.from && timestamp < range.to)
}

export function shouldReturnToLive(position: PlaybackPosition, now: number, timeout: number, held: boolean) {
  return position.requestedAt !== null && timeout > 0 && !held && now - position.lastInteraction >= timeout * 1000
}
