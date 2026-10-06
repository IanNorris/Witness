import { test } from 'node:test'
import assert from 'node:assert/strict'
import { streamMuxReplay, binaryHash, loadMuxReplay, nativeReplayFragments, muxReplayProfile } from './fixtures/muxReplay.mjs'

const h264Profile = { videoCodec: 'h264', width: 896, height: 512, audioCodec: '' }
function box(type, ...payload) {
  const body = Buffer.concat(payload), header = Buffer.alloc(8)
  header.writeUInt32BE(body.length + 8); header.write(type, 4, 'ascii')
  return Buffer.concat([header, body])
}
function track(kind, type, width = 1920, height = 1080) {
  const handler = Buffer.alloc(12); handler.write(kind, 8, 'ascii')
  const entry = Buffer.alloc(kind === 'vide' ? 78 : 28)
  if (kind === 'vide') { entry.writeUInt16BE(width, 24); entry.writeUInt16BE(height, 26) }
  const sample = box(type, entry, ...(kind === 'vide' ? [box(type.startsWith('avc') ? 'avcC' : 'hvcC', Buffer.from([1]))] : []))
  const count = Buffer.alloc(8); count.writeUInt32BE(1, 4)
  return box('trak', box('mdia', box('hdlr', handler), box('minf', box('stbl', box('stsd', count, sample)))))
}

test('replay profiles come from structured init tracks, not codec-looking payload bytes', () => {
  for (const [entry, videoCodec] of [['avc1', 'h264'], ['avc3', 'h264'], ['hvc1', 'hevc'], ['hev1', 'hevc']]) {
    const init = box('moov', track('vide', entry, 3840, 2160), box('udta', Buffer.from('mp4a h264')))
    assert.deepEqual(muxReplayProfile(init), { videoCodec, width: 3840, height: 2160, audioCodec: '' })
    assert.deepEqual(muxReplayProfile(box('moov', track('vide', entry), track('soun', 'mp4a'))),
      { videoCodec, width: 1920, height: 1080, audioCodec: 'aac' })
  }
  assert.throws(() => muxReplayProfile(Buffer.from('hvc1 mp4a')), /exactly one video/)
  assert.throws(() => muxReplayProfile(box('moov', track('soun', 'mp4a'))), /exactly one video/)
  assert.throws(() => muxReplayProfile(box('moov', track('vide', 'hvc1'), track('vide', 'avc1'))), /exactly one video/)
  assert.throws(() => muxReplayProfile(box('moov', track('vide', 'avc1', 0))), /dimensions/)
  assert.throws(() => muxReplayProfile(box('moov', track('vide', 'av01'))), /Unsupported/)
  assert.throws(() => muxReplayProfile(box('moov', track('vide', 'hvc1'), track('soun', 'Opus'))), /Unsupported/)
  const malformed = box('moov', track('vide', 'hvc1'))
  const configurationType = malformed.indexOf(Buffer.from('hvcC'))
  malformed.write('xxxx', configurationType, 'ascii')
  assert.throws(() => muxReplayProfile(malformed), /codec configuration/)
})

test('native replay preserves producer boundaries and verifies every byte', () => {
  const init = box('moov', track('vide', 'avc1', 896, 512)), bytes = Buffer.from('fragment0000')
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
  assert.deepEqual(replay.profile, h264Profile)
  assert.throws(() => nativeReplayFragments(data, [records[0], {...records[1], hash: 0}]), /hash mismatch/)
  assert.throws(() => nativeReplayFragments(Buffer.concat([data, Buffer.from('extra')]), records), /entire file/)
  assert.throws(() => nativeReplayFragments(data, [records[0], {...records[1], bytes: 100}]), /metadata/)
  assert.throws(() => nativeReplayFragments(data, [records[0], {...records[1], keyframeSeekSafe: 'true'}]), /metadata/)
  assert.throws(() => nativeReplayFragments(Buffer.concat([data, bytes]), [records[0], records[1], {...records[1], generation: 2}]), /Mixed native generations/)
  const audioInit = box('moov', track('vide', 'avc1'), track('soun', 'mp4a'))
  assert.throws(() => nativeReplayFragments(Buffer.concat([audioInit, bytes]),
    [{ type: 'init', bytes: audioInit.length }, records[1]]), /Audio/)
  const silentInit = box('moov', track('vide', 'hvc1'), box('udta', Buffer.from('mp4a')))
  assert.equal(nativeReplayFragments(Buffer.concat([silentInit, bytes]),
    [{ type: 'init', bytes: silentInit.length }, records[1]]).profile.audioCodec, '')
})

test('native replay transport forwards actual safety and segment identity', async () => {
  const init = Buffer.from('init0000'), bytes = Buffer.from('fragment0000')
  const sent = []
  const cancel = streamMuxReplay({send: value => sent.push(value), onClose: () => {}}, {
    init, initHash: binaryHash(init), hasAudio: false, generation: 1, profile: h264Profile,
    fragments: [{at: 0, bytes, hash: binaryHash(bytes), independent: true,
      keyframeSeekSafe: true, segmentIndex: 7, partIndex: 0, generation: 1, duration: .3}]
  })
  try {
    await new Promise(done => setTimeout(done, 300))
    const partial = JSON.parse(sent[3])
    assert.deepEqual(JSON.parse(sent[0]), { type: 'streamSelection', stream: 'sub', codec: 'h264', width: 896, height: 512 })
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
  const replay = { init, initHash: binaryHash(init), hasAudio: true, profile: { ...h264Profile, audioCodec: 'aac' },
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
  const cancel = streamMuxReplay({ send: value => sent.push(value), onClose: cb => handlers.push(cb) }, { profile: h264Profile })
  handlers[0]()
  await new Promise(done => setTimeout(done, 250))
  assert.deepEqual(sent, [])
  cancel()
})

test('HEVC replay advertises captured codec and dimensions instead of the old H.264 defaults', async () => {
  const sent = [], init = Buffer.from('unit init')
  const cancel = streamMuxReplay({ send: value => sent.push(value), onClose: () => {} }, {
    init, initHash: binaryHash(init), hasAudio: false, fragments: [],
    profile: { videoCodec: 'hevc', width: 3840, height: 2160, audioCodec: '' },
  })
  try {
    await new Promise(done => setTimeout(done, 300))
    assert.deepEqual(JSON.parse(sent[0]), { type: 'streamSelection', stream: 'sub', codec: 'hevc', width: 3840, height: 2160 })
  } finally { cancel() }
  assert.throws(() => streamMuxReplay({ send: () => {}, onClose: () => {} }, {}), /captured track profile/)
})
