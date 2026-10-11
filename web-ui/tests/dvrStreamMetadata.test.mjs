import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
const source = await readFile('src/utils/dvrStreamMetadata.ts', 'utf8')
const output = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
const { parseDvrStreamMetadata, readDvrStreamMetadata } = await import('data:text/javascript;base64,' + Buffer.from(output).toString('base64'))
const box = (type, payload) => { const data = Buffer.alloc(8 + payload.length); data.writeUInt32BE(data.length); data.write(type, 4); payload.copy(data, 8); return data }
function metadata(codec) {
  const entry = Buffer.alloc(78); entry.writeUInt16BE(3840, 24); entry.writeUInt16BE(2160, 26)
  const header = Buffer.alloc(8); header.writeUInt32BE(1, 4)
  let data = box('stsd', Buffer.concat([header, box(codec, entry)]))
  for (const type of ['stbl', 'minf', 'mdia', 'trak', 'moov']) data = box(type, data)
  return Uint8Array.from(data).buffer
}
test('reads real MP4 sample entries, not camera configuration or stray strings', () => {
  for (const codec of ['hev1', 'hvc1', 'avc1', 'avc3']) assert.deepEqual(parseDvrStreamMetadata(metadata(codec)), { tier: 'dvr', codec, width: 3840, height: 2160 })
  assert.equal(parseDvrStreamMetadata(new ArrayBuffer(3)), undefined)
  assert.equal(parseDvrStreamMetadata(Uint8Array.from(Buffer.from('random hev1 avc1 text')).buffer), undefined)
  assert.equal(parseDvrStreamMetadata(metadata('mp4a')), undefined)
  assert.equal(parseDvrStreamMetadata(Uint8Array.from(Buffer.concat([Buffer.alloc(512, 123), Buffer.from(metadata('hev1'))])).buffer).codec, 'hev1', 'bounded suffix can locate a complete trailing moov')
})
test('authenticated prefix reads are bounded and missing Range support cannot download a recording', async () => {
  const calls = []; let cancelled = 0
  const original = globalThis.fetch
  try {
    globalThis.fetch = async (...args) => { calls.push(args); return { ok: true, headers: { get: () => '10000000' }, body: { cancel: async () => cancelled++ }, arrayBuffer: () => assert.fail('read whole recording') } }
    assert.equal(await readDvrStreamMetadata(1, new AbortController().signal), undefined)
    assert.equal(cancelled, 1)
    assert.equal(calls[0][1].credentials, 'same-origin')
    assert.equal(calls[0][1].headers.Range, 'bytes=0-16383')
    globalThis.fetch = async () => ({ ok: true, headers: { get: () => String(metadata('hev1').byteLength) }, arrayBuffer: async () => metadata('hev1') })
    assert.equal((await readDvrStreamMetadata(2, new AbortController().signal)).codec, 'hev1')
  } finally { globalThis.fetch = original }
})
