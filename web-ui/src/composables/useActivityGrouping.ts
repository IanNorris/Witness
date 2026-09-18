import type { Clip } from '../types/clip'

export interface ActivityGroup {
  id: string
  representative: Clip
  clips: Clip[]
  maximumDistance: number
}

interface ActivityDescriptor {
  camera: number
  timestamp: number
  lighting: number
  tags: string
  regime: 'dark' | 'monochrome' | 'colour' | 'unavailable'
  phash: Uint8Array
  dhash: Uint8Array
  blockHash: Uint8Array
  grid: Float32Array
}

interface GroupOptions {
  threshold?: number
  maxGapSeconds?: number
  maxSpanSeconds?: number
  maxGroupSize?: number
  maxCandidates?: number
}

const descriptorCache = new Map<number, Promise<ActivityDescriptor>>()
const cosine = Array.from({ length: 8 }, (_, frequency) =>
  Float32Array.from({ length: 32 }, (_, position) =>
    Math.cos(((2 * position + 1) * frequency * Math.PI) / 64)
  )
)

function median(values: number[]) {
  const ordered = [...values].sort((a, b) => a - b)
  const middle = Math.floor(ordered.length / 2)
  return ordered.length % 2
    ? ordered[middle]!
    : (ordered[middle - 1]! + ordered[middle]!) / 2
}

function packBits(bits: boolean[]) {
  const packed = new Uint8Array(Math.ceil(bits.length / 8))
  bits.forEach((bit, index) => {
    if (bit) packed[index >> 3] = packed[index >> 3]! | (1 << (index & 7))
  })
  return packed
}

function hamming(left: Uint8Array, right: Uint8Array) {
  let different = 0
  for (let index = 0; index < left.length; index++) {
    let value = left[index]! ^ right[index]!
    while (value) {
      value &= value - 1
      different++
    }
  }
  return different / (left.length * 8)
}

function pixelLuma(data: Uint8ClampedArray, index: number) {
  return 0.2126 * data[index]! + 0.7152 * data[index + 1]! + 0.0722 * data[index + 2]!
}

function normalisedTags(clip: Clip) {
  const visualTags = (clip.tags ?? '')
    .split(/[;,]/)
    .map(tag => tag.trim().toLowerCase())
    .filter(Boolean)
  const audioTags = [...new Set((clip.audioEvents ?? []).map(event => `audio:${event.group.toLowerCase()}`))]
  return [...visualTags, ...audioTags].sort().join(';')
}

async function extractDescriptor(clip: Clip, thumbnailUrl: (clip: Clip) => string) {
  const response = await fetch(thumbnailUrl(clip), { credentials: 'same-origin' })
  if (!response.ok) throw new Error(`thumbnail request failed (${response.status})`)
  const bitmap = await createImageBitmap(await response.blob())
  try {
    const canvas = document.createElement('canvas')
    canvas.width = 32
    canvas.height = 32
    const context = canvas.getContext('2d', { willReadFrequently: true })
    if (!context) throw new Error('canvas context unavailable')
    context.drawImage(bitmap, 0, 0, 32, 32)
    const pixels = context.getImageData(0, 0, 32, 32).data
    const gray = new Float32Array(32 * 32)
    let lumaTotal = 0
    let colourTotal = 0
    for (let pixel = 0; pixel < gray.length; pixel++) {
      const offset = pixel * 4
      const luma = pixelLuma(pixels, offset)
      gray[pixel] = luma
      lumaTotal += luma
      colourTotal += Math.max(pixels[offset]!, pixels[offset + 1]!, pixels[offset + 2]!)
        - Math.min(pixels[offset]!, pixels[offset + 1]!, pixels[offset + 2]!)
    }

    const coefficients: number[] = []
    for (let vertical = 0; vertical < 8; vertical++) {
      for (let horizontal = 0; horizontal < 8; horizontal++) {
        let value = 0
        for (let y = 0; y < 32; y++) {
          const cy = cosine[vertical]![y]!
          for (let x = 0; x < 32; x++) {
            value += gray[y * 32 + x]! * cosine[horizontal]![x]! * cy
          }
        }
        coefficients.push(value)
      }
    }
    const phashMedian = median(coefficients.slice(1))
    const phash = packBits(coefficients.map((value, index) => index !== 0 && value > phashMedian))

    const sample = (x: number, y: number) => gray[Math.min(31, y) * 32 + Math.min(31, x)]!
    const dhashBits: boolean[] = []
    for (let y = 0; y < 8; y++) {
      for (let x = 0; x < 8; x++) {
        dhashBits.push(sample((x + 1) * 3, y * 4 + 2) > sample(x * 3, y * 4 + 2))
      }
    }

    const blocks: number[] = []
    for (let y = 0; y < 16; y++) {
      for (let x = 0; x < 16; x++) {
        const base = y * 2 * 32 + x * 2
        blocks.push((gray[base]! + gray[base + 1]! + gray[base + 32]! + gray[base + 33]!) / 4)
      }
    }
    const blockMedian = median(blocks)

    const gridValues: number[] = []
    for (let y = 0; y < 8; y++) {
      for (let x = 0; x < 8; x++) {
        let value = 0
        for (let yy = 0; yy < 4; yy++) {
          for (let xx = 0; xx < 4; xx++) value += sample(x * 4 + xx, y * 4 + yy)
        }
        gridValues.push(value / 16)
      }
    }
    const gridMean = gridValues.reduce((sum, value) => sum + value, 0) / gridValues.length
    const variance = gridValues.reduce((sum, value) => sum + (value - gridMean) ** 2, 0) / gridValues.length
    const gridScale = Math.max(Math.sqrt(variance), 8)
    const grid = Float32Array.from(gridValues, value =>
      Math.max(-3, Math.min(3, (value - gridMean) / gridScale))
    )

    const meanLuma = lumaTotal / gray.length
    const colourfulness = colourTotal / gray.length
    return {
      camera: clip.camera,
      timestamp: clip.timestamp,
      lighting: clip.lighting,
      tags: normalisedTags(clip),
      regime: meanLuma < 35 ? 'dark' : colourfulness < 10 ? 'monochrome' : 'colour',
      phash,
      dhash: packBits(dhashBits),
      blockHash: packBits(blocks.map(value => value > blockMedian)),
      grid,
    } satisfies ActivityDescriptor
  } finally {
    bitmap.close()
  }
}

function descriptorFor(clip: Clip, thumbnailUrl: (clip: Clip) => string) {
  let pending = descriptorCache.get(clip.uid)
  if (!pending) {
    pending = extractDescriptor(clip, thumbnailUrl)
    descriptorCache.set(clip.uid, pending)
  }
  // Classification metadata can arrive after the thumbnail descriptor is
  // cached. Refresh compatibility fields without re-decoding the image.
  return pending.then(descriptor => ({
    ...descriptor,
    camera: clip.camera,
    timestamp: clip.timestamp,
    lighting: clip.lighting,
    tags: normalisedTags(clip),
  }))
}

function distance(left: ActivityDescriptor, right: ActivityDescriptor) {
  let gridDistance = 0
  for (let index = 0; index < left.grid.length; index++) {
    gridDistance += Math.abs(left.grid[index]! - right.grid[index]!)
  }
  gridDistance /= left.grid.length * 3
  return 0.47 * hamming(left.phash, right.phash)
    + 0.18 * hamming(left.dhash, right.dhash)
    + 0.18 * hamming(left.blockHash, right.blockHash)
    + 0.17 * gridDistance
}

function compatible(left: ActivityDescriptor, right: ActivityDescriptor) {
  return left.regime !== 'unavailable'
    && right.regime !== 'unavailable'
    && left.camera === right.camera
    && left.regime === right.regime
    && left.lighting === right.lighting
    && left.tags === right.tags
}

export async function buildActivityGroups(
  clips: Clip[],
  thumbnailUrl: (clip: Clip) => string,
  options: GroupOptions = {},
): Promise<ActivityGroup[]> {
  const threshold = options.threshold ?? 0.17
  const maxGap = options.maxGapSeconds ?? 60
  const maxSpan = options.maxSpanSeconds ?? 600
  const maxSize = options.maxGroupSize ?? 50
  const maxCandidates = options.maxCandidates ?? 8
  const chronological = [...clips].sort((left, right) => left.timestamp - right.timestamp)
  const groups: Array<ActivityGroup & { descriptor: ActivityDescriptor }> = []

  // Thumbnail fetches and bitmap decoding can overlap. Keep the actual grouping
  // pass deterministic by consuming the settled results in chronological order.
  const descriptors = await Promise.allSettled(
    chronological.map(clip => descriptorFor(clip, thumbnailUrl))
  )

  for (let clipIndex = 0; clipIndex < chronological.length; clipIndex++) {
    const clip = chronological[clipIndex]!
    let descriptor: ActivityDescriptor
    const descriptorResult = descriptors[clipIndex]!
    if (descriptorResult.status === 'rejected') {
      groups.push({ id: `clip-${clip.uid}`, representative: clip, clips: [clip], maximumDistance: 0,
        descriptor: { camera: clip.camera, timestamp: clip.timestamp, lighting: clip.lighting,
          tags: normalisedTags(clip), regime: 'unavailable', phash: new Uint8Array(8),
          dhash: new Uint8Array(8), blockHash: new Uint8Array(32), grid: new Float32Array(64) } })
      continue
    }
    descriptor = descriptorResult.value

    const candidates = [...groups].reverse().filter(group =>
      group.representative.camera === clip.camera
      && clip.timestamp - group.clips[group.clips.length - 1]!.timestamp <= maxGap
      && clip.timestamp - group.clips[0]!.timestamp <= maxSpan
      && group.clips.length < maxSize
      && compatible(group.descriptor, descriptor)
    ).slice(0, maxCandidates)

    let best: { group: typeof groups[number], distance: number } | null = null
    for (const group of candidates) {
      const candidateDistance = distance(group.descriptor, descriptor)
      if (!best || candidateDistance < best.distance) best = { group, distance: candidateDistance }
    }
    if (best && best.distance <= threshold) {
      best.group.clips.push(clip)
      best.group.maximumDistance = Math.max(best.group.maximumDistance, best.distance)
    } else {
      groups.push({ id: `activity-${clip.uid}`, representative: clip, clips: [clip], maximumDistance: 0, descriptor })
    }
  }

  // The strip is long-lived, so prevent an unbounded cache as new clips arrive.
  if (descriptorCache.size > 250) {
    const retained = new Set(clips.map(clip => clip.uid))
    for (const uid of descriptorCache.keys()) {
      if (!retained.has(uid)) descriptorCache.delete(uid)
    }
  }

  return groups
    .sort((left, right) => right.clips[right.clips.length - 1]!.timestamp
      - left.clips[left.clips.length - 1]!.timestamp)
    .map(({ descriptor: _, ...group }) => group)
}
