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
        <svg width="48" height="48" viewBox="0 0 1295 1295" fill="#4AA3DF">
          <path d="M22.055,479.729c75.82,-282.967 332.246,-479.729 625.195,-479.729c292.949,0 549.375,196.762 625.195,479.729l-216.195,57.93c-49.601,-185.116 -217.354,-313.837 -409,-313.837c-191.646,0 -359.399,128.721 -409,313.837l-216.195,-57.93Z"/>
          <path d="M1272.44,814.771c-75.82,282.967 -332.246,479.729 -625.195,479.729c-292.949,0 -549.375,-196.762 -625.195,-479.729l216.195,-57.93c49.601,185.116 217.354,313.837 409,313.837c191.646,0 359.399,-128.721 409,-313.837l216.195,57.93Z"/>
          <path d="M756.401,968.735c132.718,-46.397 221.599,-171.626 221.599,-312.22c0,-182.546 -148.204,-330.75 -330.75,-330.75c-182.546,0 -330.75,148.204 -330.75,330.75c0,140.594 88.881,265.823 221.599,312.22l71.768,-205.289c-45.454,-15.89 -75.895,-58.78 -75.895,-106.931c0,-62.52 50.758,-113.278 113.278,-113.278c62.52,0 113.278,50.758 113.278,113.278c0,48.151 -30.441,91.041 -75.895,106.931l71.768,205.289Z"/>
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
