export function openHealthMonitor(): boolean {
  const features = 'popup=yes,width=1500,height=900,resizable=yes,scrollbars=yes'
  const monitor = window.open('/health-monitor', 'witness-health-monitor', features)
    ?? window.open('/health-monitor', '_blank')
  monitor?.focus()
  return monitor !== null
}
