<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import { useCameraStore } from '../../stores/cameras'

const cameraStore = useCameraStore()
const loading = ref(true)

interface AdminCamera {
  id: number
  displayName: string
  name: string
  description: string
  connectionString: string
  status: string
  groups: number[]
}

const cameras = ref<AdminCamera[]>([])
const showAddForm = ref(false)
const newCamera = ref({ displayName: '', connectionString: '', connectionStringSub: '', description: '' })

async function fetchCameras() {
  loading.value = true
  try {
    const data = await api<AdminCamera[]>('/camera/admin_enum')
    cameras.value = data
  } finally {
    loading.value = false
  }
}

async function addCamera() {
  if (!newCamera.value.displayName || !newCamera.value.connectionString) return
  await api('/camera/admin_create', { method: 'POST', body: newCamera.value })
  newCamera.value = { displayName: '', connectionString: '', connectionStringSub: '', description: '' }
  showAddForm.value = false
  await fetchCameras()
  await cameraStore.fetchCameras()
}

async function deleteCamera(cam: AdminCamera) {
  if (!confirm(`Delete camera "${cam.displayName}"? All clips will be lost.`)) return
  await api('/camera/admin_delete', { method: 'POST', body: { id: cam.id } })
  await fetchCameras()
  await cameraStore.fetchCameras()
}

async function resetStats() {
  if (!confirm('Reset all camera stats?')) return
  await api('/camera/admin_reset_stats', { method: 'POST', body: {} })
  await fetchCameras()
}

onMounted(fetchCameras)
</script>

<template>
  <div>
    <div class="d-flex justify-content-between align-items-center mb-3">
      <h6 class="mb-0">Cameras</h6>
      <div>
        <button class="btn btn-sm btn-outline-secondary me-1" @click="resetStats">Reset Stats</button>
        <button class="btn btn-sm btn-primary" @click="showAddForm = !showAddForm">
          {{ showAddForm ? 'Cancel' : '+ Add Camera' }}
        </button>
      </div>
    </div>

    <!-- Add camera form -->
    <div v-if="showAddForm" class="card bg-dark mb-3">
      <div class="card-body">
        <div class="row g-2">
          <div class="col-md-4">
            <label class="form-label small">Name</label>
            <input v-model="newCamera.displayName" class="form-control form-control-sm" placeholder="Front Door" />
          </div>
          <div class="col-md-8">
            <label class="form-label small">RTSP URL (Main)</label>
            <input v-model="newCamera.connectionString" class="form-control form-control-sm" placeholder="rtsp://user:pass@192.168.1.x/stream1" />
          </div>
          <div class="col-md-8">
            <label class="form-label small">RTSP URL (Sub-stream, optional)</label>
            <input v-model="newCamera.connectionStringSub" class="form-control form-control-sm" placeholder="rtsp://user:pass@192.168.1.x/stream2" />
          </div>
          <div class="col-md-4">
            <label class="form-label small">Description</label>
            <input v-model="newCamera.description" class="form-control form-control-sm" />
          </div>
          <div class="col-12">
            <button class="btn btn-sm btn-primary" @click="addCamera">Create Camera</button>
          </div>
        </div>
      </div>
    </div>

    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <table v-else class="table table-dark table-sm">
      <thead>
        <tr>
          <th>ID</th>
          <th>Name</th>
          <th>Status</th>
          <th>Connection</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="cam in cameras" :key="cam.id">
          <td>{{ cam.id }}</td>
          <td>{{ cam.displayName || cam.name }}</td>
          <td>
            <span class="badge" :class="cam.status === 'Connected' ? 'bg-success' : 'bg-danger'">
              {{ cam.status }}
            </span>
          </td>
          <td class="text-truncate small" style="max-width: 300px;">{{ cam.connectionString }}</td>
          <td>
            <button class="btn btn-sm btn-outline-danger" @click="deleteCamera(cam)">Delete</button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>
