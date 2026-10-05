// A hidden document may keep receiving media while its decoder and timers are
// suspended by the browser. That is not evidence of a failed input connection.
export const MSE_BACK_BUFFER_SECONDS = 5
export const MSE_HIDDEN_BUFFER_SECONDS = 30

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
