<script lang="ts">
// Bound parallel work and cache size, including unavailable crops. Grouping still
// compares the original scene, not these display-only detection crops.
const cache = new Map<string, Promise<string | null>>()
let active = 0
const queue: Array<() => void> = []
async function limited<T>(work: () => Promise<T>): Promise<T> {
  if (active >= 2) await new Promise<void>(resolve => queue.push(resolve))
  else active++
  try { return await work() } finally {
    const next = queue.shift()
    if (next) next() // Transfer the reserved slot, rather than releasing it twice.
    else active--
  }
}
</script>
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import type { Clip } from '../../types/clip'
import { activityCrop } from '../../utils/activityThumbnail'
const props = defineProps<{ clip: Clip; src: string; focus: boolean }>()
const image = ref<HTMLImageElement | null>(null)
const crop = ref<string | null>(null)
const visible = ref(false)
let generation = 0
let observer: IntersectionObserver | undefined
const url = computed(() => props.focus && crop.value ? crop.value : props.src)
watch(() => [props.clip.uid, props.clip.timestamp, props.clip.duration, props.clip.detectionVersion, props.focus, visible.value], async () => {
  const current = ++generation
  crop.value = null
  if (!props.focus || !visible.value || props.clip.recordMode === 'Capture') return
  const clip = props.clip
  const key = `${clip.camera}:${clip.uid}:${clip.timestamp}:${clip.duration}:${clip.detectionVersion}`
  if (!cache.has(key)) {
    if (cache.size >= 200) cache.delete(cache.keys().next().value!)
    cache.set(key, limited(async () => {
      try {
        const response = await fetch(`/api/detection/${clip.camera}?from=${clip.timestamp}&to=${clip.timestamp + Math.min(Math.max(clip.duration, 0), 30)}`, { credentials: 'same-origin', signal: AbortSignal.timeout(5000) })
        if (!response.ok) return null
        const data = await response.json()
        return Array.isArray(data.frames) ? activityCrop(clip.camera, data.frames) : null
      } catch { return null }
    }))
  }
  const result = await cache.get(key)
  if (generation === current) crop.value = result ?? null
}, { immediate: true })
onMounted(() => {
  if (typeof IntersectionObserver === 'undefined') { visible.value = true; return }
  observer = new IntersectionObserver(entries => { visible.value = entries.some(entry => entry.isIntersecting) })
  if (image.value) observer.observe(image.value)
})
onUnmounted(() => { generation++; observer?.disconnect() })
</script>
<template>
  <img ref="image" :src="url" :alt="`Clip ${clip.uid}`" loading="lazy" :title="focus && crop ? 'Detected object — turn off Object focus for the full scene' : 'Full scene'" @error="crop = null" />
  <span v-if="focus && crop" class="object-crop-label">Object</span>
</template>
<style scoped>
.object-crop-label { position: absolute; bottom: 2.4rem; right: 3px; color: white; background: #102032cc; font-size: .6rem; padding: 1px 4px; border-radius: 3px; pointer-events: none; }
</style>
