<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'

const loading = ref(true)

const detectionKeys = [
  'detection_backend',
  'detection_provider',
  'detection_model_path',
  'detection_confidence',
  'detection_max_fps',
  'cudnn_path',
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
    </div>
  </div>
</template>
