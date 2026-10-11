import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
const source = await readFile('src/utils/streamChoice.ts', 'utf8')
const output = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
const policy = await import('data:text/javascript;base64,' + Buffer.from(output).toString('base64'))

test('recording errors distinguish unsupported playback from ambiguous decode failure', () => {
  const unsupported = policy.recordingPlaybackError(4, 'hev1')
  assert.match(unsupported, /H.265 \/ HEVC/)
  assert.match(unsupported, /may be unavailable or unsupported/)
  assert.match(unsupported, /metadata or delivery/)
  assert.match(unsupported, /viewing machine/)
  assert.doesNotMatch(unsupported, /Try Edge|exhausted/)
  assert.doesNotMatch(policy.recordingPlaybackError(4), /HEVC/)
  assert.match(policy.recordingPlaybackError(3, 'avc1'), /Damaged media or unavailable decoder resources/)
  assert.doesNotMatch(policy.recordingPlaybackError(2, 'hev1'), /unsupported/)
})
test('codec aliases, stream metadata and unknown values are explicit', () => {
  for (const codec of ['hevc', 'HEVC', 'h265', 'hev1.1.6.L93.B0', 'hvc1.1.6.L93.B0']) assert.equal(policy.codecLabel(codec), 'H.265 / HEVC')
  for (const codec of ['h264', 'avc1.42001e', 'avc3']) assert.equal(policy.isH264(codec), true)
  assert.equal(policy.isH264('hevc'), false)
  assert.equal(policy.validStreamChoice('broken'), 'auto')
  assert.equal(policy.validStreamChoice('main'), 'main')
  const camera = { codec: 'hevc', width: 3840, height: 2160, subCodec: 'h264', subWidth: 640, subHeight: 360 }
  assert.match(policy.streamSummary(policy.cameraStreamInfo(camera, 'sub')), /Sub · H.264 \/ AVC · 640×360 · Frame rate not reported/)
  assert.match(policy.streamSummary({ tier: 'dvr' }), /DVR recording · Codec not reported/)
})
