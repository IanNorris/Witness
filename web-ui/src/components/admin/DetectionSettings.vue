<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import { useSettingsStore } from '../../stores/settings'

const loading = ref(true)
const settings = useSettingsStore()

const detectionKeys = [
  'detection_backend',
  'detection_provider',
  'detection_model_path',
  'detection_confidence',
  'detection_max_fps',
  'cudnn_path',
  'face_detection_enabled',
  'face_detection_confidence',
  'face_recognition_enabled',
  'face_recognition_model_path',
  'face_recognition_confidence',
]

const detectionSettings = ref<Record<string, string>>({})
const cudaTestResult = ref<string | null>(null)
const cudaTesting = ref(false)

async function fetchSettings() {
  loading.value = true
  try {
    const data = await api<{ settings?: Record<string, string> }>('/api/setup/settings')
    if (data) {
      for (const key of detectionKeys) {
        detectionSettings.value[key] = (data as Record<string, string>)[key] ?? ''
      }
    }
  } finally {
    loading.value = false
  }
}

async function saveSetting(name: string) {
  await api('/api/settings/set', {
    method: 'POST',
    body: { name, value: detectionSettings.value[name] ?? '' },
  })
}

async function testCuda() {
  cudaTesting.value = true
  cudaTestResult.value = null
  try {
    const data = await api<{ success: boolean; message: string }>('/api/setup/test-cuda', {
      method: 'POST',
      body: { cudnn_path: detectionSettings.value['cudnn_path'] },
    })
    cudaTestResult.value = data.success ? `✓ ${data.message}` : `✗ ${data.message}`
  } catch (e) {
    cudaTestResult.value = `✗ ${e}`
  } finally {
    cudaTesting.value = false
  }
}

onMounted(fetchSettings)
</script>

<template>
  <div>
    <h6 class="mb-3">Detection Settings</h6>

    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <div v-else class="row g-3">
      <div class="col-md-6">
        <label class="form-label small">Backend</label>
        <select v-model="detectionSettings['detection_backend']" class="form-select form-select-sm" @change="saveSetting('detection_backend')">
          <option value="">Disabled</option>
          <option value="onnx">ONNX (Local)</option>
        </select>
      </div>

      <div class="col-md-6">
        <label class="form-label small">Provider</label>
        <select v-model="detectionSettings['detection_provider']" class="form-select form-select-sm" @change="saveSetting('detection_provider')">
          <option value="cpu">CPU</option>
          <option value="cuda">CUDA (NVIDIA GPU)</option>
        </select>
      </div>

      <div class="col-md-6">
        <label class="form-label small">Model Path</label>
        <input v-model="detectionSettings['detection_model_path']" class="form-control form-control-sm" @blur="saveSetting('detection_model_path')" />
      </div>

      <div class="col-md-3">
        <label class="form-label small">Confidence</label>
        <input v-model="detectionSettings['detection_confidence']" type="number" step="0.05" min="0" max="1" class="form-control form-control-sm" @blur="saveSetting('detection_confidence')" />
      </div>

      <div class="col-md-3">
        <label class="form-label small">Max FPS</label>
        <input v-model="detectionSettings['detection_max_fps']" type="number" step="0.5" min="0.5" class="form-control form-control-sm" @blur="saveSetting('detection_max_fps')" />
      </div>

      <div class="col-md-8">
        <label class="form-label small">cuDNN Path (for CUDA)</label>
        <input v-model="detectionSettings['cudnn_path']" class="form-control form-control-sm" @blur="saveSetting('cudnn_path')" />
      </div>

      <div class="col-md-4 d-flex align-items-end">
        <button class="btn btn-sm btn-outline-secondary" :disabled="cudaTesting" @click="testCuda">
          {{ cudaTesting ? 'Testing...' : 'Test CUDA' }}
        </button>
      </div>

      <div v-if="cudaTestResult" class="col-12">
        <div class="alert alert-sm py-1 px-2 small" :class="cudaTestResult.startsWith('✓') ? 'alert-success' : 'alert-danger'">
          {{ cudaTestResult }}
        </div>
      </div>

      <div class="col-12 mt-3">
        <h6 class="mb-2">Face Detection</h6>
      </div>

      <div class="col-md-6">
        <label class="form-label small">Face Detection</label>
        <select v-model="detectionSettings['face_detection_enabled']" class="form-select form-select-sm" @change="saveSetting('face_detection_enabled')">
          <option value="">Disabled</option>
          <option value="1">Enabled</option>
        </select>
        <div class="form-text small">Detect faces within person bounding boxes using YuNet.</div>
      </div>

      <div class="col-md-6">
        <label class="form-label small">Face Confidence</label>
        <input v-model="detectionSettings['face_detection_confidence']" type="number" step="0.05" min="0" max="1" class="form-control form-control-sm" @blur="saveSetting('face_detection_confidence')" />
        <div class="form-text small">Minimum confidence for face detection (default: 0.5).</div>
      </div>

      <div class="col-12 mt-3">
        <h6 class="mb-2">Face Recognition</h6>
      </div>

      <div class="col-md-4">
        <label class="form-label small">Face Recognition</label>
        <select v-model="detectionSettings['face_recognition_enabled']" class="form-select form-select-sm" @change="saveSetting('face_recognition_enabled')">
          <option value="">Disabled</option>
          <option value="1">Enabled</option>
        </select>
        <div class="form-text small">Match detected faces against known identities.</div>
      </div>

      <div class="col-md-4">
        <label class="form-label small">Recognition Model Path</label>
        <input v-model="detectionSettings['face_recognition_model_path']" type="text" class="form-control form-control-sm" placeholder="models/face_recognition.onnx" @blur="saveSetting('face_recognition_model_path')" />
        <div class="form-text small">ONNX model for face embedding (ArcFace, MobileFaceNet, etc.).</div>
      </div>

      <div class="col-md-4">
        <label class="form-label small">Match Threshold</label>
        <input v-model="detectionSettings['face_recognition_confidence']" type="number" step="0.05" min="0.1" max="0.9" class="form-control form-control-sm" @blur="saveSetting('face_recognition_confidence')" />
        <div class="form-text small">Cosine similarity threshold for identity match (default: 0.5).</div>
      </div>

      <div class="col-12 mt-3">
        <h6 class="mb-2">Overlay Display</h6>
      </div>

      <div class="col-md-6">
        <label class="form-label small">Min Confidence to Display ({{ settings.detectionMinConfidence }}%)</label>
        <input v-model.number="settings.detectionMinConfidence" type="range" min="0" max="100" step="5" class="form-range" />
        <div class="form-text small">Detection boxes below this confidence will be hidden in the overlay.</div>
      </div>
    </div>
  </div>
</template>
