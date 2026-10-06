// Shared test-only transport. Reads saved mux output; never contacts a camera.
import { readFile, stat } from 'node:fs/promises'

export function binaryHash(buffer) {
  let hash = 0x811c9dc5
  for (const byte of buffer) hash = Math.imul((hash ^ byte) >>> 0, 0x01000193) >>> 0
  return hash.toString(16).padStart(8, '0')
}

function boxes(buffer, from = 0, to = buffer.length) {
  const result = []
  for (let p = from; p + 8 <= to;) {
    const size = buffer.readUInt32BE(p), type = buffer.toString('ascii', p + 4, p + 8)
    if (size < 8 || p + size > to) break
    result.push({ from: p, to: p + size, type }); p += size
  }
  return result
}

export function nativeReplayFragments(data, records) {
  if (!Array.isArray(records) || records.length < 2 || records.length > 20000 ||
      records[0]?.type !== 'init') throw new Error('Invalid native event capture')
  let offset = records[0].bytes
  if (!Number.isSafeInteger(offset) || offset < 8 || offset > data.length)
    throw new Error('Invalid native init size')
  const init = data.subarray(0, offset), fragments = []
  let lastVideoTime = 0, generation
  for (const record of records.slice(1)) {
    if (record.type !== 'partial' || !Number.isSafeInteger(record.bytes) || record.bytes < 8 ||
        offset + record.bytes > data.length || !Number.isSafeInteger(record.hash) ||
        record.hash < 0 || record.hash > 0xffffffff ||
        !Number.isSafeInteger(record.segmentIndex) || record.segmentIndex < 0 ||
        !Number.isSafeInteger(record.partIndex) || record.partIndex < 0 ||
        !Number.isSafeInteger(record.generation) || record.generation < 0 ||
        !Number.isFinite(record.duration) || record.duration <= 0 ||
        typeof record.independent !== 'boolean' || typeof record.keyframeSeekSafe !== 'boolean')
      throw new Error('Invalid native partial metadata')
    generation ??= record.generation
    if (record.generation !== generation) throw new Error('Mixed native generations')
    const bytes = data.subarray(offset, offset + record.bytes)
    offset += record.bytes
    const hash = binaryHash(bytes)
    if (hash !== record.hash.toString(16).padStart(8, '0')) throw new Error('Native partial hash mismatch')
    if (record.keyframeSeekSafe && init.includes(Buffer.from('mp4a')))
      throw new Error('Audio capture cannot claim safe RAP seek')
    let at = null
    for (const moof of boxes(bytes).filter(b => b.type === 'moof')) {
      for (const traf of boxes(bytes, moof.from + 8, moof.to).filter(b => b.type === 'traf')) {
        const children = boxes(bytes, traf.from + 8, traf.to)
        const tfhd = children.find(b => b.type === 'tfhd'), tfdt = children.find(b => b.type === 'tfdt')
        if (tfhd && tfdt && bytes.readUInt32BE(tfhd.from + 12) === 1 && at === null)
          at = bytes[tfdt.from + 8] === 1 ? Number(bytes.readBigUInt64BE(tfdt.from + 12)) / 90000 : bytes.readUInt32BE(tfdt.from + 12) / 90000
      }
    }
    if (at !== null) lastVideoTime = at
    fragments.push({ ...record, at: at ?? lastVideoTime, bytes, hash })
  }
  if (offset !== data.length) throw new Error('Native event capture does not cover entire file')
  return { init, fragments, generation }
}

export async function loadMuxReplay(file, seconds, metadataFile) {
  if (!Number.isFinite(seconds) || seconds < 10 || seconds > 1800) throw new Error('Replay duration must be 10..1800 seconds')
  const info = await stat(file)
  if (!info.isFile() || info.size > 512 * 1024 * 1024) throw new Error('Replay requires a local file bounded to 512 MiB')
  const data = await readFile(file)
  if (metadataFile) {
    const metadataInfo = await stat(metadataFile)
    if (!metadataInfo.isFile() || metadataInfo.size > 8 * 1024 * 1024)
      throw new Error('Native metadata exceeds bounded size')
    const records = (await readFile(metadataFile, 'utf8')).trim().split(/\r?\n/).map(line => JSON.parse(line))
    const { init, fragments, generation } = nativeReplayFragments(data, records)
    if (fragments.at(-1).at - fragments[0].at < seconds + 2)
      throw new Error('Capture is too short for requested replay')
    return { init, initHash: binaryHash(init), fragments, generation, hasAudio: init.includes(Buffer.from('mp4a')) }
  }
  const top = boxes(data)
  const first = top.find(b => b.type === 'moof')?.from
  if (first === undefined) throw new Error('No fragments in mux capture')
  const init = data.subarray(0, first), fragments = []
  let lastVideoTime = 0
  for (let i = 0; i < top.length; i++) {
    const moof = top[i]
    if (moof.type !== 'moof' || top[i + 1]?.type !== 'mdat') continue
    let at = null
    for (const traf of boxes(data, moof.from + 8, moof.to).filter(b => b.type === 'traf')) {
      const children = boxes(data, traf.from + 8, traf.to)
      const tfhd = children.find(b => b.type === 'tfhd'), tfdt = children.find(b => b.type === 'tfdt')
      if (tfhd && tfdt && data.readUInt32BE(tfhd.from + 12) === 1)
        at = data[tfdt.from + 8] === 1 ? Number(data.readBigUInt64BE(tfdt.from + 12)) / 90000 : data.readUInt32BE(tfdt.from + 12) / 90000
    }
    if (at !== null) lastVideoTime = at
    const bytes = data.subarray(moof.from, top[i + 1].to)
    fragments.push({ at: at ?? lastVideoTime, bytes, hash: binaryHash(bytes) })
  }
  if (!fragments.length || fragments.at(-1).at - fragments[0].at < seconds + 2)
    throw new Error('Capture is too short for requested replay')
  return { init, initHash: binaryHash(init), fragments, hasAudio: init.includes(Buffer.from('mp4a')) }
}

export function streamMuxReplay(socket, replay) {
  const timers = new Set()
  let closed = false
  const cancel = () => { closed = true; for (const timer of timers) clearTimeout(timer); timers.clear() }
  socket.onClose(cancel)
  const later = (fn, ms) => {
    const timer = setTimeout(() => { timers.delete(timer); if (!closed) fn() }, ms)
    timers.add(timer)
  }
  later(() => {
    socket.send(JSON.stringify({ type: 'streamSelection', stream: 'sub', codec: 'h264', width: 896, height: 512 }))
    socket.send(JSON.stringify({ type: 'initSegment', generation: replay.generation ?? 0, ...(replay.hasAudio ? { audioCodec: 'mp4a.40.2' } : {}), bytes: replay.init.length, hash: replay.initHash }))
    socket.send(replay.init)
    replay.fragments.forEach((fragment, i) => later(() => {
      socket.send(JSON.stringify({ type: 'partial', generation: fragment.generation ?? 0, segmentIndex: fragment.segmentIndex ?? Math.floor(fragment.at / 2), partIndex: fragment.partIndex ?? i,
        independent: fragment.independent ?? i === 0, keyframeSeekSafe: fragment.keyframeSeekSafe ?? false, duration: fragment.duration ?? (replay.fragments[i + 1]?.at - fragment.at || .15),
        bytes: fragment.bytes.length, hash: fragment.hash }))
      socket.send(fragment.bytes)
    }, Math.max(0, fragment.at - replay.fragments[0].at) * 1000))
  }, 200)
  return cancel
}
