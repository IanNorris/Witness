import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const source = await readFile(new URL('../src/composables/mseEviction.ts', import.meta.url), 'utf8')
const output = ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText
const { dependencySafeTrimTarget: trim } = await import('data:text/javascript;base64,' + Buffer.from(output).toString('base64'))

test('eviction protects a prior independent interval instead of assuming fixed GOP length', () => {
  assert.equal(trim(6, 7, [.15, 5.15, 10.15]), .15)
  assert.equal(trim(2, 7, [.15, 5.15, 10.15]), .15)
  assert.equal(trim(99, 100, [.15, 60.15, 120.15]), .15)
  assert.equal(trim(11, 12, [.15, 5.15, 10.15]), 5.15)
  assert.equal(trim(3, 12, [.15, 5.15, 10.15]), 3)
})

test('unknown, invalid and insufficient independent boundaries do not permit unsafe eviction', () => {
  for (const boundaries of [[], [.15], [.15, 10], [NaN, 1], [1, 1], [2, 1], [-1, 2], [1, Infinity], Array.from({length:65},(_,i)=>i)])
    assert.equal(trim(6, 7, boundaries), 0)
  assert.equal(trim(0, 7, [.15, 5.15]), 0)
  assert.equal(trim(Infinity, 7, [.15, 5.15]), 0)
  assert.equal(trim(6, NaN, [.15, 5.15]), 0)
})

test('hidden eviction references the live edge rather than a frozen playhead', () => {
  const boundaries = Array.from({length:64},(_,i)=>i+100)
  assert.equal(trim(170-30, 170, boundaries), 140)
  assert.equal(trim(170-5, 170, boundaries), 162)
})
