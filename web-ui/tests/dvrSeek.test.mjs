import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
const source = await readFile('src/utils/dvrSeek.ts', 'utf8')
const code = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext } }).outputText
const { waitForDvrSeek } = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'))
class Video extends EventTarget {
  readyState = 0; currentTime = 0; seeking = false; error = null
  event(type) { this.dispatchEvent(new Event(type)) }
}
test('waits for metadata, target seek and decoded frame, even when paused', async () => {
  const video = new Video(); let ready = 0
  waitForDvrSeek(video, 12, () => ready++, assert.fail)
  await Promise.resolve(); assert.equal(ready, 0)
  video.readyState = 1; video.event('loadedmetadata')
  assert.equal(video.currentTime, 12); assert.equal(ready, 0)
  video.seeking = true; video.readyState = 3; video.event('canplay')
  assert.equal(ready, 0)
  video.seeking = false; video.event('seeked'); video.event('canplay')
  assert.equal(ready, 1)
})
test('error and timeout fail once without presenting a frame', async () => {
  for (const reason of ['mediaError', 'seekTimeout']) {
    const video = new Video(); const failures = []
    waitForDvrSeek(video, 10, assert.fail, r => failures.push(r), 10)
    if (reason === 'mediaError') { video.error = { code: 4 }; video.event('error') }
    await new Promise(resolve => setTimeout(resolve, 25))
    assert.deepEqual(failures, [reason])
    video.readyState = 3; video.event('canplay')
    assert.deepEqual(failures, [reason])
  }
})
test('superseded and unmounted operations cannot complete or time out', async () => {
  const video = new Video()
  const cancel = waitForDvrSeek(video, 10, assert.fail, assert.fail, 10)
  cancel(); video.readyState = 3; video.event('canplay')
  await new Promise(resolve => setTimeout(resolve, 25))
})
