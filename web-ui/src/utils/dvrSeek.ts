// One cancellable operation for initial loads, segment changes and in-file seeks.
// Never expose an undecoded frame just because a fallback timer expired.
export function waitForDvrSeek(video: HTMLVideoElement, offset: number,
  ready: () => void, failed: (reason: string) => void, timeoutMs = 10000): () => void {
  let done = false
  let applied = false
  const events = ['loadedmetadata', 'loadeddata', 'canplay', 'seeked']
  const cleanup = () => {
    events.forEach(type => video.removeEventListener(type, check))
    video.removeEventListener('error', onError)
    clearTimeout(timeout)
  }
  const fail = (reason: string) => { if (done) return; done = true; cleanup(); failed(reason) }
  const onError = () => fail('mediaError')
  const check = () => {
    if (done) return
    if (video.error) { onError(); return }
    if (!applied && video.readyState >= 1) {
      applied = true
      try { video.currentTime = offset } catch { fail('seekFailed'); return }
    }
    if (applied && !video.seeking && video.readyState >= 2 && Math.abs(video.currentTime - offset) < 0.5) {
      done = true; cleanup(); ready()
    }
  }
  const timeout = setTimeout(() => fail('seekTimeout'), timeoutMs)
  events.forEach(type => video.addEventListener(type, check))
  video.addEventListener('error', onError)
  // Defer so the caller can assign src/load before examining the old resource.
  queueMicrotask(check)
  return () => { done = true; cleanup() }
}
