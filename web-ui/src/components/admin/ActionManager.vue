<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import { useCameraStore } from '../../stores/cameras'
import ConfirmModal from '../common/ConfirmModal.vue'

interface Action {
  id: number
  name: string
  command: string
  param1: string
  param2: string
  param3: string
}

interface CameraActionAssignment {
  id: number
  actionId: number
  cameraId: number
  mdThreshold: number
  detectionClass: string
}

interface SoundFile {
  file: string
  name: string
}

const cameraStore = useCameraStore()
const actions = ref<Action[]>([])
const assignments = ref<CameraActionAssignment[]>([])
const sounds = ref<SoundFile[]>([])
const loading = ref(true)

// New action form
const newName = ref('')
const newSound = ref('')

// New assignment form
const assignActionId = ref(0)
const assignCameraId = ref(0)
const assignClass = ref('')
const assignMdThreshold = ref(0.05)

// Editing
const editingId = ref<number | null>(null)
const editName = ref('')
const editSound = ref('')

// Delete confirmation
const showConfirm = ref(false)
const confirmMessage = ref('')
const confirmAction = ref<(() => void) | null>(null)

const detectionClasses = ['person', 'face', 'known_face', 'unknown_face', 'car', 'truck', 'dog', 'cat', 'bicycle', 'motorcycle']

function assignmentsForAction(actionId: number) {
  return assignments.value.filter(a => a.actionId === actionId)
}

function cameraName(id: number) {
  return cameraStore.getCameraById(id)?.name ?? `Camera ${id}`
}

async function fetchAll() {
  loading.value = true
  try {
    const [actionData, soundData] = await Promise.all([
      api<{ actions: Action[]; assignments: CameraActionAssignment[] }>('/action/enum'),
      api<{ sounds: SoundFile[] }>('/action/sounds'),
    ])
    actions.value = actionData.actions ?? []
    assignments.value = actionData.assignments ?? []
    sounds.value = soundData.sounds ?? []

    if (cameraStore.cameras.length === 0) {
      await cameraStore.fetchCameras()
    }
  } finally {
    loading.value = false
  }
}

async function createAction() {
  if (!newName.value.trim() || !newSound.value) return
  await api('/action/create', {
    method: 'POST',
    body: { name: newName.value.trim(), command: 'PlaySound', param1: newSound.value, param2: '', param3: '' },
  })
  newName.value = ''
  newSound.value = ''
  await fetchAll()
}

function startEdit(action: Action) {
  editingId.value = action.id
  editName.value = action.name
  editSound.value = action.param1
}

function cancelEdit() {
  editingId.value = null
}

async function saveEdit(action: Action) {
  await api('/action/update', {
    method: 'POST',
    body: { id: action.id, name: editName.value, command: 'PlaySound', param1: editSound.value, param2: '', param3: '' },
  })
  editingId.value = null
  await fetchAll()
}

function confirmDelete(action: Action) {
  confirmMessage.value = `Delete action "${action.name}" and all its camera assignments?`
  confirmAction.value = async () => {
    await api('/action/delete', { method: 'POST', body: { id: action.id } })
    await fetchAll()
  }
  showConfirm.value = true
}

async function addAssignment() {
  if (!assignActionId.value || !assignCameraId.value || !assignClass.value) return
  await api('/action/assign', {
    method: 'POST',
    body: {
      actionId: assignActionId.value,
      cameraId: assignCameraId.value,
      detectionClass: assignClass.value,
      mdThreshold: assignMdThreshold.value,
    },
  })
  assignClass.value = ''
  await fetchAll()
}

async function removeAssignment(id: number) {
  await api('/action/unassign', { method: 'POST', body: { id } })
  await fetchAll()
}

async function testSound(file: string) {
  await api('/action/test_sound', { method: 'POST', body: { file } })
}

function soundDisplayName(file: string) {
  const s = sounds.value.find(s => s.file === file)
  return s?.name ?? file
}

function onConfirm() {
  showConfirm.value = false
  confirmAction.value?.()
}

onMounted(fetchAll)
</script>

<template>
  <div>
    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <div v-else>
      <!-- Create new action -->
      <div class="card bg-dark border-secondary mb-4">
        <div class="card-body">
          <h6 class="card-title mb-3">Create Action</h6>
          <div class="row g-2 align-items-end">
            <div class="col-md-4">
              <label class="form-label small">Name</label>
              <input v-model="newName" type="text" class="form-control form-control-sm" placeholder="e.g. FrontDoorAlert" />
            </div>
            <div class="col-md-4">
              <label class="form-label small">Sound</label>
              <div class="input-group input-group-sm">
                <select v-model="newSound" class="form-select form-select-sm">
                  <option value="">Select sound...</option>
                  <option v-for="s in sounds" :key="s.file" :value="s.file">{{ s.name }}</option>
                </select>
                <button class="btn btn-outline-secondary" :disabled="!newSound" @click="testSound(newSound)" title="Preview">
                  &#9654;
                </button>
              </div>
            </div>
            <div class="col-md-2">
              <button class="btn btn-sm btn-primary w-100" :disabled="!newName.trim() || !newSound" @click="createAction">
                + Create
              </button>
            </div>
          </div>
        </div>
      </div>

      <!-- Actions list -->
      <div v-for="action in actions" :key="action.id" class="card bg-dark border-secondary mb-3">
        <div class="card-body">
          <!-- Action header -->
          <div v-if="editingId === action.id" class="row g-2 align-items-end mb-3">
            <div class="col-md-4">
              <label class="form-label small">Name</label>
              <input v-model="editName" type="text" class="form-control form-control-sm" />
            </div>
            <div class="col-md-4">
              <label class="form-label small">Sound</label>
              <div class="input-group input-group-sm">
                <select v-model="editSound" class="form-select form-select-sm">
                  <option v-for="s in sounds" :key="s.file" :value="s.file">{{ s.name }}</option>
                </select>
                <button class="btn btn-outline-secondary" :disabled="!editSound" @click="testSound(editSound)" title="Preview">
                  &#9654;
                </button>
              </div>
            </div>
            <div class="col-md-4">
              <button class="btn btn-sm btn-success me-1" @click="saveEdit(action)">Save</button>
              <button class="btn btn-sm btn-secondary" @click="cancelEdit">Cancel</button>
            </div>
          </div>
          <div v-else class="d-flex justify-content-between align-items-center mb-3">
            <div>
              <strong>{{ action.name }}</strong>
              <span class="text-muted-custom ms-2 small">{{ soundDisplayName(action.param1) }}</span>
              <button class="btn btn-sm btn-link p-0 ms-2" @click="testSound(action.param1)" title="Preview sound">
                &#9654;
              </button>
            </div>
            <div>
              <button class="btn btn-sm btn-outline-secondary me-1" @click="startEdit(action)">Edit</button>
              <button class="btn btn-sm btn-outline-danger" @click="confirmDelete(action)">Delete</button>
            </div>
          </div>

          <!-- Camera assignments for this action -->
          <div class="ms-2">
            <div class="small text-muted-custom mb-2">Camera Triggers:</div>

            <div v-for="ca in assignmentsForAction(action.id)" :key="ca.id"
                 class="d-flex align-items-center mb-1 ps-2 border-start border-secondary">
              <span class="me-2 small">
                <strong>{{ cameraName(ca.cameraId) }}</strong>
                &mdash;
                <span class="badge bg-info text-dark">{{ ca.detectionClass || 'motion' }}</span>
                <span v-if="ca.detectionClass === '' && ca.mdThreshold > 0" class="text-muted-custom ms-1">
                  (threshold: {{ ca.mdThreshold }})
                </span>
              </span>
              <button class="btn btn-sm btn-outline-danger py-0 px-1" @click="removeAssignment(ca.id)">&times;</button>
            </div>

            <div v-if="assignmentsForAction(action.id).length === 0" class="text-muted-custom small ps-2 mb-2">
              No cameras assigned
            </div>

            <!-- Add assignment inline -->
            <div class="row g-1 align-items-end mt-2 ps-2">
              <div class="col-auto">
                <select v-model.number="assignCameraId" class="form-select form-select-sm" style="width: auto;">
                  <option :value="0" disabled>Camera...</option>
                  <option v-for="cam in cameraStore.cameras" :key="cam.id" :value="cam.id">{{ cam.name }}</option>
                </select>
              </div>
              <div class="col-auto">
                <select v-model="assignClass" class="form-select form-select-sm" style="width: auto;">
                  <option value="" disabled>Trigger...</option>
                  <option v-for="cls in detectionClasses" :key="cls" :value="cls">{{ cls }}</option>
                </select>
              </div>
              <div class="col-auto">
                <button class="btn btn-sm btn-outline-primary"
                        :disabled="!assignCameraId || !assignClass"
                        @click="assignActionId = action.id; addAssignment()">
                  + Add
                </button>
              </div>
            </div>
          </div>
        </div>
      </div>

      <div v-if="actions.length === 0" class="text-muted-custom text-center py-4">
        No actions configured. Create one above to get started.
      </div>
    </div>

    <ConfirmModal
      :show="showConfirm"
      :message="confirmMessage"
      @confirm="onConfirm"
      @cancel="showConfirm = false"
    />
  </div>
</template>
