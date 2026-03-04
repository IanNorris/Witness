<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useCameraStore } from '../../stores/cameras'
import { format } from 'date-fns'
import DvrPlayer from './DvrPlayer.vue'

const props = defineProps<{
  cameraIds: number[]
  from: number
  to: number
  startAt?: number
}>()

const emit = defineEmits<{
  close: []
}>()

const cameraStore = useCameraStore()
const enabledCameras = ref(new Set<number>(props.cameraIds))
const playerRefs = ref<Record<number, InstanceType<typeof DvrPlayer>>>({})

// Global time display — driven by first active player's timeupdate
const globalTimestamp = ref(props.startAt ?? props.from)
let pollTimer: ReturnType<typeof setInterval> | null = null

const activeCameras = computed(() =>
  props.cameraIds.filter(id => enabledCameras.value.has(id))
)

function toggleCamera(id: number) {
  const s = new Set(enabledCameras.value)
  if (s.has(id)) {
    if (s.size > 1) s.delete(id)
  } else {
    s.add(id)
  }
  enabledCameras.value = s
}

function cameraName(id: number): string {
  return cameraStore.cameras.find(c => c.id === id)?.name ?? `Camera ${id}`
}

function cameraColor(id: number): string {
  const hue = (id * 137) % 360
  return `hsl(${hue}, 65%, 55%)`
}

const formattedTimestamp = computed(() =>
  format(new Date(globalTimestamp.value * 1000), 'HH:mm:ss')
)

// Poll the first active player for time info
function pollTime() {
  const firstId = activeCameras.value[0]
  if (!firstId) return
  const player = playerRefs.value[firstId]
  if (!player) return
  globalTimestamp.value = player.currentTimestamp ?? props.from
}

onMounted(() => {
  pollTimer = setInterval(pollTime, 250)
})

onUnmounted(() => {
  if (pollTimer) clearInterval(pollTimer)
})
</script>

<template>
  <div class="dvr-multi">
    <div class="dvr-multi-header">
      <span class="dvr-multi-title">📹 DVR · {{ formattedTimestamp }}</span>
      <div class="dvr-cam-toggles">
        <button
          v-for="id in cameraIds"
          :key="id"
          class="dvr-cam-toggle"
          :class="{ active: enabledCameras.has(id) }"
          :style="{ borderColor: enabledCameras.has(id) ? cameraColor(id) : 'transparent' }"
          @click="toggleCamera(id)"
          :title="cameraName(id)"
        >{{ cameraName(id) }}</button>
      </div>
      <button class="dvr-close" @click="emit('close')" title="Close">✕</button>
    </div>
    <div class="dvr-carousel">
      <DvrPlayer
        v-for="id in activeCameras"
        :key="id"
        :ref="(el: any) => { if (el) playerRefs[id] = el }"
        :camera-id="id"
        :from="from"
        :to="to"
        :start-at="startAt"
        :camera-name="cameraName(id)"
        :compact="true"
        @close="toggleCamera(id)"
      />
    </div>
  </div>
</template>

<style scoped>
.dvr-multi {
  margin-top: 0.5rem;
  border: 1px solid var(--bs-border-color, #444);
  border-radius: 0.5rem;
  background: rgba(0,0,0,0.6);
  overflow: hidden;
}

.dvr-multi-header {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  padding: 0.6rem 1rem;
  background: rgba(255,255,255,0.05);
  border-bottom: 1px solid var(--bs-border-color, #333);
}

.dvr-multi-title {
  font-size: 1rem;
  font-weight: 500;
  color: rgba(255,255,255,0.85);
  white-space: nowrap;
}

.dvr-cam-toggles {
  display: flex;
  flex-wrap: wrap;
  gap: 3px;
  flex: 1;
}

.dvr-cam-toggle {
  background: rgba(255,255,255,0.06);
  border: 2px solid transparent;
  color: rgba(255,255,255,0.35);
  font-size: 0.8rem;
  padding: 3px 10px;
  border-radius: 4px;
  cursor: pointer;
  transition: all 0.15s;
}
.dvr-cam-toggle.active {
  color: rgba(255,255,255,0.85);
  background: rgba(255,255,255,0.1);
}
.dvr-cam-toggle:hover {
  color: rgba(255,255,255,0.7);
}

.dvr-close {
  background: none;
  border: none;
  color: rgba(255,255,255,0.5);
  font-size: 1.2rem;
  cursor: pointer;
  padding: 0 0.5rem;
  flex-shrink: 0;
}
.dvr-close:hover {
  color: rgba(255,255,255,0.9);
}

.dvr-carousel {
  display: flex;
  overflow-x: auto;
  scroll-snap-type: x proximity;
  scrollbar-width: thin;
  scrollbar-color: rgba(255,255,255,0.15) transparent;
}
.dvr-carousel > :deep(.dvr-player) {
  flex: 0 0 300px;
  min-width: 250px;
  max-width: 400px;
  scroll-snap-align: start;
}
</style>
