<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { api } from '../../../composables/useApi'
import { useCameraStore } from '../../../stores/cameras'

// Clip activity over the last N hours, one lane per camera. The timeline
// endpoint is access-filtered server-side; we narrow further to the scope.
const props = defineProps<{ hours: number; cameraIds: number[] }>()
const cameraStore = useCameraStore()
interface Bucket { from: number; to: number; clipCount: number; cameraIDs: number[] }
const buckets = ref<Bucket[]>([])
const range = ref({ from: 0, to: 1 })
let timer: ReturnType<typeof setInterval> | undefined

async function load() {
  const to = Math.floor(Date.now() / 1000), from = to - props.hours * 3600
  try {
    const data = await api<{ events: Bucket[] }>(`/clip/timeline/${from}/${to}`, { redirectOnFail: false })
    buckets.value = data?.events ?? []
    range.value = { from, to }
  } catch { /* keep the previous picture */ }
}
const lanes = computed(() => props.cameraIds.map(id => ({
  id,
  name: cameraStore.getCameraById(id)?.name ?? '',
  marks: buckets.value.filter(b => b.cameraIDs.includes(id)).map(b => ({
    left: (b.from - range.value.from) / (range.value.to - range.value.from) * 100,
    width: Math.max(0.4, (b.to - b.from) / (range.value.to - range.value.from) * 100),
  })),
})))
watch(() => props.hours, load)
onMounted(() => { void load(); timer = setInterval(load, 60000) })
onUnmounted(() => { if (timer) clearInterval(timer) })
</script>

<template>
  <div class="timeline-tile">
    <div class="timeline-head"><span>Last {{ hours }} h</span><span>now</span></div>
    <div v-for="lane in lanes" :key="lane.id" class="lane">
      <span class="lane-name">{{ lane.name }}</span>
      <div class="lane-track"><i v-for="(mark, i) in lane.marks" :key="i" :style="{ left: `${mark.left}%`, width: `${mark.width}%` }" /></div>
    </div>
    <p v-if="lanes.length === 0" class="empty">No cameras in scope</p>
  </div>
</template>

<style scoped>
.timeline-tile { width: 100%; height: 100%; padding: .5rem .65rem; background: #0b1119; color: var(--witness-ink, #edf3fc); overflow: auto; font-size: .72rem; }
.timeline-head { display: flex; justify-content: space-between; color: var(--witness-muted, #a5b6cc); margin-bottom: .35rem; padding-left: 7rem; }
.lane { display: flex; align-items: center; gap: .5rem; height: 1.15rem; }
.lane-name { width: 6.5rem; flex: none; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: #c7d6e6; }
.lane-track { position: relative; flex: 1; height: .5rem; border-radius: 99px; background: #172231; }
.lane-track i { position: absolute; top: 0; bottom: 0; border-radius: 99px; background: #79beef; }
.empty { color: var(--witness-muted, #a5b6cc); margin: 0; }
</style>
