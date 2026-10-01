// Bounded, allowlisted playback evidence. Never export media URLs or arbitrary
// diagnostic payloads received from another browser tab.
const numericFields = ['currentTime', 'bufferedEnd', 'lagMs', 'playbackRate', 'readyState',
  'generation', 'sampleIntervalMs', 'playbackAdvanceMs', 'bufferAdvanceMs', 'lagChangeMs',
  'fragmentAgeMs', 'appendAgeMs', 'appendQueueLength', 'appendQueueBytes',
  'appendQueueOldestAgeMs', 'appendOperationAgeMs', 'safeKeyframes', 'futureSafeKeyframes',
  'highLatencyMs', 'schedulingDelayMs', 'maxFragmentGapMs', 'fragmentsSinceSample',
  'maxAppendDurationMs', 'lastAppendDurationMs', 'lastBufferAdvanceMs', 'lastPartialDurationMs',
  'segmentIndex', 'partIndex', 'timestampOffsetSeconds', 'volume', 'videoFrames', 'droppedVideoFrames'] as const
const booleanFields = ['paused', 'seeking', 'catchUpActive', 'hasAudio', 'sourceBufferUpdating',
  'partialIndependent', 'partialSeekSafe', 'muted'] as const
export type DriftSample = Partial<Record<typeof numericFields[number], number>> &
  Partial<Record<typeof booleanFields[number], boolean>> & {
    t: string
    visibility?: string
    buffered?: number[][]
    sourceBuffered?: number[][]
    sourceBufferMode?: string
  }

export function sanitizeDriftSample(value: unknown): DriftSample | undefined {
  if (!value || typeof value !== 'object') return undefined
  const source = value as Record<string, unknown>
  if (typeof source.t !== 'string' || source.t.length > 40 || !Number.isFinite(Date.parse(source.t))) return undefined
  const result: DriftSample = { t: source.t }
  for (const field of numericFields) {
    const value = source[field]
    if (typeof value === 'number' && Number.isFinite(value)) result[field] = value
  }
  for (const field of booleanFields) if (typeof source[field] === 'boolean') result[field] = source[field]
  if (source.visibility === 'visible' || source.visibility === 'hidden') result.visibility = source.visibility
  if (source.sourceBufferMode === 'sequence' || source.sourceBufferMode === 'segments') result.sourceBufferMode = source.sourceBufferMode
  for (const field of ['buffered', 'sourceBuffered'] as const) {
    if (Array.isArray(source[field])) result[field] = source[field].slice(-8).filter(
      (range): range is number[] => Array.isArray(range) && range.length === 2 &&
        range.every(value => typeof value === 'number' && Number.isFinite(value)) && range[1] >= range[0],
    ).map(range => [...range])
  }
  return result
}

export function sanitizeDriftSamples(value: unknown): DriftSample[] | undefined {
  if (!Array.isArray(value)) return undefined
  return value.slice(-60).map(sanitizeDriftSample).filter((sample): sample is DriftSample => sample !== undefined)
}
