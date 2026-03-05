<script setup lang="ts">
import { onMounted, onUnmounted, watch } from 'vue'
import { useSettingsStore } from './stores/settings'
import { useAuthStore } from './stores/auth'
import { useEventStream } from './composables/useEventStream'

const settings = useSettingsStore()
const auth = useAuthStore()
const events = useEventStream()

function startEventStream() {
  events.installBuildHashHandler()
  events.installCameraHandler()
  events.installClipHandler()
  events.connect()
}

onMounted(() => {
  if (auth.isAuthenticated) startEventStream()
})

watch(() => auth.isAuthenticated, (loggedIn) => {
  if (loggedIn) startEventStream()
  else events.disconnect()
})

onUnmounted(() => {
  events.disconnect()
})
</script>

<template>
  <div :class="{ 'fullscreen-active': settings.fullscreenMode }">
    <router-view />
  </div>
</template>
