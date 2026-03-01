<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { useAuthStore } from '../stores/auth'

const auth = useAuthStore()
const router = useRouter()

const username = ref('')
const password = ref('')
const error = ref('')
const isSubmitting = ref(false)

async function handleLogin() {
  error.value = ''
  isSubmitting.value = true
  try {
    const success = await auth.login(username.value, password.value)
    if (success) {
      router.push('/')
    } else {
      error.value = 'Invalid username or password'
    }
  } catch {
    error.value = 'Connection failed'
  } finally {
    isSubmitting.value = false
  }
}
</script>

<template>
  <div class="login-container">
    <div class="login-card">
      <div class="text-center mb-4">
        <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="#4a90d9" stroke-width="2">
          <circle cx="12" cy="12" r="3" /><path d="M2 12s4-8 10-8 10 8 10 8-4 8-10 8-10-8-10-8z" />
        </svg>
        <h4 class="mt-2 mb-0">Witness</h4>
        <p class="text-muted-custom small">Sign in to continue</p>
      </div>

      <form @submit.prevent="handleLogin">
        <div class="mb-3">
          <label for="username" class="form-label small">Username</label>
          <input
            id="username"
            v-model="username"
            type="text"
            class="form-control"
            autocomplete="username"
            autofocus
            required
          />
        </div>

        <div class="mb-3">
          <label for="password" class="form-label small">Password</label>
          <input
            id="password"
            v-model="password"
            type="password"
            class="form-control"
            autocomplete="current-password"
            required
          />
        </div>

        <div v-if="error" class="alert alert-danger py-2 small">
          {{ error }}
        </div>

        <button
          type="submit"
          class="btn btn-primary w-100"
          :disabled="isSubmitting"
        >
          {{ isSubmitting ? 'Signing in...' : 'Sign In' }}
        </button>
      </form>
    </div>
  </div>
</template>
