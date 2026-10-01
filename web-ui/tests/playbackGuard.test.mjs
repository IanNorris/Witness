import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

// The guard only needs ref.value; keep these tests independent of the DOM.
const source = (await readFile('src/composables/playbackGuard.ts', 'utf8'))
  .replace("import { ref } from 'vue'", 'const ref = value => ({ value })')
const output = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
const { createPlaybackGuard, blockedAudioCameras, playbackWarning, retryCameraAudio } = await import('data:text/javascript;base64,' + Buffer.from(output).toString('base64'))
const denied = () => Object.assign(new Error('autoplay denied'), { name: 'NotAllowedError' })

test('blocked audio resumes muted, logs camera and action, warns once and permits explicit retry', async () => {
  const events = []
  let calls = 0
  const video = { src: 'blob:one', muted: false, play: async () => { calls++; if (!video.muted) throw denied() } }
  const guard = createPlaybackGuard(13, () => video, (type, data) => events.push({ type, ...data }))
  await guard.play('initialBuffer')
  assert.equal(calls, 2)
  assert.equal(video.muted, true)
  assert.equal(blockedAudioCameras.value.has(13), true)
  assert.match(playbackWarning.value, /Camera 13/)
  assert.deepEqual(events.map(e => e.type), ['playRejected', 'audioBlocked', 'mutedPlaybackResumed'])
  assert.equal(events[0].reason, 'initialBuffer')
  playbackWarning.value = ''
  video.muted = false
  await guard.play('audioEnable')
  assert.equal(playbackWarning.value, '')
  retryCameraAudio(13)
  assert.equal(blockedAudioCameras.value.has(13), false)
})

test('abort and muted rejection do not disable audio or retry recursively', async () => {
  for (const [muted, name] of [[false, 'AbortError'], [true, 'NotAllowedError']]) {
    let calls = 0
    const video = { src: 'blob:two', muted, play: async () => { calls++; throw Object.assign(new Error('failed'), { name }) } }
    const guard = createPlaybackGuard(8, () => video, () => {})
    await guard.play('watchdogResume')
    assert.equal(calls, 1)
    assert.equal(blockedAudioCameras.value.has(8), false)
  }
})

test('ignores stale rejections after replacement or a newer audio choice', async () => {
  for (const change of ['source', 'audio']) {
    let reject
    const video = { src: 'blob:old', muted: false, play: () => new Promise((_, fail) => { reject = fail }) }
    const guard = createPlaybackGuard(9, () => video, () => {})
    const pending = guard.play('initialBuffer')
    const originalReject = reject
    if (change === 'source') video.src = 'blob:new'
    else guard.setAudioEnabled(false)
    originalReject(denied())
    await pending
    assert.equal(blockedAudioCameras.value.has(9), false)
  }
})

test('muted fallback failure is recorded without an infinite retry', async () => {
  let calls = 0
  const types = []
  const video = { src: 'blob:failed', muted: false, play: async () => { calls++; throw denied() } }
  await createPlaybackGuard(10, () => video, type => types.push(type)).play('audioEnable')
  assert.equal(calls, 2)
  assert.equal(types.at(-1), 'mutedPlayRejected')
})
