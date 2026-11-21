<script setup lang="ts">
import { computed } from 'vue'
import { formatDistanceToNow } from 'date-fns'
import { useClipStore } from '../../../stores/clips'
import { useCameraStore } from '../../../stores/cameras'
import type { Clip } from '../../../types/clip'

// Most recent tagged clip (person, vehicle, ...) in scope. Clips come from
// /clip/recent, which the server already filters by the viewer's access.
const props = defineProps<{ cameraIds: number[] }>()
const emit = defineEmits<{ play: [clip: Clip] }>()
const clipStore = useClipStore()
const cameraStore = useCameraStore()
const latest = computed(() => [...clipStore.recentClips]
  .filter(clip => props.cameraIds.includes(clip.camera) && clip.tags)
  .sort((a, b) => b.timestamp - a.timestamp)[0] ?? null)
const thumb = computed(() => {
  const clip = latest.value
  if (!clip) return ''
  return clip.recordMode === 'Capture' ? `/clip/captured/${clip.uid}/thumbnail` : clipStore.thumbnailUrl(clip.camera, clip.timestamp)
})
const tags = computed(() => (latest.value?.tags ?? '').split(/[;,]/).map(t => t.trim()).filter(Boolean).slice(0, 3))
</script>

<template>
  <button v-if="latest" class="detection-tile" type="button" @click="emit('play', latest)">
    <img :src="thumb" alt="" loading="lazy">
    <span class="detection-caption">
      <strong>{{ tags.join(' · ') || 'Activity' }}</strong>
      <small>{{ cameraStore.getCameraById(latest.camera)?.name }} · {{ formatDistanceToNow(latest.timestamp * 1000, { addSuffix: true }) }}</small>
    </span>
  </button>
  <div v-else class="detection-tile empty">No recent detections</div>
</template>

<style scoped>
.detection-tile { position: relative; width: 100%; height: 100%; padding: 0; border: 0; background: #000; color: var(--witness-ink, #edf3fc); overflow: hidden; text-align: left; }
.detection-tile img { width: 100%; height: 100%; object-fit: contain; }
.detection-caption { position: absolute; left: 8px; bottom: 8px; right: 8px; display: flex; flex-direction: column; padding: .3rem .5rem; border-radius: 6px; background: rgba(8, 12, 18, .72); }
.detection-caption strong { font-size: .8rem; text-transform: capitalize; }
.detection-caption small { font-size: .68rem; color: #b9c8d8; }
.empty { display: flex; align-items: center; justify-content: center; background: #0b1119; color: var(--witness-muted, #a5b6cc); font-size: .8rem; }
</style>
