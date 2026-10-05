// A hidden document may keep receiving media while its decoder and timers are
// suspended by the browser. That is not evidence of a failed input connection.
export const MSE_BACK_BUFFER_SECONDS = 5
export const MSE_HIDDEN_BUFFER_SECONDS = 30

// Passive evidence only: do not seek, merge ranges or infer individual audio /
// video track readiness from the media element's intersected buffered ranges.
export function playbackBufferPosition(buffered: TimeRanges | undefined, currentTime: number): {
  bufferedRangeCount?: number; playheadBuffered?: boolean; nextBufferedStartSeconds?: number;
  playheadGapMs?: number; headroomInCurrentRangeMs?: number;
} {
  if (!buffered || !Number.isFinite(currentTime)) return {}
  const count = buffered.length
  if (!Number.isInteger(count) || count < 0) return {}
  if (count > 64) return { bufferedRangeCount: count } // Unknown position, not a false gap.
  const ranges: number[][] = []
  try {
    for (let i = 0; i < count; i++) {
      const start = buffered.start(i), end = buffered.end(i)
      if (!Number.isFinite(start) || !Number.isFinite(end) || end < start ||
          (i > 0 && start < ranges[i - 1]![1]!)) return { bufferedRangeCount: count }
      ranges.push([start, end])
    }
  } catch { return { bufferedRangeCount: count } }
  for (const [start, end] of ranges) {
    if (currentTime >= start! && currentTime < end!) return {
      bufferedRangeCount: count, playheadBuffered: true, playheadGapMs: 0,
      headroomInCurrentRangeMs: (end! - currentTime) * 1000,
    }
    if (currentTime < start!) return {
      bufferedRangeCount: count, playheadBuffered: false, nextBufferedStartSeconds: start,
      playheadGapMs: (start! - currentTime) * 1000, headroomInCurrentRangeMs: 0,
    }
  }
  return { bufferedRangeCount: count, playheadBuffered: false, headroomInCurrentRangeMs: 0 }
}

export function playbackRecoveryEnabled(visibility: string): boolean {
  return visibility !== 'hidden'
}

export function playbackBufferTrimTarget(currentTime: number, bufferedEnd: number,
  visibility: string, aggressive = false): number {
  const reference = visibility === 'hidden' ? bufferedEnd : currentTime
  if (!Number.isFinite(reference) || reference < 0) return 0
  const retained = visibility === 'hidden'
    ? (aggressive ? MSE_BACK_BUFFER_SECONDS : MSE_HIDDEN_BUFFER_SECONDS)
    : (aggressive ? 1 : MSE_BACK_BUFFER_SECONDS)
  return Math.max(0, reference - retained)
}
