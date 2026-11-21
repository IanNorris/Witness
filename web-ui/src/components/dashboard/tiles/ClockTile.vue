<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useAuthStore } from '../../../stores/auth'

// Server-local clock (same source as the dashboard clock badge). Falls back
// to the viewer's clock, labelled as such, when the server clock is unknown.
const props = withDefaults(defineProps<{ seconds?: boolean; date?: boolean; compact?: boolean }>(), { seconds: true, date: true })
const auth = useAuthStore()
const tick = ref(performance.now())
let timer: ReturnType<typeof setInterval> | undefined
const now = computed(() => {
  const clock = auth.serverClock
  if (!clock) return { value: new Date(), server: false }
  // Shifted so the UTC getters read the server's local wall time.
  return { value: new Date(clock.unixMs + tick.value - clock.receivedAt + clock.offsetMinutes * 60000), server: true }
})
const time = computed(() => {
  const d = now.value.value
  const parts = now.value.server
    ? [d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds()]
    : [d.getHours(), d.getMinutes(), d.getSeconds()]
  return parts.slice(0, props.seconds ? 3 : 2).map(n => String(n).padStart(2, '0')).join(':')
})
const dateText = computed(() => now.value.value.toLocaleDateString(undefined, {
  weekday: 'short', day: 'numeric', month: 'short', ...(now.value.server ? { timeZone: 'UTC' } : {}),
}))
onMounted(() => { timer = setInterval(() => { tick.value = performance.now() }, 1000) })
onUnmounted(() => { if (timer) clearInterval(timer) })
</script>

<template>
  <div class="clock-tile" :class="{ compact }" :title="now.server ? 'Server local time' : 'Viewer local time — server clock unavailable'">
    <time class="clock-time">{{ time }}</time>
    <span v-if="date" class="clock-date">{{ dateText }}</span>
    <small v-if="!now.server" class="clock-source">Viewer time</small>
  </div>
</template>

<style scoped>
.clock-tile { container-type: size; width: 100%; height: 100%; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: .2em; color: var(--witness-ink, #edf3fc); background: #0b1119; font-variant-numeric: tabular-nums; }
.clock-time { font-size: clamp(1rem, min(21cqw, 50cqh), 9rem); font-weight: 600; letter-spacing: -.02em; line-height: 1; }
.clock-date { font-size: clamp(.65rem, min(7cqw, 14cqh), 2rem); color: var(--witness-muted, #a5b6cc); }
.clock-source { font-size: .65rem; color: #8ea2b8; }
/* Badge form sizes to its content (a size container would collapse to 0×0). */
.compact { container-type: normal; width: auto; height: auto; background: rgba(16, 26, 40, .8); border-radius: .5rem; padding: .35rem .6rem; }
.compact .clock-time { font-size: 1.1rem; }
.compact .clock-date { font-size: .65rem; }
</style>
