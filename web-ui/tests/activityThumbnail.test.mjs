import assert from 'node:assert/strict'
import test from 'node:test'
import { readFile } from 'node:fs/promises'
import ts from 'typescript'
const source = ts.transpileModule(await readFile('src/utils/activityThumbnail.ts', 'utf8'), { compilerOptions: { module: ts.ModuleKind.ESNext } }).outputText
const { activityCrop } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'))
test('object focus chooses confident non-baseline evidence, scoped to the camera', () => {
  assert.equal(activityCrop(8, [{ boxes: [
    { crop: '8/baseline.jpg', conf: .99, baseline: true },
    { crop: '9/wrong-camera.jpg', conf: .99 },
    { crop: '8/low.jpg', conf: .4 },
    { crop: '8/object.jpg', conf: .9 },
    { crop: '8/other.jpg', conf: .7 },
  ] }]), '/api/detection/crop/8/object.jpg')
})
test('missing or unsafe crops keep the original scene', () => {
  for (const crop of [undefined, 'https://example.com/x', '8/../9/x', '8/x?y', 'C:/x']) {
    assert.equal(activityCrop(8, [{ boxes: [{ crop, conf: .9 }] }]), null)
  }
  assert.equal(activityCrop(8, []), null)
  assert.equal(activityCrop(8, [{ boxes: [{ crop: 'X:\\WitnessCache\\crops\\8\\object.jpg', conf: .9 }] }]), '/api/detection/crop/8/object.jpg')
})
