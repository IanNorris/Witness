<script setup lang="ts">
import { computed, onMounted, watch, ref } from 'vue'
import { useRoute } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import ClipCard from '../components/clips/ClipCard.vue'
import ClipPlayer from '../components/clips/ClipPlayer.vue'
import ActivityStrip from '../components/clips/ActivityStrip.vue'
import ActivityTimeline from '../components/clips/ActivityTimeline.vue'
import ClipFilters from '../components/clips/ClipFilters.vue'
import { useCameraStore } from '../stores/cameras'
import { useClipStore } from '../stores/clips'
import { useSettingsStore } from '../stores/settings'
import { useFilterStore } from '../stores/filters'
import type { Clip } from '../types/clip'

const route = useRoute()
const cameraStore = useCameraStore()
const clipStore = useClipStore()
const settings = useSettingsStore()
const filterStore = useFilterStore()

const playingClip = ref<Clip | null>(null)
const confirmDelete = ref<Clip | null>(null)

const cameraId = computed(() =>
  route.params.cameraId ? Number(route.params.cameraId) : null
)

const title = computed(() => {
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
  await clipStore.fetchClips(cameraId.value, 0)
}

watch(cameraId, () => loadClips())
watch(() => filterStore.filterQueryString, () => loadClips())
onMounted(() => loadClips())

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
  if (!settings.hideShortClips) return clipStore.clips
  return clipStore.clips.filter(c => c.duration >= TRIVIAL_DURATION)
})

function isTrivialClip(clip: Clip) {
  return clip.duration < TRIVIAL_DURATION
}

const pageSizeOptions = [6, 12, 24, 48, 100]

function changePageSize(event: Event) {
  const val = Number((event.target as HTMLSelectElement).value)
  settings.clipsPerPage = val
  clipStore.fetchClips(cameraId.value, 0)
}

function handleTagClick(_tag: string) {
  // Future: filter by tag
}

function handleTimelinePlay(clip: { uid: number; camera: number; timestamp: number; duration: number; tags: string }) {
  playingClip.value = {
    uid: clip.uid,
    camera: clip.camera,
    timestamp: clip.timestamp,
    duration: clip.duration,
    tags: clip.tags,
    cameraName: '',
    saved: false,
    recordMode: '',
    description: '',
    detectionVersion: 0,
    lighting: 0,
    reviewed: false,
  } as Clip
}
</script>

<template>
  <AppLayout>
    <template #title>{{ title }}</template>
    <template #actions>
      <div class="d-flex align-items-center gap-3">
        <button class="btn btn-sm btn-outline-secondary" @click="loadClips" title="Refresh">↻</button>
        <div class="form-check form-switch mb-0">
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
          class="form-select form-select-sm page-size-select"
          :value="settings.clipsPerPage"
          @change="changePageSize"
        >
          <option v-for="n in pageSizeOptions" :key="n" :value="n">{{ n }} per page</option>
        </select>
        <span v-if="clipStore.totalCount" class="text-muted-custom small">
          {{ clipStore.totalCount }} clips
        </span>
      </div>
    </template>

    <!-- Activity Strip -->
    <ActivityStrip @play="handlePlay" />

    <!-- Activity Timeline -->
    <ActivityTimeline @play="handleTimelinePlay" />

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
              @play="handlePlay"
              @toggle-save="handleToggleSave"
              @delete="handleDeleteRequest"
              @retag="handleRetag"
              @tag-click="handleTagClick"
            />
          </div>

          <!-- Pagination -->
          <nav v-if="clipStore.totalPages > 1" class="mt-3 d-flex justify-content-center">
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

      <aside class="clips-sidebar">
        <ClipFilters />
      </aside>
    </div>

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
</style>
