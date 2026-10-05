// Persisted reports can come from older or minimal clients. Missing metadata
// must not hide their otherwise useful player/error evidence.
export function formatHealthTimestamp(value: unknown, timeOnly = false): string {
  if (typeof value !== 'string' || !value.trim()) return 'Unknown'
  const date = new Date(value)
  if (!Number.isFinite(date.getTime())) return 'Unknown'
  return timeOnly ? date.toLocaleTimeString() : date.toLocaleString()
}

export function formatHealthBuild(value: unknown): string {
  return typeof value === 'string' && value.trim() ? value : 'Unknown'
}
