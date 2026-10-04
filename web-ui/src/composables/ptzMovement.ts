// A stop must follow the acknowledgement of its corresponding move. Never
// retarget a queued command by consulting a subsequently selected camera.
export function createPtzMovement(
  send: (cameraId: number, command: string, speed: number) => Promise<unknown>,
  changed: (moving: boolean) => void,
  failed: (command: string, error: unknown) => void,
) {
  type Gesture = { cameraId: number; command: string; speed: number; sent: boolean }
  let gesture: Gesture | null = null
  let disposed = false
  let tail = Promise.resolve()
  let deadline: ReturnType<typeof setTimeout> | null = null
  let stopFailed = false
  function enqueue(work: () => Promise<void>) {
    tail = tail.then(work).catch(() => {})
  }
  function stop() {
    if (deadline) clearTimeout(deadline)
    deadline = null
    const previous = gesture
    gesture = null
    changed(false)
    if (previous) enqueue(async () => {
      if (!previous.sent) return
      try { await send(previous.cameraId, 'stop', previous.speed); stopFailed = false }
      catch (error) { stopFailed = true; failed('stop', error) }
    })
  }
  function start(cameraId: number, command: string, speed: number) {
    if (disposed) return
    stop()
    const next: Gesture = { cameraId, command, speed, sent: false }
    gesture = next
    changed(true)
    enqueue(async () => {
      if (gesture !== next || disposed) return // released/replaced before dispatch
      next.sent = true
      try { await send(next.cameraId, next.command, next.speed) }
      catch (error) { failed(next.command, error); if (gesture === next) stop() }
    })
    // Lost pointer-up events must not leave a movement held indefinitely.
    // This bounds the browser gesture, not firmware/network execution time.
    deadline = setTimeout(stop, 10000)
  }
  function emergencyStop(cameraId: number, speed: number) {
    const held = gesture?.cameraId
    stop()
    if (held !== cameraId) enqueue(async () => {
      try { await send(cameraId, 'stop', speed); stopFailed = false }
      catch (error) { stopFailed = true; failed('stop', error) }
    })
  }
  return {
    start, stop, emergencyStop,
    position(work: () => Promise<unknown>): Promise<boolean> {
      stop()
      return new Promise(resolve => enqueue(async () => {
        let success = false
        try {
          if (disposed) return
          if (stopFailed) throw new Error('Stop has not been confirmed; retry Stop before moving to a saved position')
          await work()
          success = true
        } catch (error) { failed('position', error) }
        finally { resolve(success) }
      }))
    },
    dispose() { stop(); disposed = true },
    async settled() {
      // A failed move can append its safety stop while we await the move.
      let observed: Promise<void>
      do { observed = tail; await observed } while (observed !== tail)
    },
  }
}
