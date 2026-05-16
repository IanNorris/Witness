<script setup lang="ts">
import { ref, watch, nextTick, computed } from 'vue'

export interface CameraFormData {
  id?: number
  displayName: string
  connectionString: string
  connectionStringSub: string
  description: string
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
  ptzEnabled: number
  ptzApiHost: string
  ptzApiPort: number
  ptzUsername: string
  ptzPassword: string
  linkedCameraId: number
  motionSourceCameraId: number
}

const props = defineProps<{
  show: boolean
  camera?: CameraFormData | null
  availableGroups?: { id: number; displayName: string }[]
}>()

const emit = defineEmits<{
  save: [data: CameraFormData]
  cancel: []
}>()

const form = ref<CameraFormData>(defaultForm())
const nameRef = ref<HTMLInputElement | null>(null)

const isEditing = computed(() => !!props.camera?.id)
const title = computed(() => isEditing.value ? `Edit Camera — ${props.camera?.displayName}` : 'Add Camera')

function defaultForm(): CameraFormData {
  return {
    displayName: '',
    connectionString: '',
    connectionStringSub: '',
    description: '',
    enabled: 1,
    skipFrames: 1,
    mdFrameHeight: 400,
    mdThreshold: 0.0001,
    motionFilter: '',
    blackoutMaskPath: '',
    focusMaskPath: '',
    continuousRecording: 0,
    lowLatencyHLS: 0,
    groups: [],
    ptzEnabled: 0,
    ptzApiHost: '',
    ptzApiPort: 443,
    ptzUsername: '',
    ptzPassword: '',
    linkedCameraId: 0,
    motionSourceCameraId: 0,
  }
}

watch(() => props.show, async (v) => {
  if (v) {
    form.value = props.camera ? { ...props.camera } : defaultForm()
    await nextTick()
    nameRef.value?.focus()
  }
})

function onSubmit() {
  emit('save', { ...form.value })
}

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') emit('cancel')
}

function toggleGroup(groupId: number) {
  const idx = form.value.groups.indexOf(groupId)
  if (idx >= 0) form.value.groups.splice(idx, 1)
  else form.value.groups.push(groupId)
}
</script>

<template>
  <Teleport to="body">
    <div v-if="show" class="modal-backdrop show" @click="emit('cancel')" />
    <div v-if="show" class="modal d-block" tabindex="-1" @keydown="onKeydown">
      <div class="modal-dialog modal-dialog-centered modal-lg modal-dialog-scrollable" @click.stop>
        <div class="modal-content bg-dark text-light">
          <div class="modal-header border-secondary py-2">
            <h6 class="modal-title">{{ title }}</h6>
            <button type="button" class="btn-close btn-close-white" @click="emit('cancel')" />
          </div>
          <form @submit.prevent="onSubmit">
            <div class="modal-body py-3">
              <!-- Basic Info -->
              <h6 class="text-muted mb-2 small text-uppercase">General</h6>
              <div class="row g-2 mb-3">
                <div class="col-md-6">
                  <label class="form-label small">Name</label>
                  <input ref="nameRef" v-model="form.displayName" class="form-control form-control-sm" required placeholder="Front Door" />
                </div>
                <div class="col-md-6">
                  <label class="form-label small">Description</label>
                  <input v-model="form.description" class="form-control form-control-sm" placeholder="Optional description" />
                </div>
                <div class="col-12">
                  <label class="form-label small">RTSP URL (Main stream)</label>
                  <input v-model="form.connectionString" class="form-control form-control-sm font-monospace" required placeholder="rtsp://user:pass@192.168.1.x/stream1" />
                </div>
                <div class="col-12">
                  <label class="form-label small">RTSP URL (Sub-stream, optional)</label>
                  <input v-model="form.connectionStringSub" class="form-control form-control-sm font-monospace" placeholder="rtsp://user:pass@192.168.1.x/stream2" />
                </div>
                <div class="col-auto">
                  <div class="form-check mt-2">
                    <input class="form-check-input" type="checkbox" id="cam-enabled" :checked="!!form.enabled" @change="form.enabled = form.enabled ? 0 : 1" />
                    <label class="form-check-label small" for="cam-enabled">Enabled</label>
                  </div>
                </div>
                <div class="col-auto">
                  <div class="form-check mt-2">
                    <input class="form-check-input" type="checkbox" id="cam-continuous" :checked="!!form.continuousRecording" @change="form.continuousRecording = form.continuousRecording ? 0 : 1" />
                    <label class="form-check-label small" for="cam-continuous">Continuous Recording</label>
                  </div>
                </div>
                <div class="col-auto">
                  <div class="form-check mt-2">
                    <input class="form-check-input" type="checkbox" id="cam-llhls" :checked="!!form.lowLatencyHLS" @change="form.lowLatencyHLS = form.lowLatencyHLS ? 0 : 1" />
                    <label class="form-check-label small" for="cam-llhls">Low-Latency HLS</label>
                  </div>
                </div>
              </div>

              <!-- Motion Detection -->
              <h6 class="text-muted mb-2 small text-uppercase">Motion Detection</h6>
              <div class="row g-2 mb-3">
                <div class="col-md-4">
                  <label class="form-label small">Threshold</label>
                  <input v-model.number="form.mdThreshold" type="number" step="0.00001" min="0" class="form-control form-control-sm" />
                  <div class="form-text">Lower = more sensitive</div>
                </div>
                <div class="col-md-4">
                  <label class="form-label small">Frame Height (px)</label>
                  <input v-model.number="form.mdFrameHeight" type="number" min="100" max="1080" class="form-control form-control-sm" />
                  <div class="form-text">Analysis resolution</div>
                </div>
                <div class="col-md-4">
                  <label class="form-label small">Skip Frames</label>
                  <input v-model.number="form.skipFrames" type="number" min="1" max="30" class="form-control form-control-sm" />
                  <div class="form-text">Process every Nth frame</div>
                </div>
                <div class="col-md-6">
                  <label class="form-label small">Motion Filter</label>
                  <input v-model="form.motionFilter" class="form-control form-control-sm" placeholder="Default (from server config)" />
                </div>
              </div>

              <!-- Masks -->
              <h6 class="text-muted mb-2 small text-uppercase">Detection Masks</h6>
              <div class="row g-2 mb-3">
                <div class="col-md-6">
                  <label class="form-label small">Blackout Mask Path</label>
                  <input v-model="form.blackoutMaskPath" class="form-control form-control-sm font-monospace" placeholder="Path to grayscale mask image" />
                  <div class="form-text">Excludes regions (black = ignore)</div>
                </div>
                <div class="col-md-6">
                  <label class="form-label small">Focus Mask Path</label>
                  <input v-model="form.focusMaskPath" class="form-control form-control-sm font-monospace" placeholder="Path to grayscale mask image" />
                  <div class="form-text">Amplifies regions (white = focus)</div>
                </div>
              </div>

              <!-- PTZ Configuration -->
              <h6 class="text-muted mb-2 small text-uppercase">PTZ Control</h6>
              <div class="row g-2 mb-3">
                <div class="col-auto">
                  <div class="form-check mt-2">
                    <input class="form-check-input" type="checkbox" id="cam-ptz" :checked="!!form.ptzEnabled" @change="form.ptzEnabled = form.ptzEnabled ? 0 : 1" />
                    <label class="form-check-label small" for="cam-ptz">PTZ Enabled</label>
                  </div>
                </div>
                <template v-if="form.ptzEnabled">
                  <div class="col-md-6">
                    <label class="form-label small">Camera API Host</label>
                    <input v-model="form.ptzApiHost" class="form-control form-control-sm font-monospace" placeholder="192.168.1.x" />
                  </div>
                  <div class="col-md-3">
                    <label class="form-label small">API Port</label>
                    <input v-model.number="form.ptzApiPort" type="number" min="1" max="65535" class="form-control form-control-sm" />
                  </div>
                  <div class="col-md-4">
                    <label class="form-label small">Username</label>
                    <input v-model="form.ptzUsername" class="form-control form-control-sm" placeholder="admin" />
                  </div>
                  <div class="col-md-4">
                    <label class="form-label small">Password</label>
                    <input v-model="form.ptzPassword" type="password" class="form-control form-control-sm" placeholder="••••••" />
                  </div>
                  <div class="col-md-4">
                    <label class="form-label small">Linked Camera ID</label>
                    <input v-model.number="form.linkedCameraId" type="number" min="0" class="form-control form-control-sm" />
                    <div class="form-text">0 = none (dual-lens link)</div>
                  </div>
                </template>
              </div>

              <!-- Motion Source -->
              <div class="row g-2 mb-3">
                <div class="col-md-6">
                  <label class="form-label small">Motion Source Camera ID</label>
                  <input v-model.number="form.motionSourceCameraId" type="number" min="0" class="form-control form-control-sm" />
                  <div class="form-text">0 = detect own motion. Set to another camera's ID to pair (this camera records when the source detects motion, uses passthrough decode for minimal CPU)</div>
                </div>
              </div>

              <!-- Groups -->
              <div v-if="availableGroups && availableGroups.length > 0">
                <h6 class="text-muted mb-2 small text-uppercase">Groups</h6>
                <div class="d-flex flex-wrap gap-2">
                  <div v-for="group in availableGroups" :key="group.id" class="form-check">
                    <input
                      class="form-check-input"
                      type="checkbox"
                      :id="`grp-${group.id}`"
                      :checked="form.groups.includes(group.id)"
                      @change="toggleGroup(group.id)"
                    />
                    <label class="form-check-label small" :for="`grp-${group.id}`">{{ group.displayName }}</label>
                  </div>
                </div>
              </div>
            </div>
            <div class="modal-footer border-secondary py-2">
              <button type="button" class="btn btn-sm btn-secondary" @click="emit('cancel')">Cancel</button>
              <button type="submit" class="btn btn-sm btn-primary">
                {{ isEditing ? 'Save Changes' : 'Create Camera' }}
              </button>
            </div>
          </form>
        </div>
      </div>
    </div>
  </Teleport>
</template>
