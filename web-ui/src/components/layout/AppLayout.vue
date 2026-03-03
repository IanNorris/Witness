<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import { RouterLink } from 'vue-router'
import { useAuthStore } from '../../stores/auth'
import { useCameraStore } from '../../stores/cameras'
import { useEventStream } from '../../composables/useEventStream'
import { format } from 'date-fns'

const auth = useAuthStore()
const cameraStore = useCameraStore()
const { connected } = useEventStream()

const clockTime = ref('')
const clockDate = ref('')
let clockTimer: ReturnType<typeof setInterval> | null = null

function updateClock() {
  const now = new Date()
  clockTime.value = format(now, 'HH:mm:ss')
  clockDate.value = format(now, 'EEE, dd MMM')
}

function downloadDiag() {
  const fn = (window as unknown as Record<string, unknown>)._witnessDumpDiag as (() => void) | undefined
  if (fn) fn()
}

onMounted(() => {
  updateClock()
  clockTimer = setInterval(updateClock, 1000)
})
onUnmounted(() => { if (clockTimer) clearInterval(clockTimer) })
</script>

<template>
  <div class="app-wrapper">
    <!-- Sidebar -->
    <aside class="app-sidebar">
      <RouterLink to="/" class="sidebar-brand">
        <svg width="24" height="24" viewBox="0 0 1295 1295" fill="#4AA3DF">
          <path d="M22.055,479.729c75.82,-282.967 332.246,-479.729 625.195,-479.729c292.949,0 549.375,196.762 625.195,479.729l-216.195,57.93c-49.601,-185.116 -217.354,-313.837 -409,-313.837c-191.646,0 -359.399,128.721 -409,313.837l-216.195,-57.93Z"/>
          <path d="M1272.44,814.771c-75.82,282.967 -332.246,479.729 -625.195,479.729c-292.949,0 -549.375,-196.762 -625.195,-479.729l216.195,-57.93c49.601,185.116 217.354,313.837 409,313.837c191.646,0 359.399,-128.721 409,-313.837l216.195,57.93Z"/>
          <path d="M756.401,968.735c132.718,-46.397 221.599,-171.626 221.599,-312.22c0,-182.546 -148.204,-330.75 -330.75,-330.75c-182.546,0 -330.75,148.204 -330.75,330.75c0,140.594 88.881,265.823 221.599,312.22l71.768,-205.289c-45.454,-15.89 -75.895,-58.78 -75.895,-106.931c0,-62.52 50.758,-113.278 113.278,-113.278c62.52,0 113.278,50.758 113.278,113.278c0,48.151 -30.441,91.041 -75.895,106.931l71.768,205.289Z"/>
        </svg>
        Witness
        <span class="sidebar-clock">{{ clockTime }}</span>
      </RouterLink>

      <nav class="sidebar-nav">
        <div class="nav-item">
          <RouterLink to="/" class="nav-link">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/></svg>
            Dashboard
          </RouterLink>
        </div>
        <div class="nav-item">
          <RouterLink to="/clips" class="nav-link">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="23 7 16 12 23 17 23 7"/><rect x="1" y="5" width="15" height="14" rx="2"/></svg>
            All Clips
          </RouterLink>
        </div>

        <div class="sidebar-section">Cameras</div>
        <div
          v-for="camera in cameraStore.cameras"
          :key="camera.id"
          class="nav-item sidebar-camera"
        >
          <RouterLink :to="`/clips/${camera.id}`" class="nav-link">
            <span
              class="status-dot"
              :class="{
                connected: camera.status === 'Connected',
                disconnected: camera.status === 'Disconnected',
                unknown: camera.status !== 'Connected' && camera.status !== 'Disconnected',
              }"
            />
            {{ camera.name }}
          </RouterLink>
        </div>

        <template v-if="auth.isAdmin">
          <div class="sidebar-section">Administration</div>
          <div class="nav-item">
            <RouterLink to="/admin" class="nav-link">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
              Settings
            </RouterLink>
          </div>
        </template>
      </nav>

      <!-- User section at bottom -->
      <div class="p-3 border-top" style="border-color: var(--bs-border-color) !important;">
        <div class="d-flex align-items-center justify-content-between">
          <div class="d-flex align-items-center gap-2">
            <div class="rounded-circle bg-primary d-flex align-items-center justify-content-center" style="width: 32px; height: 32px; font-size: 0.8rem;">
              {{ auth.initials }}
            </div>
            <span class="small">{{ auth.displayName }}</span>
          </div>
          <button class="btn btn-sm btn-outline-secondary" @click="downloadDiag" title="Download HLS diagnostics">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
          </button>
          <button class="btn btn-sm btn-outline-secondary" @click="auth.logout()" title="Logout">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><polyline points="16 17 21 12 16 7"/><line x1="21" y1="12" x2="9" y2="12"/></svg>
          </button>
        </div>
      </div>
    </aside>

    <!-- Main content -->
    <div class="app-main">
      <header class="app-topbar">
        <div class="topbar-title">
          <slot name="title" />
        </div>
        <div class="topbar-actions">
          <slot name="actions" />
        </div>
      </header>

      <main class="app-content">
        <slot />
      </main>
    </div>

    <!-- WebSocket disconnect overlay -->
    <Transition name="fade">
      <div v-if="!connected" class="disconnect-overlay">
        <div class="disconnect-content">
          <svg width="64" height="64" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" class="disconnect-icon">
            <line x1="1" y1="1" x2="23" y2="23" />
            <path d="M16.72 11.06A10.94 10.94 0 0 1 19 12.55" />
            <path d="M5 12.55a10.94 10.94 0 0 1 5.17-2.39" />
            <path d="M10.71 5.05A16 16 0 0 1 22.56 9" />
            <path d="M1.42 9a15.91 15.91 0 0 1 4.7-2.88" />
            <path d="M8.53 16.11a6 6 0 0 1 6.95 0" />
            <line x1="12" y1="20" x2="12.01" y2="20" />
          </svg>
          <h4 class="disconnect-title">Connection Lost</h4>
          <p class="disconnect-text">Attempting to reconnect…</p>
        </div>
      </div>
    </Transition>
  </div>
</template>
