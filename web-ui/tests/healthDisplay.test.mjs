import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const code = ts.transpileModule(await readFile('src/composables/healthDisplay.ts', 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText
const { formatHealthTimestamp, formatHealthBuild } = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'))

test('missing or malformed historical metadata is visibly unknown', () => {
  for (const value of [undefined, null, '', ' ', 'not a date', {}, [], 0, NaN]) {
    assert.equal(formatHealthTimestamp(value), 'Unknown')
    assert.equal(formatHealthTimestamp(value, true), 'Unknown')
  }
  for (const value of [undefined, null, '', ' ', {}, [], 0]) {
    assert.equal(formatHealthBuild(value), 'Unknown')
  }
})

test('valid timestamps retain browser-local date/time formatting', () => {
  const value = '2026-10-05T04:43:53.188Z'
  assert.equal(formatHealthTimestamp(value), new Date(value).toLocaleString())
  assert.equal(formatHealthTimestamp(value, true), new Date(value).toLocaleTimeString())
  assert.equal(formatHealthBuild('6c558bdd9d9e'), '6c558bdd9d9e')
})

test('health table routes persisted metadata through the guarded formatters', async () => {
  const component = await readFile('src/components/admin/HealthDashboard.vue', 'utf8')
  assert.ok(component.includes('formatHealthTimestamp(session.sampledAtUtc, true)'))
  assert.ok(component.includes('formatHealthBuild(session.buildHash)'))
  assert.ok(!component.includes('new Date(session.sampledAtUtc)'))
  assert.ok(component.includes('v-if="player.lastError"'))
})
