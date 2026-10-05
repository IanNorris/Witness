// Append-completion positions are not exact video RAP timestamps (especially
// with multiplexed audio). Never use these positions as seek targets. Retain
// the two most recent independent append boundaries behind the reference: a
// remove can extend through dependent video frames up to the following RAP.
// An older boundary therefore protects the current decoding interval without
// assuming a fixed camera GOP length or equating native and sequence time.
export function dependencySafeTrimTarget(desired: number, reference: number,
  independentAppendEnds: readonly number[]): number {
  if (!Number.isFinite(desired) || desired <= 0 || !Number.isFinite(reference) ||
      reference < 0 || independentAppendEnds.length > 64) return 0
  let previous = -Infinity
  let protectedEnd: number | undefined
  let latest: number | undefined
  for (const end of independentAppendEnds) {
    if (!Number.isFinite(end) || end < 0 || end <= previous) return 0
    previous = end
    if (end < reference) { protectedEnd = latest; latest = end }
  }
  // No safe boundary is known yet. Quota pressure must recover via bounded
  // append/queue watchdogs, not by deleting potentially needed future frames.
  return protectedEnd === undefined ? 0 : Math.min(desired, protectedEnd)
}
