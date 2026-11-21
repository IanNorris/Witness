<script setup lang="ts">
import { computed, onMounted, provide, ref } from 'vue'
import { useRouter } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import CameraGrid from '../components/camera/CameraGrid.vue'
import ActivityStrip from '../components/clips/ActivityStrip.vue'
import ClipPlayer from '../components/clips/ClipPlayer.vue'
import RecentReplayOverlay from '../components/clips/RecentReplayOverlay.vue'
import DashboardClipSearch from '../components/clips/DashboardClipSearch.vue'
import CameraStreamChoices from '../components/camera/CameraStreamChoices.vue'
import DashboardSwitcher from '../components/dashboard/DashboardSwitcher.vue'
import MobileSourceView from '../components/dashboard/MobileSourceView.vue'
import { useIsPhone } from '../composables/useViewport'
import { useDashboardPlayback, dashboardPlaybackKey } from '../composables/useDashboardPlayback'
import { useCameraStore } from '../stores/cameras'
import { useDashboardStore } from '../stores/dashboards'
import { useSettingsStore } from '../stores/settings'
import type { Clip } from '../types/clip'

// Zero-configuration overview of every camera the viewer can access, and
// the entry point to dashboards. Not stored on the server.
const router = useRouter()
const cameraStore = useCameraStore()
const store = useDashboardStore()
const settings = useSettingsStore()
const phone = useIsPhone()
const playingClip = ref<Clip | null>(null)
const replayCameraId = ref<number | null>(null)
const searchingClips = ref(false)
const replayCamera = computed(() => cameraStore.getCameraById(replayCameraId.value ?? -1))
const connectedCount = computed(() => cameraStore.cameras.filter(camera => camera.status === 'Connected').length)
const captureStartAt = ref<number | undefined>()

// Live / DVR scrub-back on every overview tile, as on dashboards.
const allCameraIds = computed(() => cameraStore.cameras.map(camera => camera.id))
const playbackHeld = computed(() => searchingClips.value || playingClip.value !== null || replayCameraId.value !== null)
const playback = useDashboardPlayback(allCameraIds, playbackHeld, (id, timestamp) => {
  captureStartAt.value = playback.get(id).requestedAt === null ? Math.floor(Date.now() / 1000) - 60 : timestamp
  replayCameraId.value = id
})
provide(dashboardPlaybackKey, playback)

function openDashboard(id: number | null) {
  if (id !== null) void router.push(`/d/${id}`)
}

onMounted(async () => {
  store.listen()
  if (settings.fullscreenMode) settings.fullscreenMode = false // legacy fullscreen preference
  const [list] = await Promise.all([store.fetchList().catch(() => []), cameraStore.fetchCameras()])
  // Reopen the dashboard this device last used, unless the viewer chose Overview.
  const last = store.lastDashboardId()
  if (last !== null && router.currentRoute.value.query.overview === undefined) {
    if (list.some(d => d.id === last)) void router.replace(`/d/${last}`)
    else store.rememberDashboard(null)
  }
})
</script>

<template>
  <AppLayout wrap-actions>
    <template #title>Overview</template>
    <template #actions>
      <div class="d-flex align-items-center gap-2 flex-wrap">
        <DashboardSwitcher :model-value="null" allow-overview @update:model-value="openDashboard" />
        <RouterLink class="btn btn-sm btn-outline-secondary" to="/dashboards">Manage dashboards</RouterLink>
        <template v-if="!phone">
          <label class="dashboard-sync"><input type="checkbox" :checked="playback.sync.value" @change="playback.setSync(($event.target as HTMLInputElement).checked)"> Sync cameras</label>
          <details class="dashboard-playback-settings">
            <summary>Playback settings</summary>
            <label>Return to live after inactivity
              <select v-model.number="settings.dashboardReturnLiveSeconds" class="form-select form-select-sm">
                <option :value="0">Never</option><option :value="60">1 minute</option><option :value="180">3 minutes</option><option :value="300">5 minutes</option><option :value="600">10 minutes</option>
              </select>
            </label>
            <label><input v-model="settings.dashboardQuietControls" type="checkbox"> Quiet controls on unselected live cameras</label>
            <CameraStreamChoices :cameras="cameraStore.cameras" />
          </details>
          <button class="btn btn-sm btn-outline-secondary" @click="settings.decreaseScale" title="Zoom out">−</button>
          <span class="small text-muted-custom" style="min-width: 3rem; text-align: center;">{{ settings.cameraPreviewScale }}%</span>
          <button class="btn btn-sm btn-outline-secondary" @click="settings.increaseScale" title="Zoom in">+</button>
          <button class="btn btn-sm btn-outline-secondary" @click="searchingClips = true">Search clips</button>
        </template>
      </div>
    </template>

    <section class="view-heading" aria-label="Live overview">
      <div><div class="ui-eyebrow">LIVE OVERVIEW</div><h1>Your cameras</h1><p>Every camera you can access. Build a dashboard for the wall display.</p></div>
      <span class="ui-count-pill">{{ cameraStore.cameras.length }} cameras · {{ connectedCount }} connected</span>
    </section>

    <section v-if="store.loaded && store.list.length === 0 && !phone" class="overview-cta">
      <div><strong>No dashboards yet.</strong> Arrange cameras, spotlights and activity for a full-screen display.</div>
      <RouterLink class="btn btn-sm btn-primary" to="/dashboards?new=1">Create a dashboard</RouterLink>
    </section>

    <MobileSourceView v-if="phone" :camera-ids="cameraStore.cameras.map(camera => camera.id)" />
    <template v-else>
      <CameraGrid @replay="replayCameraId = $event" />
      <div class="overview-activity"><ActivityStrip @play="playingClip = $event" /></div>
    </template>

    <ClipPlayer v-if="playingClip" :clip="playingClip" @close="playingClip = null" />
    <RecentReplayOverlay v-if="replayCamera" :key="replayCamera.id" :camera-id="replayCamera.id"
      :camera-name="replayCamera.name" capture-only :start-at="captureStartAt" @close="replayCameraId = null" />
    <DashboardClipSearch v-if="searchingClips" @close="searchingClips = false" />
  </AppLayout>
</template>

<style scoped>
.overview-cta { display: flex; align-items: center; justify-content: space-between; gap: 1rem; flex-wrap: wrap; margin-bottom: 1rem; padding: .75rem 1rem; border: 1px solid #2c4058; border-radius: .7rem; background: #111c2b; font-size: .85rem; }
.overview-activity { margin-top: 1rem; }
.dashboard-sync { display: inline-flex; gap: 6px; align-items: center; font-size: 12px; white-space: nowrap; }
.dashboard-playback-settings { position: relative; font-size: 12px; }
.dashboard-playback-settings[open] { background: var(--bs-body-bg); padding: 8px; border: 1px solid var(--bs-border-color); border-radius: 6px; }
.dashboard-playback-settings label { display: block; margin-top: 8px; }
</style>
