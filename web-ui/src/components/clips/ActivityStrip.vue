<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { useClipStore } from '../../stores/clips'
import { useCameraStore } from '../../stores/cameras'
import { useTagStore } from '../../stores/tags'
import { useEventStream } from '../../composables/useEventStream'
import { formatDistanceToNow } from 'date-fns'
import type { Clip } from '../../types/clip'
import { buildActivityGroups, type ActivityGroup } from '../../composables/useActivityGrouping'

const TRIVIAL_DURATION = 2
const MAX_STRIP_CLIPS = 10
const ACTIVITY_GROUPING_KEY = 'witness-activity-grouping-enabled'

const emit = defineEmits<{
  play: [clip: Clip]
}>()

const props = defineProps<{
  cameraIds?: Set<number> | null
  orientation?: 'horizontal' | 'vertical'
  forceVisible?: boolean
  fill?: boolean
}>()

const clipStore = useClipStore()
const cameraStore = useCameraStore()
const tagStore = useTagStore()
const events = useEventStream()

let removeListener: (() => void) | null = null
let previewRefreshTimer: ReturnType<typeof setInterval> | null = null
const previewTick = ref(0)
const activityGroups = ref<ActivityGroup[]>([])
const groupingPending = ref(false)
const groupingEnabled = ref(localStorage.getItem(ACTIVITY_GROUPING_KEY) !== '0')
const expandedGroups = ref<Set<string>>(new Set())
let groupingGeneration = 0

// Cameras currently recording (ongoing) — pinned left with live preview
const ongoingCameras = computed(() =>
  cameraStore.cameras.filter(c =>
    c.isRecording && (!props.cameraIds || props.cameraIds.has(c.id))
  )
)

// Recent clips sorted by time, hiding trivial (<2s), capped to strip size
const recentCandidates = computed(() =>
  [...clipStore.recentClips]
    .filter(c =>
      c.duration >= TRIVIAL_DURATION &&
      (!props.cameraIds || props.cameraIds.has(c.camera))
    )
    .sort((a, b) => b.timestamp - a.timestamp)
)

const visibleGroups = computed(() => activityGroups.value.slice(0, MAX_STRIP_CLIPS))

const matchingClipCount = computed(() =>
  clipStore.recentClips.filter(c =>
    c.duration >= TRIVIAL_DURATION &&
    (!props.cameraIds || props.cameraIds.has(c.camera))
  ).length
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

function groupLastTimestamp(group: ActivityGroup) {
  return group.clips[group.clips.length - 1]?.timestamp ?? group.representative.timestamp
}

function toggleGrouping() {
  groupingEnabled.value = !groupingEnabled.value
  localStorage.setItem(ACTIVITY_GROUPING_KEY, groupingEnabled.value ? '1' : '0')
  expandedGroups.value = new Set()
}

function toggleGroup(group: ActivityGroup) {
  const updated = new Set(expandedGroups.value)
  if (updated.has(group.id)) updated.delete(group.id)
  else updated.add(group.id)
  expandedGroups.value = updated
}

watch([recentCandidates, groupingEnabled], async ([clips, enabled]) => {
  const generation = ++groupingGeneration
  groupingPending.value = true
  if (!enabled) {
    activityGroups.value = clips.map(clip => ({
      id: `clip-${clip.uid}`,
      representative: clip,
      clips: [clip],
      maximumDistance: 0,
    }))
    groupingPending.value = false
    return
  }
  const groups = await buildActivityGroups(clips, clip => thumbUrl(clip))
  if (generation !== groupingGeneration) return
  activityGroups.value = groups
  groupingPending.value = false
}, { immediate: true })

onMounted(async () => {
  if (tagStore.tags.length === 0) await tagStore.fetchTags()
  await clipStore.fetchRecent(50)

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
  <div
    v-if="forceVisible || ongoingCameras.length > 0 || recentCandidates.length > 0"
    class="activity-strip"
    :class="{ vertical: orientation === 'vertical', fill }"
  >
    <div class="strip-header">
      <span class="strip-title">Recent Activity</span>
      <span class="strip-count text-muted-custom">
        {{ matchingClipCount }} unreviewed<span v-if="groupingEnabled && !groupingPending"> · {{ activityGroups.length }} activities</span>
      </span>
      <button
        class="strip-grouping-toggle"
        :class="{ active: groupingEnabled }"
        :title="groupingEnabled ? 'Show every clip separately' : 'Group visually similar clips'"
        @click.stop="toggleGrouping"
      >{{ groupingPending ? 'Grouping…' : groupingEnabled ? 'Grouped' : 'Group similar' }}</button>
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
      <div v-if="forceVisible && ongoingCameras.length === 0 && recentCandidates.length === 0" class="strip-empty">
        Activity will appear here
      </div>

      <!-- Recent activities ordered by their latest clip. Groups use a stable
           representative and expand without navigating or resetting the page. -->
      <template v-for="group in visibleGroups" :key="group.id">
        <div
          class="strip-thumb"
          :class="{ 'strip-unreviewed': !group.representative.reviewed, 'strip-grouped': group.clips.length > 1 }"
          @click.stop="onClickClip(group.representative)"
        >
          <img :src="thumbUrl(group.representative)" :alt="`Clip ${group.representative.uid}`" loading="lazy" />
          <div class="strip-thumb-tags">
            <span v-for="tag in clipTags(group.representative)" :key="tag.name" class="strip-tag">
              {{ tag.icon || tag.display }}
            </span>
            <span v-for="name in (group.representative.recognizedFaces ?? [])" :key="'face-' + name" class="strip-tag strip-face-tag">
              👤
            </span>
          </div>
          <button
            v-if="group.clips.length > 1"
            class="strip-group-count"
            :title="expandedGroups.has(group.id) ? 'Collapse similar clips' : `Show ${group.clips.length} similar clips`"
            @click.stop="toggleGroup(group)"
          >{{ expandedGroups.has(group.id) ? '−' : '×' + group.clips.length }}</button>
          <div class="strip-overlay-bottom">
            <span class="strip-cam-name">{{ cameraName(group.representative.camera) }}</span>
            <span class="strip-time-badge">{{ timeAgo(groupLastTimestamp(group)) }}</span>
          </div>
          <span class="strip-duration">{{ group.representative.duration }}s</span>
        </div>
        <div
          v-for="clip in expandedGroups.has(group.id) ? group.clips.slice(1).reverse() : []"
          :key="`member-${clip.uid}`"
          class="strip-thumb strip-group-member"
          :class="{ 'strip-unreviewed': !clip.reviewed }"
          @click.stop="onClickClip(clip)"
        >
          <img :src="thumbUrl(clip)" :alt="`Clip ${clip.uid}`" loading="lazy" />
          <div class="strip-overlay-bottom">
            <span class="strip-cam-name">{{ cameraName(clip.camera) }}</span>
            <span class="strip-time-badge">{{ timeAgo(clip.timestamp) }}</span>
          </div>
          <span class="strip-duration">{{ clip.duration }}s</span>
        </div>
      </template>
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
.strip-grouping-toggle {
  margin-left: auto;
  border: 1px solid var(--bs-border-color, #555);
  border-radius: 999px;
  background: transparent;
  color: var(--bs-secondary-color, #999);
  padding: 0.08rem 0.45rem;
  font-size: 0.62rem;
  line-height: 1.25;
}
.strip-grouping-toggle:hover,
.strip-grouping-toggle.active {
  border-color: var(--bs-info, #0dcaf0);
  color: var(--bs-info, #0dcaf0);
}
.strip-items {
  display: flex;
  gap: 0.4rem;
  overflow-x: auto;
  overflow-y: hidden;
  padding-bottom: 0.2rem;
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
.strip-grouped {
  box-shadow: 3px 3px 0 rgba(13, 202, 240, 0.3), 6px 6px 0 rgba(13, 202, 240, 0.12);
  margin-right: 6px;
}
.strip-group-member {
  border-style: dashed;
  opacity: 0.92;
}
.strip-group-count {
  position: absolute;
  top: 2px;
  right: 2px;
  z-index: 2;
  border: 0;
  border-radius: 999px;
  background: rgba(13, 202, 240, 0.9);
  color: #061519;
  font-size: 0.62rem;
  font-weight: 700;
  min-width: 1.55rem;
  padding: 0.1rem 0.3rem;
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
.strip-grouped .strip-duration {
  top: 1.55rem;
}
.activity-strip.vertical {
  height: 100%;
  display: flex;
  flex-direction: column;
  margin: 0;
  border-radius: 0;
}
.activity-strip.fill:not(.vertical) {
  height: 100%;
  display: flex;
  flex-direction: column;
  margin: 0;
  border-radius: 0;
}
.activity-strip.fill:not(.vertical) .strip-items {
  flex: 1;
  min-height: 0;
}
.activity-strip.fill:not(.vertical) .strip-thumb {
  width: auto;
  height: 100%;
}
.activity-strip.vertical .strip-header {
  flex-wrap: wrap;
}
.activity-strip.vertical .strip-items {
  flex: 1;
  min-height: 0;
  flex-direction: column;
  overflow-x: hidden;
  overflow-y: auto;
  align-items: stretch;
}
.activity-strip.vertical .strip-thumb {
  width: 100%;
}
.strip-empty {
  color: var(--bs-secondary-color, #888);
  font-size: 0.75rem;
  padding: 0.5rem;
}
</style>
