<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import { useCameraStore } from '../../stores/cameras'
import ConfirmModal from '../common/ConfirmModal.vue'

interface KnownFace {
  id: number
  name: string
  notes: string
  createdAt: number
  updatedAt: number
  verifiedCount: number
  totalCount: number
  bestCropPath: string
  bestCropUID: number
}

interface UnidentifiedFace {
  cropUID: number
  cameraId: number
  timestamp: number
  filePath: string
  detectionConfidence: number
  embeddingUID: number
  matchConfidence: number
}

const cameraStore = useCameraStore()
const knownFaces = ref<KnownFace[]>([])
const unknownFaces = ref<UnidentifiedFace[]>([])
const loading = ref(true)
const unknownOffset = ref(0)
const unknownLimit = 50

// New known face form
const newName = ref('')
const newNotes = ref('')

// Editing
const editingId = ref<number | null>(null)
const editName = ref('')
const editNotes = ref('')

// Assignment mode
const assigningCropUID = ref<number | null>(null)
const assignTargetId = ref(0)
const assignNewName = ref('')

// Merge mode
const mergeSourceId = ref(0)
const mergeTargetId = ref(0)

// Expanded known face
const expandedFaceId = ref<number | null>(null)
const expandedSightings = ref<any[]>([])

// Delete confirmation
const showConfirm = ref(false)
const confirmMessage = ref('')
const confirmAction = ref<(() => void) | null>(null)

// Reprocess
const reprocessing = ref(false)
const reprocessResult = ref<{ processed: number } | null>(null)

// Clip preview
const previewFace = ref<{ cameraId: number; timestamp: number } | null>(null)

function cameraName(id: number) {
  return cameraStore.getCameraById(id)?.name ?? `Camera ${id}`
}

function formatTime(ts: number) {
  if (!ts) return ''
  return new Date(ts * 1000).toLocaleString()
}

async function fetchAll() {
  loading.value = true
  try {
    const [knownData, unknownData] = await Promise.all([
      api<{ faces: KnownFace[] }>('/api/face/known'),
      api<{ faces: UnidentifiedFace[] }>(`/api/face/unknown?limit=${unknownLimit}&offset=${unknownOffset.value}`),
    ])
    knownFaces.value = knownData.faces ?? []
    unknownFaces.value = unknownData.faces ?? []

    if (cameraStore.cameras.length === 0) {
      await cameraStore.fetchCameras()
    }
  } finally {
    loading.value = false
  }
}

async function createKnownFace() {
  if (!newName.value.trim()) return
  await api('/api/face/known/create', {
    method: 'POST',
    body: { name: newName.value.trim(), notes: newNotes.value.trim() },
  })
  newName.value = ''
  newNotes.value = ''
  await fetchAll()
}

function startEdit(face: KnownFace) {
  editingId.value = face.id
  editName.value = face.name
  editNotes.value = face.notes
}

async function saveEdit() {
  if (!editingId.value || !editName.value.trim()) return
  await api('/api/face/known/update', {
    method: 'POST',
    body: { id: editingId.value, name: editName.value.trim(), notes: editNotes.value.trim() },
  })
  editingId.value = null
  await fetchAll()
}

function cancelEdit() {
  editingId.value = null
}

function confirmDelete(face: KnownFace) {
  confirmMessage.value = `Delete "${face.name}"? Their embeddings will become unidentified.`
  confirmAction.value = async () => {
    await api('/api/face/known/delete', { method: 'POST', body: { id: face.id } })
    showConfirm.value = false
    await fetchAll()
  }
  showConfirm.value = true
}

function onConfirm() {
  if (confirmAction.value) confirmAction.value()
}

// Assignment
function startAssign(cropUID: number) {
  assigningCropUID.value = cropUID
  assignTargetId.value = 0
  assignNewName.value = ''
}

async function doAssign() {
  if (!assigningCropUID.value) return

  let targetId = assignTargetId.value

  // Create new known face if name provided
  if (!targetId && assignNewName.value.trim()) {
    const result = await api<{ id: number }>('/api/face/known/create', {
      method: 'POST',
      body: { name: assignNewName.value.trim(), notes: '' },
    })
    targetId = result.id
  }

  if (!targetId) return

  await api('/api/face/assign', {
    method: 'POST',
    body: { cropUID: assigningCropUID.value, knownFaceUID: targetId },
  })

  assigningCropUID.value = null
  await fetchAll()
}

function cancelAssign() {
  assigningCropUID.value = null
}

// Expand/collapse known face sightings
async function toggleExpand(faceId: number) {
  if (expandedFaceId.value === faceId) {
    expandedFaceId.value = null
    expandedSightings.value = []
    return
  }

  expandedFaceId.value = faceId
  const data = await api<{ sightings: any[] }>(`/api/face/sightings/${faceId}?limit=20`)
  expandedSightings.value = data.sightings ?? []
}

async function removeSighting(embeddingUID: number) {
  await api('/api/face/unassign', {
    method: 'POST',
    body: { embeddingUID },
  })
  expandedSightings.value = expandedSightings.value.filter(s => s.embeddingUID !== embeddingUID)
  await fetchAll()
}

// Merge
async function doMerge() {
  if (!mergeSourceId.value || !mergeTargetId.value || mergeSourceId.value === mergeTargetId.value) return
  await api('/api/face/merge', {
    method: 'POST',
    body: { sourceId: mergeSourceId.value, targetId: mergeTargetId.value },
  })
  mergeSourceId.value = 0
  mergeTargetId.value = 0
  await fetchAll()
}

// Load more unknowns
async function loadMoreUnknowns() {
  unknownOffset.value += unknownLimit
  const data = await api<{ faces: UnidentifiedFace[] }>(`/api/face/unknown?limit=${unknownLimit}&offset=${unknownOffset.value}`)
  unknownFaces.value.push(...(data.faces ?? []))
}

// Reprocess
async function reprocess() {
  reprocessing.value = true
  reprocessResult.value = null
  try {
    const result = await api<{ processed: number; remaining: boolean }>('/api/face/reprocess', {
      method: 'POST',
      body: {},
    })
    reprocessResult.value = { processed: result.processed }
    await fetchAll()
  } finally {
    reprocessing.value = false
  }
}

onMounted(fetchAll)
</script>

<template>
  <div v-if="loading" class="text-center py-4 text-muted-custom">Loading...</div>

  <div v-else>
    <!-- Known Faces Section -->
    <div class="mb-4">
      <h5>Known Faces</h5>

      <!-- Create new known face -->
      <div class="row g-2 align-items-end mb-3">
        <div class="col-auto">
          <label class="form-label small">Name</label>
          <input v-model="newName" type="text" class="form-control form-control-sm" placeholder="Person name..." />
        </div>
        <div class="col-auto">
          <label class="form-label small">Notes</label>
          <input v-model="newNotes" type="text" class="form-control form-control-sm" placeholder="Optional notes..." />
        </div>
        <div class="col-auto">
          <button class="btn btn-sm btn-primary" :disabled="!newName.trim()" @click="createKnownFace">
            + Add Person
          </button>
        </div>
      </div>

      <!-- Merge controls -->
      <div v-if="knownFaces.length >= 2" class="row g-2 align-items-end mb-3 border-top pt-2">
        <div class="col-auto">
          <label class="form-label small">Merge</label>
          <select v-model.number="mergeSourceId" class="form-select form-select-sm" style="width: auto;">
            <option :value="0" disabled>From...</option>
            <option v-for="f in knownFaces" :key="f.id" :value="f.id">{{ f.name }}</option>
          </select>
        </div>
        <div class="col-auto">
          <label class="form-label small">Into</label>
          <select v-model.number="mergeTargetId" class="form-select form-select-sm" style="width: auto;">
            <option :value="0" disabled>Into...</option>
            <option v-for="f in knownFaces.filter(x => x.id !== mergeSourceId)" :key="f.id" :value="f.id">{{ f.name }}</option>
          </select>
        </div>
        <div class="col-auto">
          <button class="btn btn-sm btn-outline-warning"
                  :disabled="!mergeSourceId || !mergeTargetId || mergeSourceId === mergeTargetId"
                  @click="doMerge">
            Merge
          </button>
        </div>
      </div>

      <!-- Known faces grid -->
      <div class="row g-2">
        <div v-for="face in knownFaces" :key="face.id" class="col-md-4 col-lg-3">
          <div class="card bg-dark border-secondary">
            <div class="card-body p-2">
              <!-- Editing mode -->
              <div v-if="editingId === face.id">
                <input v-model="editName" class="form-control form-control-sm mb-1" />
                <input v-model="editNotes" class="form-control form-control-sm mb-1" placeholder="Notes..." />
                <button class="btn btn-sm btn-success me-1" @click="saveEdit">Save</button>
                <button class="btn btn-sm btn-secondary" @click="cancelEdit">Cancel</button>
              </div>

              <!-- Display mode -->
              <div v-else>
                <div class="d-flex align-items-start">
                  <img v-if="face.bestCropUID"
                       :src="`/api/face/crop/${face.bestCropUID}`"
                       class="rounded me-2"
                       style="width: 56px; height: 56px; object-fit: cover;"
                       @error="($event.target as HTMLImageElement).style.display = 'none'" />
                  <div class="flex-grow-1">
                    <strong>{{ face.name }}</strong>
                    <div class="small text-muted-custom">
                      {{ face.verifiedCount }} verified / {{ face.totalCount }} total
                    </div>
                    <div v-if="face.notes" class="small text-muted-custom fst-italic">{{ face.notes }}</div>
                  </div>
                </div>
                <div class="mt-1">
                  <button class="btn btn-sm btn-outline-secondary me-1 py-0" @click="startEdit(face)">Edit</button>
                  <button class="btn btn-sm btn-outline-info me-1 py-0" @click="toggleExpand(face.id)">
                    {{ expandedFaceId === face.id ? 'Hide' : 'Sightings' }}
                  </button>
                  <button class="btn btn-sm btn-outline-danger py-0" @click="confirmDelete(face)">Delete</button>
                </div>

                <!-- Expanded sightings -->
                <div v-if="expandedFaceId === face.id" class="mt-2 border-top pt-2">
                  <div v-for="s in expandedSightings" :key="s.cropUID" class="d-flex align-items-center mb-1">
                    <img :src="`/api/face/crop/${s.cropUID}`"
                         class="rounded me-2"
                         style="width: 40px; height: 40px; object-fit: cover; cursor: pointer;"
                         title="View original clip"
                         @click="previewFace = { cameraId: s.cameraId, timestamp: s.timestamp }"
                         @error="($event.target as HTMLImageElement).style.display = 'none'" />
                    <div class="small flex-grow-1">
                      <div>{{ cameraName(s.cameraId) }}</div>
                      <div class="text-muted-custom">{{ formatTime(s.timestamp) }}</div>
                      <div class="text-muted-custom">
                        {{ Math.round(s.matchConfidence * 100) }}% match
                        <span v-if="s.verified" class="badge bg-success ms-1">verified</span>
                      </div>
                    </div>
                    <button class="btn btn-sm btn-outline-danger py-0 ms-2" title="Not this person"
                            @click="removeSighting(s.embeddingUID)">✕</button>
                  </div>
                  <div v-if="expandedSightings.length === 0" class="small text-muted-custom">No sightings yet</div>
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>

      <div v-if="knownFaces.length === 0" class="text-muted-custom small">
        No known faces registered. Add a person above, then assign face crops to them.
      </div>
    </div>

    <!-- Unidentified Faces Section -->
    <div class="mb-4">
      <div class="d-flex justify-content-between align-items-center mb-2">
        <h5 class="mb-0">Unidentified Faces</h5>
        <button class="btn btn-sm btn-outline-primary"
                :disabled="reprocessing"
                @click="reprocess">
          {{ reprocessing ? 'Processing...' : 'Reprocess Crops' }}
        </button>
      </div>

      <div v-if="reprocessResult" class="alert alert-info py-1 small">
        Processed {{ reprocessResult.processed }} crops. Faces will appear in the unidentified gallery below.
      </div>

      <div class="row g-2">
        <div v-for="face in unknownFaces" :key="face.cropUID" class="col-6 col-md-3 col-lg-2">
          <div class="card bg-dark border-secondary">
            <img :src="`/api/face/crop/${face.cropUID}`"
                 class="card-img-top"
                 style="aspect-ratio: 1; object-fit: cover; cursor: pointer;"
                 title="View original clip"
                 @click="previewFace = { cameraId: face.cameraId, timestamp: face.timestamp }"
                 @error="($event.target as HTMLImageElement).style.display = 'none'" />
            <div class="card-body p-1">
              <div class="small text-muted-custom">{{ cameraName(face.cameraId) }}</div>
              <div class="small text-muted-custom">{{ formatTime(face.timestamp) }}</div>
              <div class="small text-muted-custom">{{ Math.round(face.detectionConfidence * 100) }}%</div>

              <!-- Assign controls -->
              <div v-if="assigningCropUID === face.cropUID" class="mt-1">
                <select v-model.number="assignTargetId" class="form-select form-select-sm mb-1">
                  <option :value="0">— New person —</option>
                  <option v-for="kf in knownFaces" :key="kf.id" :value="kf.id">{{ kf.name }}</option>
                </select>
                <input v-if="!assignTargetId" v-model="assignNewName" type="text"
                       class="form-control form-control-sm mb-1" placeholder="Name..." />
                <button class="btn btn-sm btn-success py-0 me-1"
                        :disabled="!assignTargetId && !assignNewName.trim()"
                        @click="doAssign">
                  Assign
                </button>
                <button class="btn btn-sm btn-secondary py-0" @click="cancelAssign">Cancel</button>
              </div>
              <button v-else class="btn btn-sm btn-outline-info py-0 mt-1 w-100" @click="startAssign(face.cropUID)">
                Identify
              </button>
            </div>
          </div>
        </div>
      </div>

      <div v-if="unknownFaces.length === 0" class="text-muted-custom small">
        No unidentified faces.
      </div>

      <div v-if="unknownFaces.length >= unknownLimit" class="text-center mt-2">
        <button class="btn btn-sm btn-outline-secondary" @click="loadMoreUnknowns">Load More</button>
      </div>
    </div>

    <ConfirmModal
      :show="showConfirm"
      :message="confirmMessage"
      @confirm="onConfirm"
      @cancel="showConfirm = false"
    />

    <!-- Clip preview modal -->
    <Teleport to="body">
      <div v-if="previewFace" class="modal d-block" style="background: rgba(0,0,0,0.7);" @click.self="previewFace = null">
        <div class="modal-dialog modal-lg modal-dialog-centered">
          <div class="modal-content bg-dark text-light border-secondary">
            <div class="modal-header border-secondary py-2">
              <h6 class="modal-title">
                {{ cameraName(previewFace.cameraId) }} — {{ formatTime(previewFace.timestamp) }}
              </h6>
              <button type="button" class="btn-close btn-close-white" @click="previewFace = null"></button>
            </div>
            <div class="modal-body p-0">
              <video
                :src="`/clip/video/${previewFace.cameraId}/${previewFace.timestamp}`"
                controls autoplay
                class="w-100"
                style="max-height: 70vh;"
              />
            </div>
            <div class="modal-footer border-secondary py-1">
              <router-link
                :to="`/clips/${previewFace.cameraId}`"
                class="btn btn-sm btn-outline-info"
                @click="previewFace = null"
              >
                View all clips for this camera
              </router-link>
              <button class="btn btn-sm btn-secondary" @click="previewFace = null">Close</button>
            </div>
          </div>
        </div>
      </div>
    </Teleport>
  </div>
</template>
