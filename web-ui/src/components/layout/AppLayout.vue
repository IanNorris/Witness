<script setup lang="ts">
import { RouterLink } from 'vue-router'
import { useAuthStore } from '../../stores/auth'
import { useCameraStore } from '../../stores/cameras'

const auth = useAuthStore()
const cameraStore = useCameraStore()
</script>

<template>
  <div class="app-wrapper">
    <!-- Sidebar -->
    <aside class="app-sidebar">
      <RouterLink to="/" class="sidebar-brand">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
          <circle cx="12" cy="12" r="3" /><path d="M2 12s4-8 10-8 10 8 10 8-4 8-10 8-10-8-10-8z" />
        </svg>
        Witness
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
  </div>
</template>
