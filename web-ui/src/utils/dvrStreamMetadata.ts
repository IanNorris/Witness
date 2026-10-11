import type { StreamInfo } from './streamChoice'
// Read only the MP4 sample description, not video frames. Recordings may have
// different codecs from the camera's current configuration.
export function parseDvrStreamMetadata(buffer: ArrayBuffer): StreamInfo | undefined {
  const view = new DataView(buffer)
  const type = (at: number) => String.fromCharCode(...new Uint8Array(buffer, at, 4))
  const containers = new Set(['moov', 'trak', 'mdia', 'minf', 'stbl'])
  function scan(start: number, end: number, depth: number): StreamInfo | undefined {
    if (depth > 6) return
    for (let at = start; at + 8 <= end;) {
      const size = view.getUint32(at)
      if (size < 8) return // Extended/unknown sizes are deliberately not guessed.
      const name = type(at + 4), limit = Math.min(end, at + size)
      if (name === 'stsd' && at + 24 <= limit && view.getUint32(at + 12) > 0) {
        const entry = at + 16, entrySize = view.getUint32(entry), codec = type(entry + 4)
        if (['avc1', 'avc3', 'hev1', 'hvc1', 'vp09', 'av01', 'mp4v'].includes(codec) &&
          entrySize >= 36 && entry + 36 <= limit && entry + entrySize <= limit) {
          return { tier: 'dvr', codec, width: view.getUint16(entry + 32), height: view.getUint16(entry + 34) }
        }
      }
      if (containers.has(name)) { const result = scan(at + 8, limit, depth + 1); if (result) return result }
      if (at + size > end) return
      at += size
    }
  }
  const initial = scan(0, buffer.byteLength, 0)
  if (initial) return initial
  // A bounded suffix can start inside mdat. Only accept a complete moov box,
  // then traverse its sample tables; never infer a codec from stray byte strings.
  for (let at = 0; at + 8 <= buffer.byteLength; at++) {
    if (type(at + 4) !== 'moov') continue
    const size = view.getUint32(at)
    if (size < 8 || at + size > buffer.byteLength) continue
    const result = scan(at, at + size, 0)
    if (result) return result
  }
}
const cache = new Map<number, StreamInfo>()
export async function readDvrStreamMetadata(segmentId: number, signal: AbortSignal): Promise<StreamInfo | undefined> {
  const cached = cache.get(segmentId)
  if (cached) return cached
  async function readSlice(range: string, maxBytes: number): Promise<ArrayBuffer | undefined> {
    const response = await fetch(`/dvr/segment/${segmentId}`, { credentials: 'same-origin', signal, headers: { Range: range } })
    // Never accidentally download a whole recording if the server ignores Range.
    const size = Number(response.headers.get('Content-Length'))
    if (!response.ok || !Number.isFinite(size) || size <= 0 || size > maxBytes) { await response.body?.cancel(); return }
    const bytes = await response.arrayBuffer()
    return bytes.byteLength <= maxBytes ? bytes : undefined
  }
  const prefix = await readSlice('bytes=0-16383', 16384)
  if (!prefix) return
  let metadata = parseDvrStreamMetadata(prefix)
  if (!metadata) {
    const suffix = await readSlice('bytes=-65536', 65536)
    if (suffix) metadata = parseDvrStreamMetadata(suffix)
  }
  if (metadata) {
    if (cache.size >= 128) cache.delete(cache.keys().next().value!)
    cache.set(segmentId, metadata)
  }
  return metadata
}
