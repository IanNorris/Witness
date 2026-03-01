<script setup lang="ts">
import { useSettingsStore } from './stores/settings'
const settings = useSettingsStore()
</script>

<template>
  <div :class="{ 'fullscreen-active': settings.fullscreenMode }">
    <router-view />
  </div>
</template>
