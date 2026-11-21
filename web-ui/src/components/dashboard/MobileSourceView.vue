<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import CameraCard from '../camera/CameraCard.vue'
import { useCameraStore } from '../../stores/cameras'
import { useLocalStorage } from '../../composables/useLocalStorage'

// Phones get a dashboard's cameras as an ordered list (no canvas layout).
// Only cards near the viewport hold a live stream; the rest show refreshed
// snapshots. "Snapshots only" is a per-device data saver.
const props = defineProps<{ cameraIds: number[]; canReorder?: boolean }>()
const emit = defineEmits<{ reorder: [order: number[]] }>()

const cameraStore = useCameraStore()
const router = useRouter()
const snapshotsOnly = useLocalStorage('witness-mobile-snapshots-only', false)
const cameras = computed(() => props.cameraIds.map(id => cameraStore.getCameraById(id)).filter(c => c !== undefined))
const nearViewport = ref(new Set<number>())
const tick = ref(0)
const customizing = ref(false)
const draftOrder = ref<number[]>([])
let observer: IntersectionObserver | null = null
let snapshotTimer: ReturnType<typeof setInterval> | null = null

function observe() {
  observer?.disconnect()
  if (typeof IntersectionObserver === 'undefined') {
    nearViewport.value = new Set(props.cameraIds)
    return
  }
  observer = new IntersectionObserver(entries => {
    const next = new Set(nearViewport.value)
    for (const entry of entries) {
      const id = Number((entry.target as HTMLElement).dataset.cameraId)
      if (entry.isIntersecting) next.add(id); else next.delete(id)
    }
    nearViewport.value = next
  }, { rootMargin: '200px 0px' })
  document.querySelectorAll<HTMLElement>('.mobile-source[data-camera-id]').forEach(el => observer!.observe(el))
}
watch(() => props.cameraIds.join(','), async () => { await nextTick(); observe() })

function startCustomize() {
  draftOrder.value = [...props.cameraIds]
  customizing.value = true
}
function move(index: number, direction: number) {
  const order = [...draftOrder.value], next = index + direction
  if (next < 0 || next >= order.length) return
  ;[order[index], order[next]] = [order[next]!, order[index]!]
  draftOrder.value = order
}
function saveOrder() {
  emit('reorder', draftOrder.value)
  customizing.value = false
}

onMounted(async () => {
  await nextTick()
  observe()
  snapshotTimer = setInterval(() => { tick.value++ }, 3000)
})
onUnmounted(() => {
  observer?.disconnect()
  if (snapshotTimer) clearInterval(snapshotTimer)
})
</script>

<template>
  <div class="mobile-sources">
    <div class="mobile-sources-bar">
      <label class="form-check form-switch mb-0">
        <input v-model="snapshotsOnly" class="form-check-input" type="checkbox">
        <span class="form-check-label small">Snapshots only (saves data)</span>
      </label>
      <button v-if="canReorder" class="btn btn-sm btn-outline-secondary" type="button" @click="startCustomize">Reorder</button>
    </div>

    <div v-if="cameras.length === 0" class="ui-empty-state"><h2>No cameras</h2><p>This dashboard has no cameras available to you.</p></div>

    <div v-for="camera in cameras" :key="camera.id" class="mobile-source" :data-camera-id="camera.id">
      <CameraCard v-if="!snapshotsOnly && nearViewport.has(camera.id)" :camera="camera"
        @open-stream="router.push(`/live/${$event}`)" @open-clips="router.push(`/clips/${$event}`)" />
      <button v-else class="mobile-snapshot" type="button" @click="router.push(`/live/${camera.id}`)">
        <img :src="`${camera.previewUrl}?t=${tick}`" :alt="camera.name" loading="lazy">
        <span class="mobile-snapshot-name"><span class="dot" :class="{ ok: camera.status === 'Connected' }" />{{ camera.name }}</span>
      </button>
    </div>

    <div v-if="customizing" class="mobile-sheet" role="dialog" aria-label="Reorder cameras for phones">
      <div class="mobile-sheet-panel">
        <h2>Order on phones</h2>
        <ol>
          <li v-for="(id, index) in draftOrder" :key="id">
            <span>{{ cameraStore.getCameraById(id)?.name ?? id }}</span>
            <button type="button" :disabled="index === 0" :aria-label="`Move ${cameraStore.getCameraById(id)?.name} up`" @click="move(index, -1)">↑</button>
            <button type="button" :disabled="index === draftOrder.length - 1" :aria-label="`Move ${cameraStore.getCameraById(id)?.name} down`" @click="move(index, 1)">↓</button>
          </li>
        </ol>
        <div class="d-flex gap-2 justify-content-end">
          <button class="btn btn-sm btn-outline-secondary" type="button" @click="customizing = false">Cancel</button>
          <button class="btn btn-sm btn-primary" type="button" @click="saveOrder">Save order</button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.mobile-sources { display: flex; flex-direction: column; gap: .75rem; }
.mobile-sources-bar { display: flex; align-items: center; justify-content: space-between; gap: .5rem; flex-wrap: wrap; }
.mobile-source { min-height: 120px; }
.mobile-snapshot { position: relative; display: block; width: 100%; aspect-ratio: 16 / 9; padding: 0; border: 1px solid #354459; border-radius: .7rem; overflow: hidden; background: #000; }
.mobile-snapshot img { width: 100%; height: 100%; object-fit: contain; }
.mobile-snapshot-name { position: absolute; left: 8px; bottom: 8px; display: inline-flex; align-items: center; gap: .4rem; padding: 2px 8px; border-radius: 6px; background: rgba(8, 12, 18, .72); color: #edf3fc; font-size: .8rem; font-weight: 600; }
.dot { width: .45rem; height: .45rem; border-radius: 50%; background: #e05a62; }
.dot.ok { background: #4bc77a; }
.mobile-sheet { position: fixed; inset: 0; z-index: 2000; background: rgba(0, 0, 0, .55); display: flex; align-items: flex-end; }
.mobile-sheet-panel { width: 100%; max-height: 80dvh; overflow: auto; padding: 1rem 16px calc(1rem + env(safe-area-inset-bottom)); background: #141c28; border-radius: 1rem 1rem 0 0; }
.mobile-sheet-panel h2 { font-size: 1rem; }
.mobile-sheet-panel ol { padding-left: 1.25rem; }
.mobile-sheet-panel li { display: flex; align-items: center; gap: .4rem; padding: .3rem 0; }
.mobile-sheet-panel li span { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mobile-sheet-panel li button { min-width: 2.5rem; min-height: 2.25rem; border: 1px solid #3a4b61; border-radius: .4rem; background: #1b2738; color: #e8f3ff; }
</style>
