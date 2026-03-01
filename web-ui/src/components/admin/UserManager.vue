<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'

interface User {
  userid: number
  username: string
  displayName: string
  enabled: number
  admin: number
  groups: number[]
}

const users = ref<User[]>([])
const loading = ref(true)
const newUserResult = ref<{ username: string; password: string } | null>(null)

async function fetchUsers() {
  loading.value = true
  try {
    const data = await api<User[]>('/auth/admin_enum')
    users.value = data
  } finally {
    loading.value = false
  }
}

async function addUser() {
  const username = prompt('Enter username:')
  if (!username) return
  const data = await api<{ username: string; password: string }>('/auth/new_user', {
    method: 'POST',
    body: { username },
  })
  newUserResult.value = data
  await fetchUsers()
}

async function toggleEnabled(user: User) {
  await api('/auth/toggle_enabled', {
    method: 'POST',
    body: { username: user.username, value: !user.enabled },
  })
  user.enabled = user.enabled ? 0 : 1
}

async function toggleAdmin(user: User) {
  await api('/auth/toggle_admin', {
    method: 'POST',
    body: { username: user.username, value: !user.admin },
  })
  user.admin = user.admin ? 0 : 1
}

async function setDisplayName(user: User) {
  const name = prompt('Display name:', user.displayName)
  if (name === null) return
  await api('/auth/set_display_name', {
    method: 'POST',
    body: { username: user.username, value: name },
  })
  user.displayName = name
}

onMounted(fetchUsers)
</script>

<template>
  <div>
    <div class="d-flex justify-content-between align-items-center mb-3">
      <h6 class="mb-0">Users</h6>
      <button class="btn btn-sm btn-primary" @click="addUser">+ Add User</button>
    </div>

    <!-- New user password display -->
    <div v-if="newUserResult" class="alert alert-info alert-dismissible">
      <strong>New user created:</strong> {{ newUserResult.username }}<br>
      <strong>Temporary password:</strong> <code>{{ newUserResult.password }}</code>
      <button class="btn-close" @click="newUserResult = null" />
    </div>

    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <table v-else class="table table-dark table-sm">
      <thead>
        <tr>
          <th>Username</th>
          <th>Display Name</th>
          <th>Admin</th>
          <th>Enabled</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="user in users" :key="user.userid">
          <td>{{ user.username }}</td>
          <td>{{ user.displayName }}</td>
          <td>
            <span class="badge" :class="user.admin ? 'bg-primary' : 'bg-secondary'">
              {{ user.admin ? 'Yes' : 'No' }}
            </span>
          </td>
          <td>
            <span class="badge" :class="user.enabled ? 'bg-success' : 'bg-danger'">
              {{ user.enabled ? 'Yes' : 'No' }}
            </span>
          </td>
          <td>
            <button class="btn btn-sm btn-outline-secondary me-1" @click="setDisplayName(user)">Rename</button>
            <button class="btn btn-sm btn-outline-secondary me-1" @click="toggleAdmin(user)">
              {{ user.admin ? 'Revoke Admin' : 'Make Admin' }}
            </button>
            <button class="btn btn-sm" :class="user.enabled ? 'btn-outline-warning' : 'btn-outline-success'" @click="toggleEnabled(user)">
              {{ user.enabled ? 'Disable' : 'Enable' }}
            </button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>
