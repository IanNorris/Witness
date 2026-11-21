<script setup lang="ts">
import { computed } from 'vue'
import { useCameraStore } from '../../../stores/cameras'

// Summary of the viewer's accessible cameras. The camera store only holds
// cameras the server returned for this user, so nothing leaks here.
const props = defineProps<{ variant: 'summary' | 'health'; cameraIds: number[] }>()
const cameraStore = useCameraStore()
const cameras = computed(() => props.cameraIds.map(id => cameraStore.getCameraById(id)).filter(c => c !== undefined))
const online = computed(() => cameras.value.filter(c => c.status === 'Connected').length)
const recording = computed(() => cameras.value.filter(c => c.isRecording).length)
const motion = computed(() => cameras.value.filter(c => c.motionActive).length)
</script>

<template>
  <div class="status-tile">
    <template v-if="variant === 'summary'">
      <div class="metric"><strong>{{ online }}<span>/{{ cameras.length }}</span></strong><small>Online</small></div>
      <div class="metric"><strong :class="{ hot: recording > 0 }">{{ recording }}</strong><small>Recording</small></div>
      <div class="metric"><strong :class="{ warm: motion > 0 }">{{ motion }}</strong><small>Motion</small></div>
    </template>
    <ul v-else class="health-list">
      <li v-for="camera in cameras" :key="camera.id">
        <span class="dot" :class="camera.status === 'Connected' ? 'ok' : 'bad'" aria-hidden="true" />
        <span class="name">{{ camera.name }}</span>
        <span class="state">{{ camera.isRecording ? 'REC' : camera.motionActive ? 'Motion' : camera.status }}</span>
      </li>
    </ul>
  </div>
</template>

<style scoped>
.status-tile { container-type: size; width: 100%; height: 100%; display: flex; align-items: center; justify-content: space-around; gap: .5rem; padding: .5rem; background: #0b1119; color: var(--witness-ink, #edf3fc); overflow: hidden; }
.metric { display: flex; flex-direction: column; align-items: center; }
.metric strong { font-size: clamp(1rem, 30cqh, 3rem); font-weight: 650; font-variant-numeric: tabular-nums; }
.metric strong span { font-size: .55em; color: var(--witness-muted, #a5b6cc); }
.metric small { font-size: .7rem; color: var(--witness-muted, #a5b6cc); text-transform: uppercase; letter-spacing: .08em; }
.hot { color: #ff8a8f; }
.warm { color: #f5c26b; }
.health-list { list-style: none; margin: 0; padding: 0; width: 100%; height: 100%; overflow: auto; font-size: .75rem; }
.health-list li { display: flex; align-items: center; gap: .45rem; padding: .2rem 0; border-bottom: 1px solid #1c2735; }
.dot { width: .5rem; height: .5rem; border-radius: 50%; flex: none; }
.ok { background: #4bc77a; }
.bad { background: #e05a62; }
.name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.state { color: var(--witness-muted, #a5b6cc); }
</style>
