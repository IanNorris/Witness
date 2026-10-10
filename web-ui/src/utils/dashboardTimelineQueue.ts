// All tiles share a small request budget. A slow HTTP worker must not be
// amplified by a dashboard full of independent polling loops.
let active = 0
const waiting: (() => void)[] = []
export async function withTimelineBudget<T>(operation: () => Promise<T>): Promise<T> {
  if (active >= 2) await new Promise<void>(resolve => waiting.push(resolve))
  else ++active
  try { return await operation() }
  finally {
    const next = waiting.shift()
    if (next) next()
    else --active
  }
}
