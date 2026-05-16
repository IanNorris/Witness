<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import { useCameraStore } from '../../stores/cameras'
import CameraEditorModal from './CameraEditorModal.vue'
import AddCameraWizard from './AddCameraWizard.vue'
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
  continuousRecording: number
  lowLatencyHLS: number
  groups: number[]
  ptzEnabled?: number
  ptzApiHost?: string
  ptzApiPort?: number
  ptzUsername?: string
  ptzPassword?: string
  linkedCameraId?: number
  frameCount?: number
  processingTimeOfEachMS?: number
  processingActualMS?: number
  scaleTimeOfEachMS?: number
  jpegEncodingTimeOfEachMS?: number
  observerTimeOfEachMS?: number
  firstPassFilterTimeOfEachMS?: number
  secondPassFilterTimeOfEachMS?: number
  thirdPassFilterTimeOfEachMS?: number
  streamReadTimeMS?: number
  streamDecodeTimeMS?: number
  streamOutputTimeMS?: number
}

interface Group {
  id: number
  displayName: string
}

const cameras = ref<AdminCamera[]>([])
const groups = ref<Group[]>([])
const expandedDiag = ref<number | null>(null)

// Wizard state
const showWizard = ref(false)

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
  showWizard.value = true
}

async function onWizardCreate(data: { displayName: string; connectionString: string; connectionStringSub: string; description: string }) {
  showWizard.value = false
  await api('/camera/admin_create', { method: 'POST', body: data as Record<string, unknown> })
  await fetchCameras()
  await cameraStore.fetchCameras()
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
    continuousRecording: cam.continuousRecording ?? 0,
    lowLatencyHLS: cam.lowLatencyHLS ?? 0,
    groups: [...(cam.groups ?? [])],
    ptzEnabled: cam.ptzEnabled ?? 0,
    ptzApiHost: cam.ptzApiHost ?? '',
    ptzApiPort: cam.ptzApiPort ?? 80,
    ptzUsername: cam.ptzUsername ?? '',
    ptzPassword: cam.ptzPassword ?? '',
    linkedCameraId: cam.linkedCameraId ?? 0,
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

function toggleDiag(camId: number) {
  expandedDiag.value = expandedDiag.value === camId ? null : camId
}

function fmtMs(val?: number): string {
  if (val == null || val === 0) return '—'
  return val < 1 ? `${(val * 1000).toFixed(0)}µs` : `${val.toFixed(2)}ms`
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
        <template v-for="cam in cameras" :key="cam.id">
          <tr :class="{ 'opacity-50': !cam.enabled }">
            <td>{{ cam.id }}</td>
            <td>{{ cam.displayName || cam.name }}</td>
            <td class="text-center">
              <span class="badge" :class="cam.enabled ? 'bg-success' : 'bg-secondary'">
                {{ cam.enabled ? 'Yes' : 'No' }}
              </span>
            </td>
            <td>
              <span
                class="badge"
                :class="cam.status === 'Connected' ? 'bg-success' : 'bg-danger'"
                style="cursor: pointer"
                @click="toggleDiag(cam.id)"
                :title="cam.frameCount ? `${cam.frameCount} frames — click for details` : 'Click for details'"
              >
                {{ cam.status }}
                <span v-if="cam.frameCount" class="ms-1 opacity-75">⚙</span>
              </span>
            </td>
            <td class="text-truncate small font-monospace" style="max-width: 250px;">{{ maskPassword(cam.connectionString) }}</td>
            <td>
              <button class="btn btn-sm btn-outline-secondary me-1" @click="openEdit(cam)">Edit</button>
              <button class="btn btn-sm btn-outline-danger" @click="deleteCamera(cam)">Delete</button>
            </td>
          </tr>
          <!-- Expanded diagnostics row -->
          <tr v-if="expandedDiag === cam.id && cam.frameCount" class="diag-row">
            <td colspan="6" class="px-3 py-2">
              <div class="row g-3 small">
                <div class="col-md-4">
                  <div class="fw-bold mb-1">Processing Pipeline</div>
                  <table class="table table-dark table-sm table-borderless mb-0 diag-table">
                    <tr><td class="text-muted">Total frames</td><td>{{ cam.frameCount?.toLocaleString() }}</td></tr>
                    <tr><td class="text-muted">Processing</td><td>{{ fmtMs(cam.processingTimeOfEachMS) }}</td></tr>
                    <tr><td class="text-muted">Scaling</td><td>{{ fmtMs(cam.scaleTimeOfEachMS) }}</td></tr>
                    <tr><td class="text-muted">JPEG encoding</td><td>{{ fmtMs(cam.jpegEncodingTimeOfEachMS) }}</td></tr>
                  </table>
                </div>
                <div class="col-md-4">
                  <div class="fw-bold mb-1">Detection Filters</div>
                  <table class="table table-dark table-sm table-borderless mb-0 diag-table">
                    <tr><td class="text-muted">Observer</td><td>{{ fmtMs(cam.observerTimeOfEachMS) }}</td></tr>
                    <tr><td class="text-muted">1st pass</td><td>{{ fmtMs(cam.firstPassFilterTimeOfEachMS) }}</td></tr>
                    <tr><td class="text-muted">2nd pass (ONNX)</td><td>{{ fmtMs(cam.secondPassFilterTimeOfEachMS) }}</td></tr>
                    <tr><td class="text-muted">3rd pass</td><td>{{ fmtMs(cam.thirdPassFilterTimeOfEachMS) }}</td></tr>
                  </table>
                </div>
                <div class="col-md-4">
                  <div class="fw-bold mb-1">Stream Decode</div>
                  <table class="table table-dark table-sm table-borderless mb-0 diag-table">
                    <tr><td class="text-muted">Read</td><td>{{ fmtMs(cam.streamReadTimeMS) }}</td></tr>
                    <tr><td class="text-muted">Decode</td><td>{{ fmtMs(cam.streamDecodeTimeMS) }}</td></tr>
                    <tr><td class="text-muted">Output</td><td>{{ fmtMs(cam.streamOutputTimeMS) }}</td></tr>
                  </table>
                </div>
              </div>
            </td>
          </tr>
        </template>
      </tbody>
    </table>

    <AddCameraWizard
      :show="showWizard"
      @create="onWizardCreate"
      @cancel="showWizard = false"
    />
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

<style scoped>
.diag-row td {
  background: rgba(0, 0, 0, 0.3) !important;
  border-top: 1px solid rgba(255, 255, 255, 0.05);
}
.diag-table td {
  padding: 0.1rem 0.4rem !important;
  font-size: 0.75rem;
}
</style>
