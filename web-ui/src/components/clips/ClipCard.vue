<script setup lang="ts">
import { computed } from 'vue'
import type { Clip } from '../../types/clip'
import { LightingCondition } from '../../types/clip'
import { useClipStore } from '../../stores/clips'
import { useCameraStore } from '../../stores/cameras'
import { format } from 'date-fns'

const props = defineProps<{
  clip: Clip
}>()

const emit = defineEmits<{
  play: [clip: Clip]
  toggleSave: [clip: Clip]
  delete: [clip: Clip]
  retag: [clip: Clip]
}>()

const clipStore = useClipStore()
const cameraStore = useCameraStore()

const camera = computed(() => cameraStore.getCameraById(props.clip.camera))
const thumbUrl = computed(() => clipStore.thumbnailUrl(props.clip.camera, props.clip.timestamp))
const dateStr = computed(() => format(new Date(props.clip.timestamp * 1000), 'dd MMM yyyy'))
const timeStr = computed(() => format(new Date(props.clip.timestamp * 1000), 'HH:mm:ss'))
const durationStr = computed(() => {
  const s = props.clip.duration
  if (s < 60) return `${s}s`
  return `${Math.floor(s / 60)}m ${s % 60}s`
})

const tagList = computed(() => {
  if (!props.clip.tags) return []
  return props.clip.tags.split(',').map(t => t.trim()).filter(Boolean)
})

const lightingLabel = computed(() => {
  if (props.clip.lighting === LightingCondition.Day) return 'Day'
  if (props.clip.lighting === LightingCondition.Night) return 'Night'
  return null
})

const lightingClass = computed(() => {
  if (props.clip.lighting === LightingCondition.Day) return 'bg-warning text-dark'
  if (props.clip.lighting === LightingCondition.Night) return 'bg-primary'
  return ''
})
</script>

<template>
  <div class="clip-card" :class="{ 'clip-saved': clip.saved }">
    <div class="clip-thumb" @click="emit('play', clip)">
      <img :src="thumbUrl" :alt="`Clip ${clip.uid}`" loading="lazy" />
      <div class="clip-play-btn">
        <svg width="36" height="36" viewBox="0 0 24 24" fill="currentColor">
          <path d="M8 5v14l11-7z"/>
        </svg>
      </div>
      <div class="clip-duration">{{ durationStr }}</div>
      <span v-if="lightingLabel" class="badge clip-lighting" :class="lightingClass">
        {{ lightingLabel }}
      </span>
      <div v-if="clip.saved" class="clip-saved-badge">
        <svg width="14" height="14" viewBox="0 0 16 16" fill="currentColor">
          <path d="M2 2v13.5a.5.5 0 0 0 .74.439L8 13.069l5.26 2.87A.5.5 0 0 0 14 15.5V2a2 2 0 0 0-2-2H4a2 2 0 0 0-2 2z"/>
        </svg>
      </div>
    </div>

    <div class="clip-info">
      <div class="clip-meta">
        <span class="clip-date">{{ dateStr }}</span>
        <span class="clip-time">{{ timeStr }}</span>
      </div>
      <div v-if="camera" class="clip-camera text-muted-custom small">
        {{ camera.name }}
      </div>
      <div v-if="tagList.length" class="clip-tags mt-1">
        <span v-for="tag in tagList" :key="tag" class="badge bg-secondary me-1">
          {{ tag }}
        </span>
      </div>
    </div>

    <div class="clip-actions">
      <button
        class="btn btn-sm"
        :class="clip.saved ? 'btn-warning' : 'btn-outline-secondary'"
        :title="clip.saved ? 'Unsave' : 'Save'"
        @click="emit('toggleSave', clip)"
      >
        <svg width="14" height="14" viewBox="0 0 16 16" fill="currentColor">
          <path v-if="clip.saved" d="M2 2v13.5a.5.5 0 0 0 .74.439L8 13.069l5.26 2.87A.5.5 0 0 0 14 15.5V2a2 2 0 0 0-2-2H4a2 2 0 0 0-2 2z"/>
          <path v-else d="M2 2a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v13.5a.5.5 0 0 1-.74.439L8 13.069l-5.26 2.87A.5.5 0 0 1 2 15.5V2zm2-1a1 1 0 0 0-1 1v12.566l4.723-2.482a.5.5 0 0 1 .554 0L13 14.566V2a1 1 0 0 0-1-1H4z"/>
        </svg>
      </button>
      <button
        class="btn btn-sm btn-outline-secondary"
        title="Re-detect"
        @click="emit('retag', clip)"
      >
        ↻
      </button>
      <button
        class="btn btn-sm btn-outline-danger"
        title="Delete"
        @click="emit('delete', clip)"
      >
        ✕
      </button>
    </div>
  </div>
</template>

<style scoped>
.clip-card {
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  overflow: hidden;
  transition: border-color 0.15s;
}
.clip-card:hover {
  border-color: var(--bs-primary, #7c3aed);
}
.clip-saved {
  border-color: var(--bs-warning, #f59e0b);
}

.clip-thumb {
  position: relative;
  cursor: pointer;
  aspect-ratio: 16 / 9;
  background: #000;
  overflow: hidden;
}
.clip-thumb img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.clip-play-btn {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  color: rgba(255, 255, 255, 0.85);
  background: rgba(0, 0, 0, 0.25);
  opacity: 0;
  transition: opacity 0.15s;
}
.clip-play-btn svg {
  filter: drop-shadow(0 1px 3px rgba(0,0,0,0.5));
}
.clip-thumb:hover .clip-play-btn {
  opacity: 1;
}
.clip-duration {
  position: absolute;
  bottom: 4px;
  right: 4px;
  background: rgba(0,0,0,0.75);
  color: #fff;
  font-size: 0.7rem;
  padding: 1px 5px;
  border-radius: 3px;
}
.clip-lighting {
  position: absolute;
  top: 4px;
  left: 4px;
  font-size: 0.65rem;
}
.clip-saved-badge {
  position: absolute;
  top: 4px;
  right: 4px;
  color: var(--bs-warning, #f59e0b);
}

.clip-info {
  padding: 0.5rem;
}
.clip-meta {
  display: flex;
  justify-content: space-between;
  font-size: 0.8rem;
}
.clip-tags .badge {
  font-size: 0.65rem;
}

.clip-actions {
  display: flex;
  gap: 0.25rem;
  padding: 0 0.5rem 0.5rem;
}
</style>
