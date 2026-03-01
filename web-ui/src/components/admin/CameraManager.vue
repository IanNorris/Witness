<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import { useCameraStore } from '../../stores/cameras'
import CameraEditorModal from './CameraEditorModal.vue'
import type { CameraFormData } from './CameraEditorModal.vue'
import ConfirmModal from '../common/ConfirmModal.vue'

const cameraStore = useCameraStore()
const loading = ref(true)

function maskPassword(url: string): string {
  return url.replace(/:([^/@]+)@/, ':****@')
}

interface AdminCamera {
  id: number
  displayName: string
  name: string
  description: string
  connectionString: string
  connectionStringSub: string
  status: string
  enabled: number
  skipFrames: number
  mdFrameHeight: number
  mdThreshold: number
  motionFilter: string
  blackoutMaskPath: string
  focusMaskPath: string
  groups: number[]
}

interface Group {
  id: number
  displayName: string
}

const cameras = ref<AdminCamera[]>([])
const groups = ref<Group[]>([])

// Editor modal state
const showEditor = ref(false)
const editingCamera = ref<CameraFormData | null>(null)

// Confirm modal state
const showConfirm = ref(false)
const confirmMessage = ref('')
const confirmAction = ref<(() => void) | null>(null)

async function fetchCameras() {
  loading.value = true
  try {
    const data = await api<AdminCamera[]>('/camera/admin_enum')
    cameras.value = data
  } finally {
    loading.value = false
  }
}

async function fetchGroups() {
  try {
    const data = await api<{ groups: Group[] }>('/group/enum')
    groups.value = data.groups ?? []
  } catch { /* groups are optional */ }
}

function openAdd() {
  editingCamera.value = null
  showEditor.value = true
}

function openEdit(cam: AdminCamera) {
  editingCamera.value = {
    id: cam.id,
    displayName: cam.displayName || cam.name,
    connectionString: cam.connectionString,
    connectionStringSub: cam.connectionStringSub ?? '',
    description: cam.description ?? '',
    enabled: cam.enabled,
    skipFrames: cam.skipFrames ?? 1,
    mdFrameHeight: cam.mdFrameHeight ?? 400,
    mdThreshold: cam.mdThreshold ?? 0.0001,
    motionFilter: cam.motionFilter ?? '',
    blackoutMaskPath: cam.blackoutMaskPath ?? '',
    focusMaskPath: cam.focusMaskPath ?? '',
    groups: [...(cam.groups ?? [])],
  }
  showEditor.value = true
}

async function onSave(data: CameraFormData) {
  showEditor.value = false
  const body = { ...data } as Record<string, unknown>
  if (data.id) {
    await api('/camera/admin_update', { method: 'POST', body })
    await api('/camera/set_groups', {
      method: 'POST',
      body: { camera: data.id, value: data.groups },
    })
  } else {
    await api('/camera/admin_create', { method: 'POST', body })
  }
  await fetchCameras()
  await cameraStore.fetchCameras()
}

function deleteCamera(cam: AdminCamera) {
  confirmMessage.value = `Delete camera "${cam.displayName || cam.name}"? All clips will be lost.`
  confirmAction.value = async () => {
    await api('/camera/admin_delete', { method: 'POST', body: { id: cam.id } })
    await fetchCameras()
    await cameraStore.fetchCameras()
  }
  showConfirm.value = true
}

function resetStats() {
  confirmMessage.value = 'Reset all camera stats?'
  confirmAction.value = async () => {
    await api('/camera/admin_reset_stats', { method: 'POST', body: {} })
    await fetchCameras()
  }
  showConfirm.value = true
}

function onConfirm() {
  showConfirm.value = false
  confirmAction.value?.()
}

onMounted(() => {
  fetchCameras()
  fetchGroups()
})
</script>

<template>
  <div>
    <div class="d-flex justify-content-between align-items-center mb-3">
      <h6 class="mb-0">Cameras</h6>
      <div>
        <button class="btn btn-sm btn-outline-secondary me-1" @click="resetStats">Reset Stats</button>
        <button class="btn btn-sm btn-primary" @click="openAdd">+ Add Camera</button>
      </div>
    </div>

    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <table v-else class="table table-dark table-sm align-middle">
      <thead>
        <tr>
          <th>ID</th>
          <th>Name</th>
          <th class="text-center">Enabled</th>
          <th>Status</th>
          <th>Connection</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="cam in cameras" :key="cam.id" :class="{ 'opacity-50': !cam.enabled }">
          <td>{{ cam.id }}</td>
          <td>{{ cam.displayName || cam.name }}</td>
          <td class="text-center">
            <span class="badge" :class="cam.enabled ? 'bg-success' : 'bg-secondary'">
              {{ cam.enabled ? 'Yes' : 'No' }}
            </span>
          </td>
          <td>
            <span class="badge" :class="cam.status === 'Connected' ? 'bg-success' : 'bg-danger'">
              {{ cam.status }}
            </span>
          </td>
          <td class="text-truncate small font-monospace" style="max-width: 250px;">{{ maskPassword(cam.connectionString) }}</td>
          <td>
            <button class="btn btn-sm btn-outline-secondary me-1" @click="openEdit(cam)">Edit</button>
            <button class="btn btn-sm btn-outline-danger" @click="deleteCamera(cam)">Delete</button>
          </td>
        </tr>
      </tbody>
    </table>

    <CameraEditorModal
      :show="showEditor"
      :camera="editingCamera"
      :available-groups="groups"
      @save="onSave"
      @cancel="showEditor = false"
    />
    <ConfirmModal
      :show="showConfirm"
      :message="confirmMessage"
      @confirm="onConfirm"
      @cancel="showConfirm = false"
    />
  </div>
</template>
