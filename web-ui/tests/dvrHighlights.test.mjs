import assert from 'node:assert/strict'
import { test } from 'node:test'
import { buildHighlights, isVideoActivityAt, nextHighlight } from '../src/utils/dvrHighlights.ts'

test('manual seeks stay in highlights only within a real video event', () => {
  const clips = [{ from: 10, to: 13 }, { from: 40, to: 43 }]
  for (const at of [10, 12.99, 40, 42.99]) assert.equal(isVideoActivityAt(clips, at), true)
  for (const at of [8, 9.99, 13, 15, 25, 38, 43, NaN, Infinity])
    assert.equal(isVideoActivityAt(clips, at), false)
  assert.equal(isVideoActivityAt([], 10), false)
  assert.equal(isVideoActivityAt([{ from: NaN, to: 12 }, { from: 10, to: Infinity }], 11), false)
  assert.equal(isVideoActivityAt([{ from: 12, to: 10 }], 11), false)
})

test('clips get lead-in/tail, merge overlaps, and stay within the selected hour', () => {
  assert.deepEqual(buildHighlights([{ id: 8, ranges: [{ from: 0, to: 100 }],
    clips: [{ from: 1, to: 10 }, { from: 11, to: 20 }, { from: 95, to: 110 }] }], 0, 100), [
    { from: 0, to: 22, cameraId: 8 }, { from: 93, to: 100, cameraId: 8 },
  ])
})

test('only recorded activity is played; coverage gaps switch the clock driver', () => {
  const highlights = buildHighlights([
    { id: 8, clips: [{ from: 10, to: 40 }], ranges: [{ from: 0, to: 20 }, { from: 35, to: 50 }] },
    { id: 9, clips: [], ranges: [{ from: 20, to: 30 }] },
  ], 0, 100)
  assert.deepEqual(highlights, [
    { from: 8, to: 20, cameraId: 8 }, { from: 20, to: 30, cameraId: 9 }, { from: 35, to: 42, cameraId: 8 },
  ])
  assert.equal(nextHighlight(highlights, 20)?.cameraId, 9)
  assert.equal(nextHighlight(highlights, 31)?.from, 35)
  assert.equal(nextHighlight(highlights, 42), undefined)
})

test('no clips, uncovered activity and invalid ranges cannot create highlights', () => {
  assert.deepEqual(buildHighlights([{ id: 1, ranges: [], clips: [{ from: 5, to: 10 }] }], 0, 20), [])
  assert.deepEqual(buildHighlights([{ id: 1, ranges: [{ from: 0, to: 20 }], clips: [] }], 0, 20), [])
  assert.deepEqual(buildHighlights([{ id: 1, ranges: [{ from: 0, to: 20 }],
    clips: [{ from: NaN, to: 10 }, { from: 50, to: 60 }, { from: 5, to: 4 }] }], 0, 20), [])
})
