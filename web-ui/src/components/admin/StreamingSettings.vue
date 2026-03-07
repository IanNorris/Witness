<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'

const loading = ref(true)

const streamingSettings = ref<Record<string, string>>({
  mse_partial_duration: '0.15',
})

const settingKeys = ['mse_partial_duration']

async function fetchSettings() {
  loading.value = true
  try {
    const data = await api<Record<string, string>>('/api/setup/settings')
    if (data) {
      for (const key of settingKeys) {
        streamingSettings.value[key] = data[key] ?? streamingSettings.value[key] ?? ''
      }
    }
  } finally {
    loading.value = false
  }
}

async function saveSetting(name: string) {
  await api('/api/settings/set', {
    method: 'POST',
    body: { name, value: streamingSettings.value[name] ?? '' },
  })
}

onMounted(fetchSettings)
</script>

<template>
  <div>
    <h6 class="mb-3">Streaming Settings</h6>
    <p class="text-muted small">Changes take effect on server restart.</p>

    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <div v-else class="row g-3">
      <div class="col-md-4">
        <label class="form-label small">MSE Partial Duration (seconds)</label>
        <input
          v-model="streamingSettings['mse_partial_duration']"
          type="number"
          step="0.05"
          min="0.05"
          max="1.0"
          class="form-control form-control-sm"
          @blur="saveSetting('mse_partial_duration')"
        />
        <div class="form-text small">
          Size of each video chunk sent via WebSocket. Lower values reduce latency but increase overhead.
          Default: 0.15s. Range: 0.05–1.0s.
        </div>
      </div>
    </div>
  </div>
</template>
