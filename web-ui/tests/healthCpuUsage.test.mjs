import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const code = ts.transpileModule(await readFile('src/composables/healthCpuUsage.ts', 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText
const { formatCpuPercent } = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'))

test('CPU display preserves measured zero and rejects invalid or legacy missing values', () => {
  assert.equal(formatCpuPercent(0), '0.0%')
  assert.equal(formatCpuPercent(25.123), '25.1%')
  assert.equal(formatCpuPercent(100), '100.0%')
  for (const value of [undefined, null, NaN, Infinity, -1, 101, '25']) {
    assert.equal(formatCpuPercent(value), 'Unknown')
  }
})
test('CPU display distinguishes warmup, resets and unsupported coverage', () => {
  assert.equal(formatCpuPercent(null, 'baselineRequired'), 'Warming up')
  for (const reason of ['clockReset', 'counterReset']) assert.equal(formatCpuPercent(null, reason), 'Resetting sample')
  for (const reason of ['platformNotImplemented', 'processorGroupsUnsupported']) assert.equal(formatCpuPercent(null, reason), 'Unavailable')
  assert.equal(formatCpuPercent(null, 'collectionFailed'), 'Unknown')
})
