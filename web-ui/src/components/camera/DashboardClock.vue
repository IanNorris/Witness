<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useAuthStore } from '../../stores/auth'
const auth = useAuthStore()
const tick = ref(performance.now())
let timer: ReturnType<typeof setInterval> | undefined
const display = computed(() => {
  const elapsed = tick.value
  const clock = auth.serverClock
  if (!clock) return new Date().toLocaleTimeString()
  return new Date(clock.unixMs + elapsed - clock.receivedAt + clock.offsetMinutes * 60000).toISOString().slice(11, 19)
})
onMounted(() => {
  timer = setInterval(() => { tick.value = performance.now() }, 1000)
})
onUnmounted(() => { if (timer) clearInterval(timer) })
</script>
<template>
  <time class="dashboard-clock" :title="auth.serverClock ? 'Server local time' : 'Viewer local time — server clock unavailable'">{{ display }}<small>{{ auth.serverClock ? 'Server time' : 'Viewer time' }}</small></time>
</template>
<style scoped>
.dashboard-clock { position: fixed; z-index: 1041; padding: .4rem .65rem; border-radius: .5rem; background: #101a28cc; color: #edf3fc; font-size: 1.1rem; font-variant-numeric: tabular-nums; pointer-events: none; }
small { display: block; font-size: .6rem; color: #b9c8d8; }
</style>
