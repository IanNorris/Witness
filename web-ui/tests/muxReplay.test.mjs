import { test } from 'node:test'
import assert from 'node:assert/strict'
import { streamMuxReplay, binaryHash, loadMuxReplay } from './fixtures/muxReplay.mjs'

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
