<script setup lang="ts">
import { onMounted } from 'vue'
import { RouterLink, RouterView } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import { useCameraStore } from '../stores/cameras'

const cameraStore = useCameraStore()

onMounted(async () => {
  if (cameraStore.cameras.length === 0) {
    await cameraStore.fetchCameras()
  }
})

const tabs = [
  { path: '/admin/cameras', label: 'Cameras' },
  { path: '/admin/users', label: 'Users' },
  { path: '/admin/groups', label: 'Groups' },
  { path: '/admin/detection', label: 'Detection' },
  { path: '/admin/storage', label: 'Storage' },
  { path: '/admin/tags', label: 'Tags' },
  { path: '/admin/debug', label: 'Debug' },
]
</script>

<template>
  <AppLayout>
    <template #title>Administration</template>

    <ul class="nav nav-tabs mb-3">
      <li v-for="tab in tabs" :key="tab.path" class="nav-item">
        <RouterLink :to="tab.path" class="nav-link" active-class="active">
          {{ tab.label }}
        </RouterLink>
      </li>
    </ul>

    <RouterView />
  </AppLayout>
</template>
