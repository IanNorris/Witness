export function formatCpuPercent(value?: number | null, reason?: string): string {
  if (typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 100) {
    return `${value.toFixed(1)}%`
  }
  if (reason === 'baselineRequired') return 'Warming up'
  if (reason === 'clockReset' || reason === 'counterReset') return 'Resetting sample'
  if (reason === 'platformNotImplemented' || reason === 'processorGroupsUnsupported') return 'Unavailable'
  return 'Unknown'
}
