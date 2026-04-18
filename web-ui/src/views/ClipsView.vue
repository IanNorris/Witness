<script setup lang="ts">
import { computed, onMounted, watch, ref } from 'vue'
import { useRoute } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import ClipCard from '../components/clips/ClipCard.vue'
import ClipPlayer from '../components/clips/ClipPlayer.vue'
import ActivityStrip from '../components/clips/ActivityStrip.vue'
import ActivityTimeline from '../components/clips/ActivityTimeline.vue'
import ClipFilters from '../components/clips/ClipFilters.vue'
import TrailsPanel from '../components/clips/TrailsPanel.vue'
import { useCameraStore } from '../stores/cameras'
import { useClipStore } from '../stores/clips'
import { useSettingsStore } from '../stores/settings'
import { useFilterStore } from '../stores/filters'
import { useGroupStore } from '../stores/groups'
import type { Clip } from '../types/clip'

const route = useRoute()
const cameraStore = useCameraStore()
const clipStore = useClipStore()
const settings = useSettingsStore()
const filterStore = useFilterStore()
const groupStore = useGroupStore()

const playingClip = ref<Clip | null>(null)
const confirmDelete = ref<Clip | null>(null)
const mobileFiltersOpen = ref(false)

// Trail integration state
const trailFilterClipIds = ref<number[] | null>(null)
const highlightedClipId = ref<number | null>(null)

// Route mode detection
const cameraId = computed(() =>
  route.params.cameraId ? Number(route.params.cameraId) : null
)
const groupId = computed(() =>
  route.params.groupId ? Number(route.params.groupId) : null
)
const isGroupMode = computed(() => groupId.value !== null)

const groupCameras = computed(() => {
  if (!groupId.value) return []
  return groupStore.camerasInGroup(groupId.value)
})

const groupCameraIds = computed(() =>
  new Set(groupCameras.value.map(c => c.id))
)

const title = computed(() => {
  if (isGroupMode.value && groupId.value) {
    const group = groupStore.getGroupById(groupId.value)
    return group ? group.displayName : 'Group'
  }
  if (cameraId.value) {
    const cam = cameraStore.getCameraById(cameraId.value)
    return cam ? `Clips — ${cam.name}` : 'Clips'
  }
  return 'All Clips'
})

// Pagination display
const pageNumbers = computed(() => {
  const total = clipStore.totalPages
  const current = clipStore.currentPage
  const pages: number[] = []
  const start = Math.max(0, current - 2)
  const end = Math.min(total - 1, current + 2)
  for (let i = start; i <= end; i++) pages.push(i)
  return pages
})

async function loadClips() {
  if (cameraStore.cameras.length === 0) {
    await cameraStore.fetchCameras()
  }
  // For group mode, fetch all clips (we'll filter client-side by group cameras)
  const camId = isGroupMode.value ? null : cameraId.value
  await clipStore.fetchClips(camId, 0)
}

// If navigated with ?t= query param (e.g. from trails view), set time range around that timestamp
function applyTimestampQuery() {
  const t = route.query.t
  if (t) {
    const ts = Number(t)
    if (ts > 0) {
      filterStore.setTimeRange(ts - 3600, ts + 3600)
    }
  }
}

watch(cameraId, () => loadClips())
watch(groupId, () => { trailFilterClipIds.value = null; loadClips() })
watch(() => filterStore.filterQueryString, () => loadClips())
watch(() => filterStore.timeRange, () => loadClips())
onMounted(async () => {
  applyTimestampQuery()
  if (route.query.trails === '1') {
    settings.showTrails = true
  }
  if (groupStore.groups.length === 0) {
    await groupStore.fetchGroups()
  }
  if (!route.query.t) {
    await loadClips()
  }
})

function handlePlay(clip: Clip) {
  playingClip.value = clip
}

async function handleToggleSave(clip: Clip) {
  await clipStore.toggleSave(clip.uid, !clip.saved)
}

function handleDeleteRequest(clip: Clip) {
  confirmDelete.value = clip
}

async function handleDeleteConfirm() {
  if (confirmDelete.value) {
    await clipStore.deleteClip(confirmDelete.value.uid)
    confirmDelete.value = null
  }
}

async function handleRetag(clip: Clip) {
  await clipStore.retagClip(clip.uid)
}

const TRIVIAL_DURATION = 2

const displayedClips = computed(() => {
  let clips = clipStore.clips
  // Filter by group cameras
  if (isGroupMode.value && groupCameraIds.value.size > 0) {
    clips = clips.filter(c => groupCameraIds.value.has(c.camera))
  }
  if (settings.hideShortClips) {
    clips = clips.filter(c => c.duration >= TRIVIAL_DURATION)
  }
  if (trailFilterClipIds.value) {
    const ids = new Set(trailFilterClipIds.value)
    clips = clips.filter(c => ids.has(c.uid))
  }
  return clips
})

function isTrivialClip(clip: Clip) {
  return clip.duration < TRIVIAL_DURATION
}

const pageSizeOptions = [6, 12, 24, 48, 100]

function changePageSize(event: Event) {
  const val = Number((event.target as HTMLSelectElement).value)
  settings.clipsPerPage = val
  const camId = isGroupMode.value ? null : cameraId.value
  clipStore.fetchClips(camId, 0)
}

function handleTagClick(_tag: string) {
  // Future: filter by tag
}

function onTrailRegionClipIds(ids: number[]) {
  trailFilterClipIds.value = ids.length > 0 ? ids : null
}

function onTrailHoverClipId(id: number | null) {
  highlightedClipId.value = id
}

function clearTrailFilter() {
  trailFilterClipIds.value = null
}

function toggleTrails() {
  settings.showTrails = !settings.showTrails
}

const reprocessingAll = ref(false)
const reprocessedCount = ref<number | null>(null)

async function handleRetagAll() {
  reprocessingAll.value = true
  reprocessedCount.value = null
  try {
    const count = await clipStore.retagAll()
    reprocessedCount.value = count
    setTimeout(() => { reprocessedCount.value = null }, 5000)
  } finally {
    reprocessingAll.value = false
  }
}
</script>

<template>
  <AppLayout>
    <template #title>{{ title }}</template>
    <template #actions>
      <div class="d-flex align-items-center gap-3">
        <button class="btn btn-sm btn-outline-secondary" @click="loadClips" title="Refresh">↻</button>
        <button
          class="btn btn-sm"
          :class="settings.showTrails ? 'btn-primary' : 'btn-outline-secondary'"
          @click="toggleTrails"
          title="Toggle trail overlay"
        >
          Trails
        </button>
        <template v-if="settings.showTrails && isGroupMode">
          <button class="btn btn-sm btn-outline-secondary" @click="settings.trailColumnWidth = Math.max(200, settings.trailColumnWidth - 50)" title="Shrink trails">−</button>
          <button class="btn btn-sm btn-outline-secondary" @click="settings.trailColumnWidth = Math.min(800, settings.trailColumnWidth + 50)" title="Enlarge trails">+</button>
        </template>
        <button
          class="btn btn-sm btn-outline-warning"
          @click="handleRetagAll"
          :disabled="reprocessingAll"
          title="Reprocess all clips in current view"
        >
          <span v-if="reprocessingAll" class="spinner-border spinner-border-sm me-1"></span>
          {{ reprocessedCount !== null ? `${reprocessedCount} queued` : 'Reprocess All' }}
        </button>
        <div class="form-check form-switch mb-0 mobile-hide">
          <input
            class="form-check-input"
            type="checkbox"
            id="hideShortClips"
            v-model="settings.hideShortClips"
          />
          <label class="form-check-label small text-muted-custom" for="hideShortClips">
            Hide short clips
          </label>
        </div>
        <select
          class="form-select form-select-sm page-size-select mobile-hide"
          :value="settings.clipsPerPage"
          @change="changePageSize"
        >
          <option v-for="n in pageSizeOptions" :key="n" :value="n">{{ n }} per page</option>
        </select>
        <span v-if="clipStore.totalCount" class="text-muted-custom small mobile-hide">
          {{ clipStore.totalCount }} clips
        </span>
        <button
          class="btn btn-sm btn-outline-secondary mobile-filter-btn"
          @click="mobileFiltersOpen = !mobileFiltersOpen"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="22 3 2 3 10 12.46 10 19 14 21 14 12.46 22 3"/></svg>
          <span v-if="filterStore.activeFilterCount > 0" class="filter-badge">{{ filterStore.activeFilterCount }}</span>
        </button>
      </div>
    </template>

    <!-- Activity Strip -->
    <ActivityStrip @play="handlePlay" />

    <!-- Activity Timeline -->
    <ActivityTimeline />

    <!-- Trails Panel(s) -->
    <!-- Single camera mode: one panel -->
    <TrailsPanel
      v-if="cameraId && !isGroupMode"
      :camera-id="cameraId"
      :visible="settings.showTrails"
      @region-clip-ids="onTrailRegionClipIds"
      @hover-clip-id="onTrailHoverClipId"
    />
    <!-- Group mode: side-by-side panels for each camera -->
    <div v-if="isGroupMode && settings.showTrails && groupCameras.length > 0" class="trails-row">
      <div
        v-for="cam in groupCameras"
        :key="cam.id"
        class="trails-row-item"
        :style="{ minWidth: settings.trailColumnWidth + 'px' }"
      >
        <div class="trails-row-label small text-muted-custom">{{ cam.name }}</div>
        <TrailsPanel
          :camera-id="cam.id"
          :visible="true"
          @region-clip-ids="onTrailRegionClipIds"
          @hover-clip-id="onTrailHoverClipId"
        />
      </div>
    </div>

    <!-- Trail filter indicator -->
    <div v-if="trailFilterClipIds" class="trail-filter-indicator">
      <span class="small">Filtered by trail region ({{ trailFilterClipIds.length }} clips)</span>
      <button class="btn btn-sm btn-link text-muted-custom p-0 ms-2" @click="clearTrailFilter">✕ Clear</button>
    </div>

    <!-- Main content: sidebar + clips -->
    <div class="clips-layout">
      <div class="clips-main">
        <!-- Loading -->
        <div v-if="clipStore.loading" class="text-center py-5">
          <div class="spinner-border text-primary" role="status">
            <span class="visually-hidden">Loading...</span>
          </div>
        </div>

        <!-- Empty state -->
        <div v-else-if="clipStore.clips.length === 0" class="text-muted-custom text-center py-5">
          No clips found
        </div>

        <!-- Clip grid -->
        <div v-else>
          <div class="clip-grid">
            <ClipCard
              v-for="clip in displayedClips"
              :key="clip.uid"
              :clip="clip"
              :trivial="isTrivialClip(clip)"
              :highlighted="clip.uid === highlightedClipId"
              @play="handlePlay"
              @toggle-save="handleToggleSave"
              @delete="handleDeleteRequest"
              @retag="handleRetag"
              @tag-click="handleTagClick"
            />
          </div>

          <!-- Pagination -->
          <nav v-if="clipStore.totalPages > 1" class="mt-3 mb-5 d-flex justify-content-center">
            <ul class="pagination pagination-sm">
              <li class="page-item" :class="{ disabled: clipStore.currentPage === 0 }">
                <a class="page-link" href="#" @click.prevent="clipStore.goToPage(0)">«</a>
              </li>
              <li class="page-item" :class="{ disabled: clipStore.currentPage === 0 }">
                <a class="page-link" href="#" @click.prevent="clipStore.prevPage()">‹</a>
              </li>
              <li
                v-for="p in pageNumbers"
                :key="p"
                class="page-item"
                :class="{ active: p === clipStore.currentPage }"
              >
                <a class="page-link" href="#" @click.prevent="clipStore.goToPage(p)">{{ p + 1 }}</a>
              </li>
              <li class="page-item" :class="{ disabled: clipStore.currentPage >= clipStore.totalPages - 1 }">
                <a class="page-link" href="#" @click.prevent="clipStore.nextPage()">›</a>
              </li>
              <li class="page-item" :class="{ disabled: clipStore.currentPage >= clipStore.totalPages - 1 }">
                <a class="page-link" href="#" @click.prevent="clipStore.goToPage(clipStore.totalPages - 1)">»</a>
              </li>
            </ul>
          </nav>
        </div>
      </div>

      <aside class="clips-sidebar clips-sidebar-desktop">
        <ClipFilters />
      </aside>
    </div>

    <!-- Mobile filter drawer -->
    <Teleport to="body">
      <Transition name="filter-drawer">
        <div v-if="mobileFiltersOpen" class="mobile-filter-overlay" @click.self="mobileFiltersOpen = false">
          <div class="mobile-filter-sheet">
            <div class="mobile-filter-sheet-header">
              <span class="fw-semibold">Filters</span>
              <button class="btn btn-sm btn-outline-secondary" @click="mobileFiltersOpen = false">✕</button>
            </div>
            <ClipFilters hide-title />
          </div>
        </div>
      </Transition>
    </Teleport>

    <!-- Video player modal -->
    <ClipPlayer v-if="playingClip" :clip="playingClip" @close="playingClip = null" />

    <!-- Delete confirmation -->
    <Teleport to="body" v-if="confirmDelete">
      <div class="clip-modal-overlay" @click.self="confirmDelete = null">
        <div class="confirm-dialog">
          <p>Delete this clip?</p>
          <div class="d-flex gap-2 justify-content-end">
            <button class="btn btn-sm btn-outline-secondary" @click="confirmDelete = null">Cancel</button>
            <button class="btn btn-sm btn-danger" @click="handleDeleteConfirm">Delete</button>
          </div>
        </div>
      </div>
    </Teleport>
  </AppLayout>
</template>

<style scoped>
.clips-layout {
  display: flex;
  gap: 1rem;
  align-items: flex-start;
}
.clips-sidebar {
  flex: 0 0 200px;
  position: sticky;
  top: 1rem;
}
.clips-main {
  flex: 1;
  min-width: 0;
}

.clip-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
  gap: 1rem;
}

.page-size-select {
  width: auto;
  min-width: 120px;
  background-color: var(--bs-dark, #1e1e2e);
  color: var(--bs-body-color, #e1e4e8);
  border-color: var(--bs-border-color, #333);
}

.mobile-filter-btn {
  display: none;
  position: relative;
}
.filter-badge {
  position: absolute;
  top: -4px;
  right: -4px;
  background: #2563eb;
  color: #fff;
  font-size: 0.6rem;
  min-width: 16px;
  height: 16px;
  border-radius: 8px;
  display: flex;
  align-items: center;
  justify-content: center;
  line-height: 1;
}

.mobile-filter-overlay {
  position: fixed;
  inset: 0;
  z-index: 9000;
  background: rgba(0, 0, 0, 0.6);
  display: flex;
  align-items: flex-end;
  justify-content: center;
}
.mobile-filter-sheet {
  background: var(--bs-dark, #1e1e2e);
  border-top: 1px solid var(--bs-border-color, #333);
  border-radius: 1rem 1rem 0 0;
  width: 100%;
  max-height: 70vh;
  overflow-y: auto;
  padding: 1rem;
}
.mobile-filter-sheet-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 0.75rem;
}

.filter-drawer-enter-active, .filter-drawer-leave-active {
  transition: opacity 0.2s ease;
}
.filter-drawer-enter-active .mobile-filter-sheet,
.filter-drawer-leave-active .mobile-filter-sheet {
  transition: transform 0.25s ease;
}
.filter-drawer-enter-from, .filter-drawer-leave-to {
  opacity: 0;
}
.filter-drawer-enter-from .mobile-filter-sheet,
.filter-drawer-leave-to .mobile-filter-sheet {
  transform: translateY(100%);
}

@media (max-width: 768px), (pointer: coarse) and (max-width: 1024px) {
  .clips-sidebar-desktop {
    display: none;
  }

  .mobile-filter-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    min-width: 36px;
    min-height: 36px;
  }

  .clip-grid {
    grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
    gap: 0.5rem;
  }
}

.clip-modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9998;
}
.confirm-dialog {
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  padding: 1.5rem;
  min-width: 300px;
}

.trail-filter-indicator {
  display: flex;
  align-items: center;
  padding: 0.375rem 0.75rem;
  background: rgba(37, 99, 235, 0.1);
  border: 1px solid rgba(37, 99, 235, 0.3);
  border-radius: 0.375rem;
  margin-bottom: 0.75rem;
}

.trails-row {
  display: flex;
  gap: 0.5rem;
  overflow-x: auto;
  padding-bottom: 0.5rem;
  margin-bottom: 0.75rem;
}

.trails-row-item {
  flex: 0 0 auto;
  display: flex;
  flex-direction: column;
}

.trails-row-label {
  padding: 0.25rem 0.25rem 0;
  font-weight: 500;
  opacity: 0.7;
}
</style>
