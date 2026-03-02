<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { api } from '../../composables/useApi'
import { useAuthStore } from '../../stores/auth'
import InputModal from '../common/InputModal.vue'
import ConfirmModal from '../common/ConfirmModal.vue'
import AlertModal from '../common/AlertModal.vue'

interface Group {
  id: number
  displayName: string
}

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
const groups = ref<Group[]>([])
const loading = ref(true)
const newUserResult = ref<{ username: string; password: string } | null>(null)

// Modal state
const showInput = ref(false)
const inputTitle = ref('')
const inputLabel = ref('')
const inputValue = ref('')
const inputType = ref('text')
const inputConfirm = ref(false)
const inputAction = ref<((val: string) => void) | null>(null)

const showConfirm = ref(false)
const confirmMessage = ref('')
const confirmAction = ref<(() => void) | null>(null)

const showAlert = ref(false)
const alertTitle = ref('')
const alertMessage = ref('')

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

async function fetchGroups() {
  try {
    const data = await api<{ groups: Group[] }>('/group/enum')
    groups.value = data.groups ?? []
  } catch { /* optional */ }
}

async function toggleUserGroup(user: User, groupId: number) {
  const idx = user.groups.indexOf(groupId)
  const newGroups = [...user.groups]
  if (idx >= 0) newGroups.splice(idx, 1)
  else newGroups.push(groupId)
  await api('/auth/set_user_groups', {
    method: 'POST',
    body: { userid: user.userid, value: newGroups },
  })
  user.groups = newGroups
}

function addUser() {
  inputTitle.value = 'Add User'
  inputLabel.value = 'Username'
  inputValue.value = ''
  inputType.value = 'text'
  inputConfirm.value = false
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
  inputType.value = 'text'
  inputConfirm.value = false
  inputAction.value = async (name) => {
    await api('/auth/set_display_name', {
      method: 'POST',
      body: { username: user.username, value: name },
    })
    user.displayName = name
  }
  showInput.value = true
}

function changePassword(user: User) {
  inputTitle.value = `Change Password — ${user.displayName || user.username}`
  inputLabel.value = 'New password'
  inputValue.value = ''
  inputType.value = 'password'
  inputConfirm.value = true
  inputAction.value = async (password) => {
    if (!password) return
    await api('/auth/change_password', {
      method: 'POST',
      body: { username: user.username, password },
    })
    alertTitle.value = 'Password Changed'
    alertMessage.value = `Password for "${user.username}" has been updated.`
    showAlert.value = true
  }
  showInput.value = true
}

function clearSessions(user: User) {
  confirmMessage.value = `Clear all sessions for "${user.displayName || user.username}"? They will be logged out everywhere.`
  confirmAction.value = async () => {
    await api('/auth/clear_sessions', {
      method: 'POST',
      body: { username: user.username },
    })
    alertTitle.value = 'Sessions Cleared'
    alertMessage.value = `All sessions for "${user.username}" have been invalidated.`
    showAlert.value = true
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

onMounted(() => {
  fetchUsers()
  fetchGroups()
})
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
          <th>Groups</th>
          <th class="text-center">Admin</th>
          <th class="text-center">Enabled</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="user in users" :key="user.userid" :class="{ 'opacity-50': !user.enabled && !isSelf(user) }">
          <td>{{ user.username }}</td>
          <td>{{ user.displayName }}</td>
          <td>
            <div v-if="groups.length" class="d-flex flex-wrap gap-1">
              <div v-for="g in groups" :key="g.id" class="form-check form-check-inline mb-0">
                <input
                  class="form-check-input"
                  type="checkbox"
                  :id="`ug-${user.userid}-${g.id}`"
                  :checked="user.groups.includes(g.id)"
                  @change="toggleUserGroup(user, g.id)"
                />
                <label class="form-check-label small" :for="`ug-${user.userid}-${g.id}`">{{ g.displayName }}</label>
              </div>
            </div>
            <span v-else class="text-muted small">No groups</span>
          </td>
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
            <div class="d-flex gap-1 flex-wrap">
              <button class="btn btn-sm btn-outline-secondary" @click="setDisplayName(user)">Rename</button>
              <button class="btn btn-sm btn-outline-secondary" @click="changePassword(user)">Password</button>
              <button class="btn btn-sm btn-outline-warning" @click="clearSessions(user)">Clear Sessions</button>
            </div>
          </td>
        </tr>
      </tbody>
    </table>

    <InputModal
      :show="showInput"
      :title="inputTitle"
      :label="inputLabel"
      :model-value="inputValue"
      :input-type="inputType"
      :confirm="inputConfirm"
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
      :title="alertTitle"
      :message="alertMessage"
      @close="showAlert = false"
    />
  </div>
</template>
