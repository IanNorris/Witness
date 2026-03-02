<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import InputModal from '../common/InputModal.vue'
import ConfirmModal from '../common/ConfirmModal.vue'

interface Group {
  id: number
  displayName: string
  description: string
}

const groups = ref<Group[]>([])
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

async function fetchGroups() {
  loading.value = true
  try {
    const data = await api<{ groups: Group[] }>('/group/enum')
    groups.value = data.groups ?? []
  } finally {
    loading.value = false
  }
}

function addGroup() {
  inputTitle.value = 'Add Group'
  inputLabel.value = 'Group name'
  inputValue.value = ''
  inputAction.value = async (name) => {
    if (!name) return
    await api('/group/create', { method: 'POST', body: { displayName: name } })
    await fetchGroups()
  }
  showInput.value = true
}

function renameGroup(group: Group) {
  inputTitle.value = 'Rename Group'
  inputLabel.value = 'New name'
  inputValue.value = group.displayName
  inputAction.value = async (name) => {
    await api('/group/update', { method: 'POST', body: { id: group.id, displayName: name } })
    group.displayName = name
  }
  showInput.value = true
}

function deleteGroup(group: Group) {
  confirmMessage.value = `Delete group "${group.displayName}"?`
  confirmAction.value = async () => {
    await api('/group/delete', { method: 'POST', body: { id: group.id } })
    groups.value = groups.value.filter(g => g.id !== group.id)
  }
  showConfirm.value = true
}

function onInputSubmit(val: string) {
  showInput.value = false
  inputAction.value?.(val)
}

function onConfirm() {
  showConfirm.value = false
  confirmAction.value?.()
}

onMounted(fetchGroups)
</script>

<template>
  <div>
    <div class="d-flex justify-content-between align-items-center mb-3">
      <h6 class="mb-0">Groups</h6>
      <button class="btn btn-sm btn-primary" @click="addGroup">+ Add Group</button>
    </div>

    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <table v-else class="table table-dark table-sm">
      <thead>
        <tr>
          <th>Name</th>
          <th>Description</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="group in groups" :key="group.id">
          <td>{{ group.displayName }}</td>
          <td>{{ group.description }}</td>
          <td>
            <button class="btn btn-sm btn-outline-secondary me-1" @click="renameGroup(group)">Rename</button>
            <button class="btn btn-sm btn-outline-danger" @click="deleteGroup(group)">Delete</button>
          </td>
        </tr>
        <tr v-if="groups.length === 0">
          <td colspan="3" class="text-muted-custom">No groups</td>
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
  </div>
</template>
