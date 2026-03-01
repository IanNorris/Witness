<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import { useAuthStore } from '../../stores/auth'
import InputModal from '../common/InputModal.vue'

interface User {
  userid: number
  username: string
  displayName: string
  enabled: number
  admin: number
  groups: number[]
}

const authStore = useAuthStore()
const users = ref<User[]>([])
const loading = ref(true)
const newUserResult = ref<{ username: string; password: string } | null>(null)

// Modal state
const showInput = ref(false)
const inputTitle = ref('')
const inputLabel = ref('')
const inputValue = ref('')
const inputAction = ref<((val: string) => void) | null>(null)

function isSelf(user: User) {
  return user.username === authStore.username
}

async function fetchUsers() {
  loading.value = true
  try {
    const data = await api<User[]>('/auth/admin_enum')
    users.value = data
  } finally {
    loading.value = false
  }
}

function addUser() {
  inputTitle.value = 'Add User'
  inputLabel.value = 'Username'
  inputValue.value = ''
  inputAction.value = async (username) => {
    if (!username) return
    const data = await api<{ username: string; password: string }>('/auth/new_user', {
      method: 'POST',
      body: { username },
    })
    newUserResult.value = data
    await fetchUsers()
  }
  showInput.value = true
}

async function toggleEnabled(user: User) {
  if (isSelf(user)) return
  await api('/auth/toggle_enabled', {
    method: 'POST',
    body: { username: user.username, value: !user.enabled },
  })
  user.enabled = user.enabled ? 0 : 1
}

async function toggleAdmin(user: User) {
  if (isSelf(user)) return
  await api('/auth/toggle_admin', {
    method: 'POST',
    body: { username: user.username, value: !user.admin },
  })
  user.admin = user.admin ? 0 : 1
}

function setDisplayName(user: User) {
  inputTitle.value = 'Set Display Name'
  inputLabel.value = 'Display name'
  inputValue.value = user.displayName
  inputAction.value = async (name) => {
    await api('/auth/set_display_name', {
      method: 'POST',
      body: { username: user.username, value: name },
    })
    user.displayName = name
  }
  showInput.value = true
}

function onInputSubmit(val: string) {
  showInput.value = false
  inputAction.value?.(val)
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

    <table v-else class="table table-dark table-sm align-middle">
      <thead>
        <tr>
          <th>Username</th>
          <th>Display Name</th>
          <th class="text-center">Admin</th>
          <th class="text-center">Enabled</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="user in users" :key="user.userid" :class="{ 'opacity-50': !user.enabled && !isSelf(user) }">
          <td>{{ user.username }}</td>
          <td>{{ user.displayName }}</td>
          <td class="text-center">
            <div class="form-check d-inline-block mb-0">
              <input
                class="form-check-input"
                type="checkbox"
                :checked="!!user.admin"
                :disabled="isSelf(user)"
                :title="isSelf(user) ? 'Cannot change your own admin status' : ''"
                @change="toggleAdmin(user)"
              />
            </div>
          </td>
          <td class="text-center">
            <div class="form-check d-inline-block mb-0">
              <input
                class="form-check-input"
                type="checkbox"
                :checked="!!user.enabled"
                :disabled="isSelf(user)"
                :title="isSelf(user) ? 'Cannot disable your own account' : ''"
                @change="toggleEnabled(user)"
              />
            </div>
          </td>
          <td>
            <button class="btn btn-sm btn-outline-secondary" @click="setDisplayName(user)">Rename</button>
          </td>
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
  </div>
</template>
