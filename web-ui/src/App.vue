<script setup lang="ts">
import { onMounted, onUnmounted, watch } from 'vue'
import { useSettingsStore } from './stores/settings'
import { useAuthStore } from './stores/auth'
import { useEventStream } from './composables/useEventStream'
import { playbackWarning } from './composables/playbackGuard'

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
    <div v-if="playbackWarning" class="playback-warning alert alert-warning shadow" role="status" aria-live="polite">
      <span>{{ playbackWarning }}</span>
      <button class="btn-close ms-3" aria-label="Dismiss audio warning" @click="playbackWarning = ''" />
    </div>
  </div>
</template>

<style scoped>
.playback-warning { position: fixed; bottom: 1rem; right: 1rem; z-index: 10000; max-width: min(32rem, calc(100vw - 2rem)); display: flex; align-items: flex-start; }
</style>
