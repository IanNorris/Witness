import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { api, setCsrfToken } from '../composables/useApi'
import type { AuthProfile } from '../types/user'

export const useAuthStore = defineStore('auth', () => {
  const username = ref('')
  const displayName = ref('')
  const isAdmin = ref(false)
  const csrfToken = ref('')
  const isAuthenticated = ref(false)
  const isLoading = ref(true)

  const initials = computed(() => {
    const name = displayName.value || username.value
    return name ? name.charAt(0).toUpperCase() : '?'
  })

  async function fetchProfile() {
    try {
      const profile = await api<AuthProfile>('/auth/profile')
      if (profile && profile.csrf) {
        csrfToken.value = profile.csrf
        setCsrfToken(profile.csrf)
        username.value = profile.username
        displayName.value = profile.displayName || profile.username
        isAdmin.value = profile.admin
        isAuthenticated.value = true
      }
    } catch {
      isAuthenticated.value = false
    } finally {
      isLoading.value = false
    }
  }

  async function login(user: string, password: string): Promise<boolean> {
    try {
      const result = await api<AuthProfile>('/auth/login', {
        method: 'POST',
        body: { username: user, password },
        redirectOnFail: false,
      })
      if (result && result.csrf) {
        csrfToken.value = result.csrf
        setCsrfToken(result.csrf)
        username.value = result.username
        displayName.value = result.displayName || result.username
        isAdmin.value = result.admin
        isAuthenticated.value = true
        return true
      }
      return false
    } catch {
      return false
    }
  }

  async function logout() {
    try {
      await api('/auth/logout', { method: 'POST' })
    } finally {
      isAuthenticated.value = false
      username.value = ''
      isAdmin.value = false
      csrfToken.value = ''
    }
  }

  return {
    username,
    displayName,
    isAdmin,
    csrfToken,
    isAuthenticated,
    isLoading,
    initials,
    fetchProfile,
    login,
    logout,
  }
})
