<script setup lang="ts">
import { computed, onMounted, onUnmounted, watch } from 'vue'
import { useSettingsStore } from './stores/settings'
import { useAuthStore } from './stores/auth'
import { useEventStream } from './composables/useEventStream'
import { playbackWarning } from './composables/playbackGuard'
import { decoderPressureWarning, playbackResourceSnapshot } from './composables/usePlaybackResources'
import { useCameraStore } from './stores/cameras'
import { isH264 } from './utils/streamChoice'

const settings = useSettingsStore()
const cameras = useCameraStore()
const eligibleDowngrades = computed(() => decoderPressureWarning.value ? cameras.cameras.filter(camera =>
  camera.hasSubStream && isH264(camera.subCodec) && decoderPressureWarning.value!.streams.some(stream =>
    stream.cameraId === camera.id && (stream.mode === 'mse' || stream.mode === 'hls') && stream.tier !== 'sub')) : [])
function reduceDecoderLoad() {
  const live = playbackResourceSnapshot()
  eligibleDowngrades.value.forEach(camera => {
    if (live.some(stream => stream.cameraId === camera.id && (stream.mode === 'mse' || stream.mode === 'hls') && stream.tier !== 'sub')) settings.setCameraStreamChoice(camera.id, 'sub')
  })
  decoderPressureWarning.value = null
}
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
  if (!loggedIn) decoderPressureWarning.value = null
})

onUnmounted(() => {
  events.disconnect()
})
</script>

<template>
  <div :class="{ 'fullscreen-active': settings.fullscreenMode }">
    <router-view />
    <div v-if="decoderPressureWarning" class="decoder-pressure-warning alert alert-warning shadow" role="status" aria-live="polite">
      <div><strong>Possible browser decoding limit</strong>
        <p>Camera {{ cameras.getCameraById(decoderPressureWarning.cameraId)?.name || decoderPressureWarning.cameraId }} could not decode video with {{ decoderPressureWarning.activeStreams }} streams open in this tab. Try fewer cameras or H.264 sub-streams. Other tabs and apps may also use decoding resources.</p>
        <p class="small">This is suspected resource pressure, not a confirmed limit. Changing live streams does not convert H.265 DVR recordings.</p>
        <button v-if="eligibleDowngrades.length" class="btn btn-sm btn-outline-dark" @click="reduceDecoderLoad">Use H.264 sub-streams for {{ eligibleDowngrades.length }} live camera(s)</button>
      </div>
      <button class="btn-close ms-3" aria-label="Dismiss decoder warning" @click="decoderPressureWarning = null" />
    </div>
    <div v-if="playbackWarning" class="playback-warning alert alert-warning shadow" role="status" aria-live="polite">
      <span>{{ playbackWarning }}</span>
      <button class="btn-close ms-3" aria-label="Dismiss audio warning" @click="playbackWarning = ''" />
    </div>
  </div>
</template>

<style scoped>
.playback-warning { position: fixed; bottom: 1rem; right: 1rem; z-index: 10000; max-width: min(32rem, calc(100vw - 2rem)); display: flex; align-items: flex-start; }
.decoder-pressure-warning { position: fixed; top: 1rem; right: 1rem; z-index: 10001; max-width: min(35rem, calc(100vw - 2rem)); display: flex; align-items: flex-start; }
.decoder-pressure-warning p { margin: .5rem 0; }
</style>
