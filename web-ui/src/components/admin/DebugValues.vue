<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import InputModal from '../common/InputModal.vue'
import ConfirmModal from '../common/ConfirmModal.vue'
import AlertModal from '../common/AlertModal.vue'

interface DebugValue {
  name: string
  value: string
}

const values = ref<DebugValue[]>([])
const loading = ref(true)

// Modal state
const showInput = ref(false)
const inputTitle = ref('')
const inputLabel = ref('')
const inputValue = ref('')
const inputAction = ref<((val: string) => void) | null>(null)

const showConfirm = ref(false)
const confirmMessage = ref('')
const confirmAction = ref<(() => void) | null>(null)

const showAlert = ref(false)
const alertMessage = ref('')

async function fetchValues() {
  loading.value = true
  try {
    const data = await api<{ values: DebugValue[] }>('/debug/enum')
    values.value = data.values ?? []
  } finally {
    loading.value = false
  }
}

function setValue(item: DebugValue) {
  inputTitle.value = 'Set Debug Value'
  inputLabel.value = item.name
  inputValue.value = item.value
  inputAction.value = async (val) => {
    await api('/debug/set', { method: 'POST', body: { name: item.name, value: val } })
    item.value = val
  }
  showInput.value = true
}

async function resetValue(item: DebugValue) {
  await api('/debug/reset', { method: 'POST', body: { name: item.name } })
  await fetchValues()
}

function resetAll() {
  confirmMessage.value = 'Reset all debug values to defaults?'
  confirmAction.value = async () => {
    for (const item of values.value) {
      await api('/debug/reset', { method: 'POST', body: { name: item.name } })
    }
    await fetchValues()
  }
  showConfirm.value = true
}

async function reloadTls() {
  const data = await api<{ success: boolean; message: string }>('/debug/reload_tls', {
    method: 'POST',
    body: {},
  })
  alertMessage.value = data.message
  showAlert.value = true
}

function onInputSubmit(val: string) {
  showInput.value = false
  inputAction.value?.(val)
}

function onConfirm() {
  showConfirm.value = false
  confirmAction.value?.()
}

onMounted(fetchValues)
</script>

<template>
  <div>
    <div class="d-flex justify-content-between align-items-center mb-3">
      <h6 class="mb-0">Debug Values</h6>
      <div>
        <button class="btn btn-sm btn-outline-secondary me-1" @click="reloadTls">Reload TLS</button>
        <button class="btn btn-sm btn-outline-danger" @click="resetAll">Reset All</button>
      </div>
    </div>

    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <table v-else class="table table-dark table-sm">
      <thead>
        <tr>
          <th>Name</th>
          <th>Value</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="item in values" :key="item.name">
          <td class="small">{{ item.name }}</td>
          <td class="small text-break" style="max-width: 400px;">{{ item.value }}</td>
          <td>
            <button class="btn btn-sm btn-outline-secondary me-1" @click="setValue(item)">Edit</button>
            <button class="btn btn-sm btn-outline-warning" @click="resetValue(item)">Reset</button>
          </td>
        </tr>
        <tr v-if="values.length === 0">
          <td colspan="3" class="text-muted-custom">No debug values</td>
        </tr>
      </tbody>
    </table>

    <InputModal
      :show="showInput"
      :title="inputTitle"
      :label="inputLabel"
      :model-value="inputValue"
      @submit="onInputSubmit"
      @cancel="showInput = false"
    />
    <ConfirmModal
      :show="showConfirm"
      :message="confirmMessage"
      @confirm="onConfirm"
      @cancel="showConfirm = false"
    />
    <AlertModal
      :show="showAlert"
      title="TLS Reload"
      :message="alertMessage"
      @close="showAlert = false"
    />
  </div>
</template>
