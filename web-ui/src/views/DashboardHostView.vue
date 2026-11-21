<script setup lang="ts">
import { computed, onMounted, onUnmounted, provide, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import DashboardCanvas from '../components/dashboard/DashboardCanvas.vue'
import DashboardSwitcher from '../components/dashboard/DashboardSwitcher.vue'
import MobileSourceView from '../components/dashboard/MobileSourceView.vue'
import ClipPlayer from '../components/clips/ClipPlayer.vue'
import RecentReplayOverlay from '../components/clips/RecentReplayOverlay.vue'
import DashboardClipSearch from '../components/clips/DashboardClipSearch.vue'
import { useDashboardPlayback, dashboardPlaybackKey } from '../composables/useDashboardPlayback'
import { useEventStream } from '../composables/useEventStream'
import { useFittedBox, useIsPhone } from '../composables/useViewport'
import { useAuthStore } from '../stores/auth'
import { useCameraStore } from '../stores/cameras'
import { useClipStore } from '../stores/clips'
import { useDashboardStore } from '../stores/dashboards'
import { useSettingsStore } from '../stores/settings'
import { readingOrderCameras, type DashboardRecord } from '../types/dashboard'
import type { Clip } from '../types/clip'

// /d/:id (inside the app shell) and /kiosk/:id (chromeless wall display).
// All kiosk state is in the route, so the build-hash auto-reload restores it.
const route = useRoute()
const router = useRouter()
const auth = useAuthStore()
const cameraStore = useCameraStore()
const clipStore = useClipStore()
const store = useDashboardStore()
const settings = useSettingsStore()
const { connected } = useEventStream()
const phone = useIsPhone()

const kiosk = computed(() => route.meta.kiosk === true)
const dashboardId = computed(() => Number(route.params.id))
const record = ref<DashboardRecord | null>(null)
const loadState = ref<'loading' | 'ready' | 'missing' | 'error'>('loading')
const playingClip = ref<Clip | null>(null)
const replayCameraId = ref<number | null>(null)
const captureStartAt = ref<number | undefined>()
const searchingClips = ref(false)
const replayCamera = computed(() => cameraStore.getCameraById(replayCameraId.value ?? -1))
const available = computed(() => cameraStore.cameras.map(camera => camera.id))
const dashboardCameras = computed(() => record.value ? readingOrderCameras(record.value.document, available.value) : [])
const dashboardCameraSet = computed(() => new Set(dashboardCameras.value))

async function load(showSpinner = true) {
  if (!Number.isInteger(dashboardId.value) || dashboardId.value <= 0) { loadState.value = 'missing'; return }
  if (showSpinner) loadState.value = 'loading'
  try {
    const next = await store.get(dashboardId.value)
    if (!next) { record.value = null; loadState.value = 'missing'; return }
    record.value = next
    loadState.value = 'ready'
    store.rememberDashboard(next.id)
    // The dashboard's behaviour drives the shared playback settings.
    settings.dashboardReturnLiveSeconds = next.document.behavior.returnToLiveSeconds
    settings.dashboardQuietControls = next.document.behavior.quietControls
  } catch {
    // Keep showing the last good layout on transient failures (kiosks).
    if (!record.value) loadState.value = 'error'
  }
}
watch(dashboardId, () => load())
// Published changes re-lay out in place; mounted cameras keep their players.
watch(() => store.changes.get(dashboardId.value), announced => {
  if (announced !== undefined && announced !== record.value?.revision) void load(false)
})
watch(() => store.accessEpoch, () => void load(false))

// ----- Playback (live / DVR scrub-back) -----------------------------------
const playbackHeld = computed(() => searchingClips.value || playingClip.value !== null || replayCameraId.value !== null)
const playback = useDashboardPlayback(dashboardCameras, playbackHeld, (id, timestamp) => {
  captureStartAt.value = playback.get(id).requestedAt === null ? Math.floor(Date.now() / 1000) - 60 : timestamp
  replayCameraId.value = id
})
provide(dashboardPlaybackKey, playback)

// ----- Kiosk chrome ----------------------------------------------------------
const chromeVisible = ref(true)
let chromeTimer: ReturnType<typeof setTimeout> | null = null
function showChrome() {
  chromeVisible.value = true
  if (chromeTimer) clearTimeout(chromeTimer)
  chromeTimer = setTimeout(() => { chromeVisible.value = false; chromeTimer = null }, 2500)
}
let wakeLock: { release(): Promise<void> } | null = null
async function acquireWakeLock() {
  try {
    const nav = navigator as Navigator & { wakeLock?: { request(type: 'screen'): Promise<{ release(): Promise<void> }> } }
    if (kiosk.value && nav.wakeLock && document.visibilityState === 'visible') wakeLock = await nav.wakeLock.request('screen')
  } catch { /* not permitted; the display may sleep */ }
}
function onVisibility() { if (document.visibilityState === 'visible') void acquireWakeLock() }
// Long-session accounts renew their cookie via /auth/profile; refresh it
// periodically so an unattended display never reaches the 400-day limit.
let profileTimer: ReturnType<typeof setInterval> | null = null

function exitKiosk() {
  if (document.fullscreenElement) void document.exitFullscreen().catch(() => undefined)
  void router.push(`/d/${dashboardId.value}`)
}
function openKiosk() {
  void document.documentElement.requestFullscreen?.().catch(() => undefined)
  void router.push(`/kiosk/${dashboardId.value}`)
}
function switchDashboard(id: number | null) {
  if (id === null) { store.rememberDashboard(null); void router.push('/'); return }
  void router.push(kiosk.value ? `/kiosk/${id}` : `/d/${id}`)
}
function onCanvasEscape() {
  if (kiosk.value) exitKiosk()
}

// ----- Saving a personal copy ----------------------------------------------
const copying = ref(false)
async function saveCopy() {
  if (!record.value || copying.value) return
  copying.value = true
  try {
    // The copy is made from the filtered view, so it never carries cameras
    // the copier cannot see (placeholders are dropped by the server).
    const document = { ...record.value.document, tiles: record.value.document.tiles.filter(t => t.type !== 'unavailable') }
    const created = await store.create(`${record.value.name} (copy)`.slice(0, 80), document)
    void router.push(`/d/${created.id}/edit`)
  } finally { copying.value = false }
}

async function saveMobileOrder(order: number[]) {
  if (!record.value?.canEdit) return
  const document = { ...record.value.document, mobileOrder: order }
  const result = await store.save(record.value.id, record.value.revision, { document })
  if (result.ok) await load(false)
}

// Letterbox the canvas to the target aspect inside the app shell.
const stageRef = ref<HTMLElement | null>(null)
const { box, measure } = useFittedBox(() => stageRef.value,
  () => record.value ? record.value.document.target.width / record.value.document.target.height : 16 / 9)
watch(() => [record.value?.document.target.width, record.value?.document.target.height, loadState.value], () => requestAnimationFrame(measure))

onMounted(async () => {
  store.listen()
  window.addEventListener('pointermove', showChrome, { passive: true })
  window.addEventListener('pointerdown', showChrome, { passive: true })
  window.addEventListener('keydown', showChrome)
  document.addEventListener('visibilitychange', onVisibility)
  showChrome()
  void acquireWakeLock()
  profileTimer = setInterval(() => { void auth.fetchProfile() }, 6 * 3600 * 1000)
  if (cameraStore.cameras.length === 0) await cameraStore.fetchCameras()
  if (clipStore.recentClips.length === 0) void clipStore.fetchRecent(30)
  await load()
})
onUnmounted(() => {
  window.removeEventListener('pointermove', showChrome)
  window.removeEventListener('pointerdown', showChrome)
  window.removeEventListener('keydown', showChrome)
  document.removeEventListener('visibilitychange', onVisibility)
  if (chromeTimer) clearTimeout(chromeTimer)
  if (profileTimer) clearInterval(profileTimer)
  void wakeLock?.release().catch(() => undefined)
})
</script>

<template>
  <!-- Kiosk: chromeless, full viewport -->
  <div v-if="kiosk && !phone" class="kiosk" :class="{ 'cursor-hidden': !chromeVisible }">
    <DashboardCanvas v-if="record" :document="record.document" kiosk :chrome-visible="chromeVisible"
      @play="playingClip = $event" @escape="onCanvasEscape" />
    <div v-else class="kiosk-message">{{ loadState === 'missing' ? 'This dashboard is not available.' : loadState === 'error' ? 'Could not load the dashboard.' : 'Loading…' }}</div>
    <div class="kiosk-pill" :class="{ visible: chromeVisible }">
      <DashboardSwitcher :model-value="dashboardId" dark @update:model-value="switchDashboard" />
      <label class="kiosk-sync"><input type="checkbox" :checked="playback.sync.value" @change="playback.setSync(($event.target as HTMLInputElement).checked)"> Sync</label>
      <button type="button" @click="searchingClips = true">Search clips</button>
      <button v-if="record?.canEdit" type="button" @click="router.push(`/d/${dashboardId}/edit`)">Edit</button>
      <button type="button" @click="exitKiosk" title="Exit kiosk (Esc)">Exit</button>
    </div>
    <div v-if="!connected" class="kiosk-reconnecting" role="status">Reconnecting…</div>
  </div>

  <!-- In the app shell (and every phone) -->
  <AppLayout v-else wrap-actions>
    <template #title>{{ record?.name ?? 'Dashboard' }}</template>
    <template #actions>
      <div class="dashboard-toolbar d-flex align-items-center gap-2 flex-wrap">
        <DashboardSwitcher :model-value="dashboardId" allow-overview @update:model-value="switchDashboard" />
        <template v-if="!phone">
          <label class="dashboard-sync"><input type="checkbox" :checked="playback.sync.value" @change="playback.setSync(($event.target as HTMLInputElement).checked)"> Sync cameras</label>
          <button class="btn btn-sm btn-outline-secondary" type="button" @click="searchingClips = true">Search clips</button>
        </template>
        <RouterLink class="btn btn-sm btn-outline-secondary" :to="`/clips/dashboard/${dashboardId}`">Clips</RouterLink>
        <template v-if="record && !phone">
          <RouterLink v-if="record.canEdit" class="btn btn-sm btn-outline-secondary" :to="`/d/${dashboardId}/edit`">Edit</RouterLink>
          <button v-else class="btn btn-sm btn-outline-secondary" type="button" :disabled="copying" @click="saveCopy">Save a copy</button>
          <button class="btn btn-sm btn-primary" type="button" @click="openKiosk">Full screen</button>
        </template>
      </div>
    </template>

    <div v-if="loadState === 'loading' && !record" class="d-flex justify-content-center py-5"><div class="spinner-border text-primary" role="status"><span class="visually-hidden">Loading…</span></div></div>
    <div v-else-if="!record" class="ui-empty-state">
      <h2>{{ loadState === 'missing' ? 'Dashboard not available' : 'Could not load this dashboard' }}</h2>
      <p>{{ loadState === 'missing' ? 'It may have been deleted or is no longer shared with you.' : 'Check the connection and try again.' }}</p>
      <RouterLink class="btn btn-sm btn-primary" to="/dashboards">All dashboards</RouterLink>
    </div>
    <MobileSourceView v-else-if="phone" :camera-ids="dashboardCameras" :can-reorder="record.canEdit" @reorder="saveMobileOrder" />
    <template v-else>
      <p v-if="(record.redacted ?? 0) > 0" class="redacted-note" role="status">{{ record.redacted }} item{{ record.redacted === 1 ? '' : 's' }} on this dashboard {{ record.redacted === 1 ? 'is' : 'are' }} not available to your account.</p>
      <div ref="stageRef" class="dashboard-stage">
        <div class="dashboard-frame" :style="{ width: `${box.width}px`, height: `${box.height}px` }">
          <DashboardCanvas :document="record.document" @play="playingClip = $event" />
        </div>
      </div>
    </template>
  </AppLayout>

  <ClipPlayer v-if="playingClip" :clip="playingClip" @close="playingClip = null" />
  <RecentReplayOverlay v-if="replayCamera" :key="replayCamera.id" :camera-id="replayCamera.id"
    :camera-name="replayCamera.name" capture-only :start-at="captureStartAt" @close="replayCameraId = null" />
  <DashboardClipSearch v-if="searchingClips" :camera-ids="dashboardCameraSet" @close="searchingClips = false" />
</template>

<style scoped>
.kiosk { position: fixed; inset: 0; z-index: 1000; background: #05070b; }
.kiosk.cursor-hidden, .kiosk.cursor-hidden :deep(*) { cursor: none !important; }
.kiosk-message { display: flex; height: 100%; align-items: center; justify-content: center; color: var(--witness-muted, #a5b6cc); }
.kiosk-pill { position: fixed; top: 12px; right: 12px; z-index: 1010; display: flex; align-items: center; gap: 6px; padding: 5px; border: 1px solid rgba(255, 255, 255, .18); border-radius: 99px; background: rgba(12, 18, 28, .92); opacity: 0; pointer-events: none; transition: opacity .2s; }
.kiosk-pill.visible { opacity: 1; pointer-events: auto; }
.kiosk-pill button { border: 1px solid rgba(255, 255, 255, .25); border-radius: 99px; background: #142235; color: #dceafa; padding: 4px 11px; font-size: .75rem; }
.kiosk-sync { display: inline-flex; align-items: center; gap: 5px; color: #dceafa; font-size: .75rem; padding: 0 6px; }
.kiosk-reconnecting { position: fixed; left: 50%; bottom: 14px; transform: translateX(-50%); z-index: 1010; padding: 4px 12px; border-radius: 99px; background: rgba(120, 74, 18, .92); color: #ffe7c2; font-size: .75rem; }
.dashboard-sync { display: inline-flex; gap: 6px; align-items: center; font-size: 12px; white-space: nowrap; }
.dashboard-stage { display: flex; align-items: center; justify-content: center; height: max(420px, calc(100dvh - 190px)); }
.dashboard-frame { position: relative; flex: none; border-radius: 10px; overflow: hidden; box-shadow: 0 10px 34px #0007; }
.redacted-note { color: var(--witness-muted, #a5b6cc); font-size: .8rem; margin-bottom: .5rem; }
@media (prefers-reduced-motion: reduce) { .kiosk-pill { transition: none; } }
</style>
