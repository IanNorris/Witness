<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'

interface Group {
  id: number
  displayName: string
  description: string
}

const groups = ref<Group[]>([])
const loading = ref(true)

async function fetchGroups() {
  loading.value = true
  try {
    const data = await api<{ groups: Group[] }>('/group/enum')
    groups.value = data.groups ?? []
  } finally {
    loading.value = false
  }
}

async function addGroup() {
  const name = prompt('Group name:')
  if (!name) return
  await api('/group/create', { method: 'POST', body: { displayName: name } })
  await fetchGroups()
}

async function renameGroup(group: Group) {
  const name = prompt('New name:', group.displayName)
  if (name === null) return
  await api('/group/update', { method: 'POST', body: { id: group.id, displayName: name } })
  group.displayName = name
}

async function deleteGroup(group: Group) {
  if (!confirm(`Delete group "${group.displayName}"?`)) return
  await api('/group/delete', { method: 'POST', body: { id: group.id } })
  groups.value = groups.value.filter(g => g.id !== group.id)
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
  </div>
</template>
