import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const source = await readFile('src/composables/msePlaybackVisibility.ts', 'utf8')
const output = ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText
const policy = await import('data:text/javascript;base64,' + Buffer.from(output).toString('base64'))

test('hidden documents defer playback-only recovery, visible documents allow it', () => {
  assert.equal(policy.playbackRecoveryEnabled('hidden'), false)
  assert.equal(policy.playbackRecoveryEnabled('visible'), true)
})

test('hidden buffer eviction follows the live edge even with a frozen playhead', () => {
  for (const end of [0, 10, 30, 60, 300, 1800]) {
    assert.equal(policy.playbackBufferTrimTarget(0, end, 'hidden'), Math.max(0, end - 30))
    assert.equal(policy.playbackBufferTrimTarget(0, end, 'hidden', true), Math.max(0, end - 5))
  }
  assert.equal(policy.playbackBufferTrimTarget(100, 1800, 'hidden'), 1770)
})

test('visible eviction preserves ordinary back-buffer and aggressive quota behavior', () => {
  assert.equal(policy.playbackBufferTrimTarget(100, 1800, 'visible'), 95)
  assert.equal(policy.playbackBufferTrimTarget(100, 1800, 'visible', true), 99)
  assert.equal(policy.playbackBufferTrimTarget(3, 1800, 'visible'), 0)
  for (const invalid of [NaN, Infinity, -1]) {
    assert.equal(policy.playbackBufferTrimTarget(0, invalid, 'hidden'), 0)
    assert.equal(policy.playbackBufferTrimTarget(invalid, 300, 'visible'), 0)
  }
})
