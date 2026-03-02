<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue'
import { useSettingsStore } from './stores/settings'
import { useEventStream } from './composables/useEventStream'

const settings = useSettingsStore()
const events = useEventStream()

onMounted(() => {
  events.installCameraHandler()
  events.installClipHandler()
  events.connect()
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
