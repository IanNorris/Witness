// Snapshot undo/redo for the dashboard editor. One entry per completed
// gesture; rapid inspector edits sharing a coalesce key within the window
// collapse into one entry. Import-free for node:test.

export class EditorHistory<T> {
  private past: string[] = []
  private future: string[] = []
  private lastKey: string | null = null
  private lastAt = 0

  private current: string
  private readonly limit: number
  private readonly coalesceMs: number

  constructor(current: string, limit = 100, coalesceMs = 800) {
    this.current = current
    this.limit = limit
    this.coalesceMs = coalesceMs
  }

  get canUndo() { return this.past.length > 0 }
  get canRedo() { return this.future.length > 0 }

  // Records `next` as the new current state. Returns false if unchanged.
  push(next: T, coalesceKey: string | null = null, now = Date.now()): boolean {
    const serialised = JSON.stringify(next)
    if (serialised === this.current) return false
    const coalesce = coalesceKey !== null && coalesceKey === this.lastKey && now - this.lastAt < this.coalesceMs && this.past.length > 0
    if (!coalesce) {
      this.past.push(this.current)
      if (this.past.length > this.limit) this.past.shift()
    }
    this.current = serialised
    this.future = []
    this.lastKey = coalesceKey
    this.lastAt = now
    return true
  }

  undo(): T | null {
    const previous = this.past.pop()
    if (previous === undefined) return null
    this.future.push(this.current)
    this.current = previous
    this.lastKey = null
    return JSON.parse(previous) as T
  }

  redo(): T | null {
    const next = this.future.pop()
    if (next === undefined) return null
    this.past.push(this.current)
    this.current = next
    this.lastKey = null
    return JSON.parse(next) as T
  }

  // Replaces history entirely (after load, save-as-copy or reload).
  reset(state: T) {
    this.past = []
    this.future = []
    this.current = JSON.stringify(state)
    this.lastKey = null
  }

  isAt(state: T) { return JSON.stringify(state) === this.current }
}
