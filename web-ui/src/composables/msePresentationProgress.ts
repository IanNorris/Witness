// A multiplexed stream's media clock can advance while no new video is shown.
// Only actual presented-frame callbacks supply progress; unsupported browsers
// leave it unknown and retain the existing clock/transport watchdogs.
export function createPresentationProgress() {
  let mediaTime: number | undefined
  let frames = 0
  let stalledMs = 0
  return {
    get mediaTime() { return mediaTime },
    get frames() { return frames },
    get stalledMs() { return stalledMs },
    reset() { mediaTime = undefined; frames = 0; stalledMs = 0 },
    presented(time: number) {
      if (!Number.isFinite(time) || time < 0) return
      frames++
      if (mediaTime === undefined || Math.abs(time - mediaTime) > .001) {
        mediaTime = time
        stalledMs = 0
      }
    },
    poll(elapsedMs: number, currentTime: number, eligible: boolean): number {
      if (!eligible || mediaTime === undefined || !Number.isFinite(currentTime) ||
          currentTime - mediaTime <= 1 || !Number.isFinite(elapsedMs) || elapsedMs < 0) {
        stalledMs = 0
        return 0
      }
      // Same scheduling grace as the production watchdog: suspension does not
      // count as a stall, but repeated delayed ticks retain runnable progress.
      stalledMs += elapsedMs > 1000 ? 250 : elapsedMs
      return stalledMs
    },
  }
}

// Hit testing is only needed when presentation falls behind the clock. Own
// controls belong to the viewport; an external covering dialog does not. Unlike
// IntersectionObserver v2 visibility, this does not reject ordinary overlays
// or embedded browser rendering as "visually compromised".
export function presentationViewportVisible(viewport: Element, doc: Document,
  width: number, height: number): boolean {
  if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0 ||
      typeof doc.elementFromPoint !== 'function') return false
  const rect = viewport.getBoundingClientRect()
  const left = Math.max(0, rect.left), top = Math.max(0, rect.top)
  const right = Math.min(width, rect.right), bottom = Math.min(height, rect.bottom)
  if (![left, top, right, bottom].every(Number.isFinite) || right <= left || bottom <= top) return false
  return [[.5,.5],[.25,.25],[.75,.25],[.25,.75],[.75,.75]].some(([x,y]) => {
    const hit = doc.elementFromPoint(left + (right-left)*x!, top + (bottom-top)*y!)
    return hit !== null && viewport.contains(hit)
  })
}
