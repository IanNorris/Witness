<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import type { Clip } from '../../types/clip'
import { useClipStore } from '../../stores/clips'
import { useDetectionPlayback } from '../../composables/useDetectionOverlay'
import AudioEventTimeline from './AudioEventTimeline.vue'
import { useSettingsStore } from '../../stores/settings'

const props = defineProps<{
  clip: Clip
}>()

const emit = defineEmits<{ close: [] }>()

const clipStore = useClipStore()
const settings = useSettingsStore()
const videoSrc = ref(clipStore.videoUrl(props.clip.camera, props.clip.timestamp))
const videoRef = ref<HTMLVideoElement | null>(null)
const canvasRef = ref<HTMLCanvasElement | null>(null)
const playbackTime = ref(0)
const backgroundOnly = computed(() => {
  const events = props.clip.audioEvents ?? []
  return events.length > 0 && events.every(event => event.group.toLowerCase() === 'wind')
})

function toggleBackgroundMute() {
  settings.muteBackgroundOnlyClips = !settings.muteBackgroundOnlyClips
  if (backgroundOnly.value && videoRef.value) {
    videoRef.value.muted = settings.muteBackgroundOnlyClips
  }
}

const { enabled: overlayEnabled, toggle: toggleOverlay, loadDetections, frames: detectionFrames } = useDetectionPlayback(
  props.clip.camera,
  canvasRef,
  videoRef,
  props.clip.timestamp, // epoch seconds offset — detection timestamps are absolute
)

const hasDetections = computed(() => detectionFrames.value.length > 0)

function nudgeDetection(direction: 'next' | 'prev') {
  const video = videoRef.value
  if (!video || !detectionFrames.value.length) return
  const absoluteTime = video.currentTime + props.clip.timestamp

  let targetTime: number | null = null
  if (direction === 'next') {
    const frame = detectionFrames.value.find(f => f.t > absoluteTime + 0.5)
    if (frame) targetTime = frame.t - props.clip.timestamp
  } else {
    for (let i = detectionFrames.value.length - 1; i >= 0; i--) {
      if ((detectionFrames.value[i]?.t ?? 0) < absoluteTime - 0.5) {
        targetTime = detectionFrames.value[i]!.t - props.clip.timestamp
        break
      }
    }
  }

  if (targetTime !== null && targetTime >= 0) {
    // Pause first — some browsers can't seek while playing a non-faststart MP4
    const wasPlaying = !video.paused
    video.pause()
    video.currentTime = targetTime
    if (wasPlaying) {
      video.addEventListener('seeked', () => video.play().catch(() => {}), { once: true })
    }
  }
}

function seekToAudioEvent(offsetSeconds: number) {
  const video = videoRef.value
  if (!video) return
  video.currentTime = Math.max(0, Math.min(offsetSeconds, Number.isFinite(video.duration) ? video.duration : offsetSeconds))
}

function handleKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') emit('close')
  if (e.key === 'n') { e.preventDefault(); nudgeDetection('next') }
  if (e.key === 'p') { e.preventDefault(); nudgeDetection('prev') }
}

onMounted(async () => {
  // Pre-load detection data for this clip's time range
  const from = props.clip.timestamp
  const to = from + props.clip.duration
  await loadDetections(from, to)
  // Auto-enable overlay based on per-camera preference (default: on)
  const saved = localStorage.getItem(`witness-detection-overlay-${props.clip.camera}`)
  if (saved === null || saved === '1') {
    toggleOverlay()
  }
})
</script>

<template>
  <Teleport to="body">
    <div class="clip-modal-overlay" @click.self="emit('close')" @keydown="handleKeydown" tabindex="0">
      <div class="clip-modal">
        <div class="clip-modal-header">
          <span>Clip {{ clip.uid }}</span>
          <div class="d-flex gap-2 align-items-center">
            <button class="btn btn-sm" :class="settings.muteBackgroundOnlyClips ? 'btn-info' : 'btn-outline-secondary'"
              :title="'Start wind-only clips muted (you can always unmute in the player)'"
              :aria-pressed="settings.muteBackgroundOnlyClips" @click="toggleBackgroundMute">
              Wind-only mute
            </button>
            <button
              class="btn btn-sm"
              :class="overlayEnabled ? 'btn-success' : 'btn-outline-secondary'"
              @click="toggleOverlay"
              title="Toggle detection overlay"
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <rect x="3" y="3" width="18" height="18" rx="2" />
                <circle cx="12" cy="12" r="3" />
              </svg>
            </button>
            <button class="btn btn-sm btn-outline-secondary" :disabled="!hasDetections" @click="nudgeDetection('prev')" title="Previous detection">⏮</button>
            <button class="btn btn-sm btn-outline-secondary" :disabled="!hasDetections" @click="nudgeDetection('next')" title="Next detection">⏭</button>
            <button class="btn btn-sm btn-outline-secondary" @click="emit('close')">✕</button>
          </div>
        </div>
        <div class="clip-modal-body">
          <div class="clip-video-wrap">
            <video ref="videoRef" :src="videoSrc" controls autoplay class="clip-video"
              :muted="backgroundOnly && settings.muteBackgroundOnlyClips"
              @timeupdate="playbackTime = videoRef?.currentTime ?? 0" />
            <canvas ref="canvasRef" class="detection-overlay" v-show="overlayEnabled" />
          </div>
          <AudioEventTimeline :clip="clip" :playback-time="playbackTime" @seek="seekToAudioEvent" />
          <div v-if="backgroundOnly && settings.muteBackgroundOnlyClips" class="audio-mute-hint">Only wind was classified; playback starts muted. Use the video controls to listen.</div>
        </div>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.clip-modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.85);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9999;
}
.clip-modal {
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  max-width: 90vw;
  max-height: 90vh;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.clip-modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 0.5rem 1rem;
  border-bottom: 1px solid var(--bs-border-color, #333);
}
.clip-modal-body {
  padding: 0;
}
.clip-video-wrap {
  position: relative;
}
.clip-video {
  width: 100%;
  max-height: 80vh;
  display: block;
}
.audio-mute-hint { padding: 0 1rem 0.65rem; color: #a9b5c5; font-size: 0.72rem; }
.detection-overlay {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
  z-index: 5;
}
</style>
