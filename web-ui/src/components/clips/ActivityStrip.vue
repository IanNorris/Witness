<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useClipStore } from '../../stores/clips'
import { useCameraStore } from '../../stores/cameras'
import { useTagStore } from '../../stores/tags'
import { useEventStream } from '../../composables/useEventStream'
import { formatDistanceToNow } from 'date-fns'
import type { Clip } from '../../types/clip'

const TRIVIAL_DURATION = 2
const MAX_STRIP_CLIPS = 10

const emit = defineEmits<{
  play: [clip: Clip]
}>()

const clipStore = useClipStore()
const cameraStore = useCameraStore()
const tagStore = useTagStore()
const events = useEventStream()

let removeListener: (() => void) | null = null
let previewRefreshTimer: ReturnType<typeof setInterval> | null = null
const previewTick = ref(0)

// Cameras currently recording (ongoing) — pinned left with live preview
const ongoingCameras = computed(() =>
  cameraStore.cameras.filter(c => c.isRecording)
)

// Recent clips sorted by time, hiding trivial (<2s), capped to strip size
const sortedClips = computed(() =>
  [...clipStore.recentClips]
    .filter(c => c.duration >= TRIVIAL_DURATION)
    .sort((a, b) => b.timestamp - a.timestamp)
    .slice(0, MAX_STRIP_CLIPS)
)

function cameraName(cameraId: number) {
  return cameraStore.getCameraById(cameraId)?.name ?? `Camera ${cameraId}`
}

function thumbUrl(clip: Clip) {
  return clipStore.thumbnailUrl(clip.camera, clip.timestamp)
}

function livePreviewUrl(cameraId: number) {
  return `/camera/preview/${cameraId}?t=${previewTick.value}`
}

function timeAgo(timestamp: number) {
  return formatDistanceToNow(new Date(timestamp * 1000), { addSuffix: true })
}

function clipTags(clip: Clip) {
  if (!clip.tags) return []
  return clip.tags.split(/[;,]/).map(t => t.trim()).filter(Boolean).map(name => {
    const info = tagStore.getTagDisplay(name)
    return { name, ...info }
  })
}

function onClickClip(clip: Clip) {
  emit('play', clip)
  if (!clip.reviewed) {
    clipStore.reviewClip(clip.uid)
  }
}

onMounted(async () => {
  if (tagStore.tags.length === 0) await tagStore.fetchTags()
  await clipStore.fetchRecent(30)

  // Refresh live preview thumbnails every 2s
  previewRefreshTimer = setInterval(() => { previewTick.value++ }, 2000)

  // Listen for clip:new and camera:recording events to refresh
  removeListener = events.onEvent((evt) => {
    if (evt.event === 'camera:recording') {
      // Force preview refresh on recording state change
      previewTick.value++
    }
  })
})

onUnmounted(() => {
  if (removeListener) removeListener()
  if (previewRefreshTimer) clearInterval(previewRefreshTimer)
})
</script>

<template>
  <div v-if="ongoingCameras.length > 0 || sortedClips.length > 0" class="activity-strip">
    <div class="strip-header">
      <span class="strip-title">Recent Activity</span>
      <span class="strip-count text-muted-custom">{{ clipStore.recentClips.length }} unreviewed</span>
    </div>
    <div class="strip-items">
      <!-- Ongoing recordings pinned left -->
      <div
        v-for="cam in ongoingCameras"
        :key="'live-' + cam.id"
        class="strip-thumb strip-ongoing"
      >
        <img :src="livePreviewUrl(cam.id)" :alt="cam.name" />
        <div class="strip-overlay-bottom">
          <span class="strip-cam-name">{{ cam.name }}</span>
          <span class="strip-live-badge">● LIVE</span>
        </div>
      </div>

      <!-- Recent clips ordered by time -->
      <div
        v-for="clip in sortedClips"
        :key="clip.uid"
        class="strip-thumb"
        :class="{ 'strip-unreviewed': !clip.reviewed }"
        @click="onClickClip(clip)"
      >
        <img :src="thumbUrl(clip)" :alt="`Clip ${clip.uid}`" loading="lazy" />
        <div class="strip-thumb-tags">
          <span v-for="tag in clipTags(clip)" :key="tag.name" class="strip-tag">
            {{ tag.icon || tag.display }}
          </span>
          <span v-for="name in (clip.recognizedFaces ?? [])" :key="'face-' + name" class="strip-tag strip-face-tag">
            👤
          </span>
        </div>
        <div class="strip-overlay-bottom">
          <span class="strip-cam-name">{{ cameraName(clip.camera) }}</span>
          <span class="strip-time-badge">{{ timeAgo(clip.timestamp) }}</span>
        </div>
        <span class="strip-duration">{{ clip.duration }}s</span>
      </div>
    </div>
  </div>
</template>

<style scoped>
.activity-strip {
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  padding: 0.5rem;
  margin-bottom: 1rem;
}
.strip-header {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0 0.25rem 0.4rem;
  font-size: 0.8rem;
}
.strip-title {
  font-weight: 600;
}
.strip-count {
  font-size: 0.7rem;
}
.strip-items {
  display: flex;
  gap: 0.4rem;
  overflow: hidden;
}
.strip-thumb {
  position: relative;
  width: 120px;
  aspect-ratio: 16 / 9;
  border-radius: 4px;
  overflow: hidden;
  cursor: pointer;
  border: 2px solid transparent;
  transition: border-color 0.15s, transform 0.1s;
  flex-shrink: 0;
}
.strip-thumb:hover {
  transform: scale(1.05);
  border-color: var(--bs-primary, #7c3aed);
}
.strip-unreviewed {
  border-color: var(--bs-info, #0dcaf0);
}
.strip-ongoing {
  border-color: #dc3545;
  cursor: default;
}
.strip-thumb img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.strip-overlay-bottom {
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  background: linear-gradient(to top, rgba(0,0,0,0.75), transparent);
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  padding: 10px 4px 2px;
}
.strip-cam-name {
  color: #fff;
  font-size: 0.55rem;
  font-weight: 600;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.strip-time-badge {
  color: rgba(255,255,255,0.6);
  font-size: 0.5rem;
  white-space: nowrap;
  flex-shrink: 0;
}
.strip-live-badge {
  color: #dc3545;
  font-size: 0.55rem;
  font-weight: 700;
  flex-shrink: 0;
}
.strip-thumb-tags {
  position: absolute;
  top: 2px;
  left: 2px;
  display: flex;
  gap: 1px;
  font-size: 0.65rem;
}
.strip-tag {
  background: rgba(0,0,0,0.6);
  padding: 0 2px;
  border-radius: 2px;
  line-height: 1.2;
}
.strip-duration {
  position: absolute;
  top: 2px;
  right: 2px;
  background: rgba(0,0,0,0.75);
  color: #fff;
  font-size: 0.6rem;
  padding: 0 3px;
  border-radius: 2px;
}
</style>
