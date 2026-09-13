<script setup lang="ts">
import { onMounted, ref, computed } from 'vue'
import AppLayout from '../components/layout/AppLayout.vue'
import CameraGrid from '../components/camera/CameraGrid.vue'
import ActivityStrip from '../components/clips/ActivityStrip.vue'
import ClipPlayer from '../components/clips/ClipPlayer.vue'
import { useCameraStore } from '../stores/cameras'
import { useClipStore } from '../stores/clips'
import { useSettingsStore } from '../stores/settings'
import { useGroupStore } from '../stores/groups'
import type { Clip } from '../types/clip'

const cameraStore = useCameraStore()
const clipStore = useClipStore()
const settings = useSettingsStore()
const groupStore = useGroupStore()

const DASHBOARD_GROUP_KEY = 'witness-dashboard-group'
const DASHBOARD_ACTIVITY_KEY = 'witness-dashboard-activity-visible'
const savedGroup = localStorage.getItem(DASHBOARD_GROUP_KEY)
const selectedGroupId = ref<number | null>(savedGroup !== null ? Number(savedGroup) : null)
const showRecentActivity = ref(localStorage.getItem(DASHBOARD_ACTIVITY_KEY) !== '0')
const playingClip = ref<Clip | null>(null)

function toggleRecentActivity() {
  showRecentActivity.value = !showRecentActivity.value
  localStorage.setItem(DASHBOARD_ACTIVITY_KEY, showRecentActivity.value ? '1' : '0')
}

function selectGroup(id: number | null) {
  selectedGroupId.value = id
  if (id === null) {
    localStorage.removeItem(DASHBOARD_GROUP_KEY)
  } else {
    localStorage.setItem(DASHBOARD_GROUP_KEY, String(id))
  }
}

const groupCameraIds = computed(() => {
  if (selectedGroupId.value === null) return null
  return new Set(groupStore.camerasInGroup(selectedGroupId.value).map(c => c.id))
})

const hasDashboardActivity = computed(() => {
  const included = (cameraId: number) =>
    !groupCameraIds.value || groupCameraIds.value.has(cameraId)
  return cameraStore.cameras.some(c => c.isRecording && included(c.id)) ||
    clipStore.recentClips.some(c => c.duration >= 2 && included(c.camera))
})

onMounted(async () => {
  await cameraStore.fetchCameras()
  if (groupStore.groups.length === 0) {
    await groupStore.fetchGroups()
  }
  // Clear saved group if it no longer exists
  if (selectedGroupId.value !== null &&
      !groupStore.activeGroups.some(g => g.id === selectedGroupId.value)) {
    selectGroup(null)
  }
})
</script>

<template>
  <AppLayout>
    <template #title>Dashboard</template>
    <template #actions>
      <div class="d-flex align-items-center gap-2">
        <!-- Group picker -->
        <div v-if="groupStore.activeGroups.length > 0" class="btn-group btn-group-sm me-2">
          <button
            class="btn"
            :class="selectedGroupId === null ? 'btn-primary' : 'btn-outline-secondary'"
            @click="selectGroup(null)"
          >All</button>
          <button
            v-for="group in groupStore.activeGroups"
            :key="group.id"
            class="btn"
            :class="selectedGroupId === group.id ? 'btn-primary' : 'btn-outline-secondary'"
            @click="selectGroup(group.id)"
          >{{ group.displayName }}</button>
        </div>
        <button
          class="btn btn-sm btn-outline-secondary mobile-hide"
          @click="settings.decreaseScale"
          title="Zoom out"
        >−</button>
        <span class="small text-muted-custom mobile-hide" style="min-width: 3rem; text-align: center;">
          {{ settings.cameraPreviewScale }}%
        </span>
        <button
          class="btn btn-sm btn-outline-secondary mobile-hide"
          @click="settings.increaseScale"
          title="Zoom in"
        >+</button>
        <button
          class="btn btn-sm"
          :class="settings.fullscreenMode ? 'btn-primary' : 'btn-outline-secondary'"
          @click="settings.toggleFullscreen"
          title="Fullscreen"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/>
            <line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/>
          </svg>
        </button>
        <div class="vr mx-1" style="border-color: var(--bs-border-color);"></div>
        <button
          class="btn btn-sm"
          :class="settings.streamingMode !== 'jpeg' ? 'btn-outline-primary' : 'btn-outline-secondary'"
          @click="settings.toggleStreamingMode"
          :title="'Mode: ' + settings.streamingMode.toUpperCase()"
        >
          {{ settings.streamingMode.toUpperCase() }}
        </button>
        <button
          class="btn btn-sm"
          :class="showRecentActivity ? 'btn-outline-primary' : 'btn-outline-secondary'"
          @click="toggleRecentActivity"
          :title="showRecentActivity ? 'Hide recent activity' : 'Show recent activity'"
        >Activity</button>
      </div>
    </template>

    <CameraGrid
      :group-camera-ids="groupCameraIds"
      :fullscreen-bottom-inset="settings.fullscreenMode && showRecentActivity && hasDashboardActivity ? 108 : 0"
    />

    <ActivityStrip
      v-if="showRecentActivity"
      class="dashboard-activity-strip"
      :class="{ 'dashboard-activity-strip-fullscreen': settings.fullscreenMode }"
      :camera-ids="groupCameraIds"
      @play="playingClip = $event"
    />

    <button
      v-if="settings.fullscreenMode"
      class="fullscreen-activity-toggle"
      :class="showRecentActivity ? 'active' : ''"
      @click="toggleRecentActivity"
      :title="showRecentActivity ? 'Hide recent activity' : 'Show recent activity'"
    >Activity</button>

    <ClipPlayer v-if="playingClip" :clip="playingClip" @close="playingClip = null" />
  </AppLayout>
</template>

<style scoped>
.dashboard-activity-strip {
  margin-top: 1rem;
  margin-bottom: 0;
}

.dashboard-activity-strip-fullscreen {
  position: fixed;
  left: 0;
  right: 0;
  bottom: 0;
  height: 108px;
  z-index: 1001;
  margin: 0;
  border-radius: 0;
  border-width: 1px 0 0;
  background: rgba(15, 15, 20, 0.97);
}

.fullscreen-activity-toggle {
  position: fixed;
  top: 10px;
  right: 56px;
  z-index: 1002;
  padding: 6px 10px;
  border: 1px solid rgba(255, 255, 255, 0.3);
  border-radius: 0.375rem;
  background: rgba(0, 0, 0, 0.6);
  color: rgba(255, 255, 255, 0.75);
  font-size: 0.75rem;
  cursor: pointer;
}

.fullscreen-activity-toggle:hover,
.fullscreen-activity-toggle.active {
  background: rgba(13, 110, 253, 0.8);
  color: #fff;
}
</style>
