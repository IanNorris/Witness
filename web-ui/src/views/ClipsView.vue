<script setup lang="ts">
import { computed, onMounted, watch, ref } from 'vue'
import { useRoute } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import ClipCard from '../components/clips/ClipCard.vue'
import ClipPlayer from '../components/clips/ClipPlayer.vue'
import { useCameraStore } from '../stores/cameras'
import { useClipStore } from '../stores/clips'
import type { Clip } from '../types/clip'

const route = useRoute()
const cameraStore = useCameraStore()
const clipStore = useClipStore()

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
</script>

<template>
  <AppLayout>
    <template #title>{{ title }}</template>
    <template #actions>
      <span v-if="clipStore.totalCount" class="text-muted-custom small">
        {{ clipStore.totalCount }} clips
      </span>
    </template>

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
          v-for="clip in clipStore.clips"
          :key="clip.uid"
          :clip="clip"
          @play="handlePlay"
          @toggle-save="handleToggleSave"
          @delete="handleDeleteRequest"
          @retag="handleRetag"
        />
      </div>

      <!-- Pagination -->
      <nav v-if="clipStore.totalPages > 1" class="mt-3 d-flex justify-content-center">
        <ul class="pagination pagination-sm">
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
        </ul>
      </nav>
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
.clip-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
  gap: 1rem;
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
