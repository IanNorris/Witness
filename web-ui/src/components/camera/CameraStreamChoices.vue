<script setup lang="ts">
import type { Camera } from '../../types/camera'
import { useSettingsStore } from '../../stores/settings'
import { cameraStreamInfo, streamSummary } from '../../utils/streamChoice'
defineProps<{ cameras: Camera[] }>()
const settings = useSettingsStore()
</script>
<template>
  <section class="stream-choice-settings">
    <h6>Live streams in this browser</h6>
    <p class="small text-muted">Auto follows tile size where supported. Pinned choices override that policy. Recording, detection and existing DVR footage are unchanged.</p>
    <div v-for="camera in cameras" :key="camera.id" class="stream-choice-row">
      <label :for="`stream-choice-${camera.id}`">{{ camera.name }}</label>
      <select :id="`stream-choice-${camera.id}`" class="form-select form-select-sm" :aria-label="`${camera.name} live stream`"
        :value="settings.getCameraStreamChoice(camera.id)" @change="settings.setCameraStreamChoice(camera.id, ($event.target as HTMLSelectElement).value)">
        <option value="auto">Auto</option><option value="main">Main</option><option value="sub" :disabled="!camera.hasSubStream">Sub{{ camera.hasSubStream ? '' : ' — unavailable' }}</option>
      </select>
      <small>{{ streamSummary(cameraStreamInfo(camera, 'main')) }}</small>
      <small v-if="camera.hasSubStream">{{ streamSummary(cameraStreamInfo(camera, 'sub')) }}</small>
      <small v-else>No secondary stream configured</small>
    </div>
  </section>
</template>
<style scoped>
.stream-choice-settings { max-height: 35vh; overflow: auto; min-width: 0; }
.stream-choice-row { display: grid; grid-template-columns: minmax(0, 1fr) 7rem; gap: .3rem .7rem; padding: .65rem 0; border-top: 1px solid var(--bs-border-color); }
.stream-choice-row small { grid-column: 1 / -1; overflow-wrap: anywhere; color: var(--bs-secondary-color); }
</style>
