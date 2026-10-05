import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const source = await readFile(new URL('../src/composables/msePlaybackVisibility.ts', import.meta.url), 'utf8')
const output = ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText
const policy = await import('data:text/javascript;base64,' + Buffer.from(output).toString('base64'))

test('hidden documents defer playback-only recovery, visible documents allow it', () => {
  assert.equal(policy.playbackRecoveryEnabled('hidden'), false)
  assert.equal(policy.playbackRecoveryEnabled('visible'), true)
})

const ranges = rows => ({length:rows.length,start:i=>rows[i][0],end:i=>rows[i][1]})
test('buffer position distinguishes real gaps, playable headroom and exhausted tails', () => {
  const buffered = ranges([[1,3],[5,9]])
  assert.deepEqual(policy.playbackBufferPosition(buffered,2), {
    bufferedRangeCount:2,playheadBuffered:true,playheadGapMs:0,headroomInCurrentRangeMs:1000,
  })
  assert.deepEqual(policy.playbackBufferPosition(buffered,3), {
    bufferedRangeCount:2,playheadBuffered:false,nextBufferedStartSeconds:5,playheadGapMs:2000,headroomInCurrentRangeMs:0,
  })
  assert.deepEqual(policy.playbackBufferPosition(buffered,9), {
    bufferedRangeCount:2,playheadBuffered:false,headroomInCurrentRangeMs:0,
  })
  assert.equal(policy.playbackBufferPosition(buffered,0).playheadGapMs,1000)
  assert.equal(policy.playbackBufferPosition(buffered,5).playheadBuffered,true)
  assert.deepEqual(policy.playbackBufferPosition(ranges([]),0), {
    bufferedRangeCount:0,playheadBuffered:false,headroomInCurrentRangeMs:0,
  })
})
test('invalid or excessive ranges report unknown position without inventing a gap', () => {
  for (const rows of [[[2,1]],[[0,2],[1,3]],[[0,Infinity]]])
    assert.deepEqual(policy.playbackBufferPosition(ranges(rows),1),{bufferedRangeCount:rows.length})
  assert.deepEqual(policy.playbackBufferPosition({length:65,start:()=>{throw Error('must not scan')}},1),{bufferedRangeCount:65})
  assert.deepEqual(policy.playbackBufferPosition({length:1,start:()=>{throw Error('range disappeared')}},1),{bufferedRangeCount:1})
  assert.deepEqual(policy.playbackBufferPosition(undefined,1),{})
  assert.deepEqual(policy.playbackBufferPosition(ranges([[0,2]]),NaN),{})
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
