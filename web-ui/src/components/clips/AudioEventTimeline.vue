<script setup lang="ts">
import { computed } from 'vue'
import type { AudioEvent, Clip } from '../../types/clip'

const props = defineProps<{
  clip: Clip
  compact?: boolean
  playbackTime?: number
}>()

const emit = defineEmits<{ seek: [offsetSeconds: number] }>()

const colors: Record<string, string> = {
  speech: '#9b8cff',
  dog: '#f5a85c',
  animal: '#e7be64',
  footsteps: '#75c8a0',
  vehicle: '#64b9e6',
  alarm: '#f07878',
  glass: '#e6a0ca',
  door: '#9bc989',
  wind: '#8997a9',
}

const events = computed(() => (props.clip.audioEvents ?? [])
  .filter(event => Number.isFinite(event.startTime) && Number.isFinite(event.endTime) && event.endTime > event.startTime)
  .sort((a, b) => a.startTime - b.startTime))

const duration = computed(() => Math.max(
  Number.isFinite(props.clip.duration) ? props.clip.duration : 0,
  ...events.value.map(event => event.endTime - props.clip.timestamp),
  0.1,
))

const groups = computed(() => {
  const byName = new Map<string, AudioEvent[]>()
  for (const event of events.value) {
    const name = event.group || 'Other'
    const group = byName.get(name) ?? []
    group.push(event)
    byName.set(name, group)
  }
  return [...byName.entries()].map(([name, segments]) => ({ name, segments }))
})

function eventStyle(event: AudioEvent) {
  const start = Math.max(0, Math.min(duration.value, event.startTime - props.clip.timestamp))
  const end = Math.max(start, Math.min(duration.value, event.endTime - props.clip.timestamp))
  return {
    left: `${start / duration.value * 100}%`,
    width: `${Math.max(0.6, (end - start) / duration.value * 100)}%`,
    backgroundColor: colors[event.group.toLowerCase()] ?? '#78b9c9',
  }
}

function eventTitle(event: AudioEvent) {
  const start = Math.max(0, event.startTime - props.clip.timestamp)
  const end = Math.max(start, event.endTime - props.clip.timestamp)
  return `${event.group} · ${Math.round(event.peakScore * 100)}% · ${start.toFixed(1)}–${end.toFixed(1)}s`
}

function seekTo(event: AudioEvent) {
  emit('seek', Math.max(0, event.startTime - props.clip.timestamp))
}

const playheadStyle = computed(() => ({
  left: `${Math.max(0, Math.min(100, (props.playbackTime ?? 0) / duration.value * 100))}%`,
}))
</script>

<template>
  <div v-if="events.length" class="audio-timeline" :class="{ compact }" :aria-label="`Audio events for clip ${clip.uid}`">
    <template v-if="compact">
      <div class="compact-track" :title="`${events.length} classified sound events`">
        <span v-for="(event, index) in events" :key="index" class="segment compact-segment"
          :style="eventStyle(event)" :title="eventTitle(event)" />
      </div>
    </template>
    <template v-else>
      <div class="timeline-heading">Sound events <span class="muted">{{ events.length }} detected</span></div>
      <div v-for="group in groups" :key="group.name" class="timeline-row">
        <span class="group-label">{{ group.name }}</span>
        <div class="track">
          <button v-for="(event, index) in group.segments" :key="index" type="button"
            class="segment event-button" :style="eventStyle(event)" :title="`${eventTitle(event)} · click to seek`"
            :aria-label="`Seek to ${eventTitle(event)}`" @click="seekTo(event)" />
          <span v-if="playbackTime !== undefined" class="playhead" :style="playheadStyle" />
        </div>
      </div>
      <div class="timeline-scale"><span>0s</span><span>{{ duration.toFixed(1) }}s</span></div>
    </template>
  </div>
</template>

<style scoped>
.audio-timeline { padding: 0.6rem 1rem 0.7rem; color: #e7edf5; }
.timeline-heading { font-size: 0.78rem; font-weight: 600; margin-bottom: 0.4rem; }
.muted { margin-left: 0.4rem; color: #a9b5c5; font-weight: 400; }
.timeline-row { display: grid; grid-template-columns: 6.5rem minmax(0, 1fr); align-items: center; gap: 0.6rem; margin: 0.2rem 0; }
.group-label { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 0.72rem; text-transform: capitalize; }
.track, .compact-track { position: relative; background: #394554; border-radius: 3px; }
.track { height: 0.9rem; }
.compact-track { height: 0.35rem; overflow: hidden; }
.segment { position: absolute; top: 0; height: 100%; border-radius: 2px; opacity: 0.9; }
.event-button { border: 1px solid rgba(255,255,255,0.3); cursor: pointer; padding: 0; min-width: 4px; }
.event-button:hover, .event-button:focus-visible { opacity: 1; outline: 2px solid white; z-index: 2; }
.playhead { position: absolute; top: -2px; bottom: -2px; width: 2px; background: white; box-shadow: 0 0 3px #000; pointer-events: none; z-index: 3; }
.timeline-scale { display: flex; justify-content: space-between; margin-left: 7.1rem; color: #a9b5c5; font-size: 0.66rem; }
.compact { padding: 0 0.5rem; }
@media (max-width: 540px) { .timeline-row { grid-template-columns: 4.5rem minmax(0, 1fr); } .timeline-scale { margin-left: 5.1rem; } }
</style>
