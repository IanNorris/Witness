import { test } from 'node:test'
import assert from 'node:assert/strict'
import { streamMuxReplay, binaryHash, loadMuxReplay, nativeReplayFragments } from './fixtures/muxReplay.mjs'

test('native replay preserves producer boundaries and verifies every byte', () => {
  const init = Buffer.from('init0000'), bytes = Buffer.from('fragment0000')
  const data = Buffer.concat([init, bytes])
  const records = [{ type: 'init', bytes: init.length }, { type: 'partial', bytes: bytes.length,
    hash: parseInt(binaryHash(bytes), 16), segmentIndex: 8, partIndex: 0, generation: 1,
    duration: .15, independent: true, keyframeSeekSafe: true }]
  const replay = nativeReplayFragments(data, records)
  assert.deepEqual(replay.init, init)
  assert.deepEqual(replay.fragments[0].bytes, bytes)
  assert.equal(replay.fragments[0].keyframeSeekSafe, true)
  assert.equal(replay.fragments[0].partIndex, 0)
  assert.equal(replay.fragments[0].generation, 1)
  assert.throws(() => nativeReplayFragments(data, [records[0], {...records[1], hash: 0}]), /hash mismatch/)
  assert.throws(() => nativeReplayFragments(Buffer.concat([data, Buffer.from('extra')]), records), /entire file/)
  assert.throws(() => nativeReplayFragments(data, [records[0], {...records[1], bytes: 100}]), /metadata/)
  assert.throws(() => nativeReplayFragments(data, [records[0], {...records[1], keyframeSeekSafe: 'true'}]), /metadata/)
  assert.throws(() => nativeReplayFragments(Buffer.concat([data, bytes]), [records[0], records[1], {...records[1], generation: 2}]), /Mixed native generations/)
  assert.throws(() => nativeReplayFragments(Buffer.concat([Buffer.from('initmp4a'), bytes]), records), /Audio/)
})

test('native replay transport forwards actual safety and segment identity', async () => {
  const init = Buffer.from('init0000'), bytes = Buffer.from('fragment0000')
  const sent = []
  const cancel = streamMuxReplay({send: value => sent.push(value), onClose: () => {}}, {
    init, initHash: binaryHash(init), hasAudio: false, generation: 1,
    fragments: [{at: 0, bytes, hash: binaryHash(bytes), independent: true,
      keyframeSeekSafe: true, segmentIndex: 7, partIndex: 0, generation: 1, duration: .3}]
  })
  try {
    await new Promise(done => setTimeout(done, 300))
    const partial = JSON.parse(sent[3])
    assert.equal(JSON.parse(sent[1]).generation, 1)
    assert.equal(partial.generation, 1)
    assert.equal(partial.keyframeSeekSafe, true)
    assert.equal(partial.segmentIndex, 7)
    assert.equal(partial.partIndex, 0)
    assert.equal(partial.duration, .3)
  } finally { cancel() }
})

test('mux replay validates duration before opening input', async () => {
  for (const duration of [NaN, 0, 9, 1801])
    await assert.rejects(loadMuxReplay('does-not-exist', duration), /duration/)
})

test('mux replay sends verified init and fragments, preserving audio-only entries', async () => {
  const init = Buffer.from('fixture mp4a'), video = Buffer.from('video'), audio = Buffer.from('audio')
  const replay = { init, initHash: binaryHash(init), hasAudio: true,
    fragments: [{ at: 0, bytes: video, hash: binaryHash(video) }, { at: .01, bytes: audio, hash: binaryHash(audio) }] }
  const sent = [], closeHandlers = []
  const cancel = streamMuxReplay({ send: value => sent.push(value), onClose: cb => closeHandlers.push(cb) }, replay)
  try {
    await new Promise(done => setTimeout(done, 300))
    assert.equal(JSON.parse(sent[0]).stream, 'sub')
    assert.equal(JSON.parse(sent[1]).audioCodec, 'mp4a.40.2')
    assert.equal(JSON.parse(sent[1]).hash, binaryHash(init))
    assert.deepEqual(sent.filter(Buffer.isBuffer), [init, video, audio])
    assert.equal(JSON.parse(sent[3]).independent, true)
    assert.equal(JSON.parse(sent[5]).independent, false)
  } finally { cancel() }
})

test('closed replay cancels pending startup without sending stale media', async () => {
  const sent = [], handlers = []
  const cancel = streamMuxReplay({ send: value => sent.push(value), onClose: cb => handlers.push(cb) }, {})
  handlers[0]()
  await new Promise(done => setTimeout(done, 250))
  assert.deepEqual(sent, [])
  cancel()
})
