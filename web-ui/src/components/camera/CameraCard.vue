<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import type { Camera } from '../../types/camera'
import { useSettingsStore } from '../../stores/settings'
import { useCameraStore } from '../../stores/cameras'
import HlsPlayer from './HlsPlayer.vue'

const props = defineProps<{
  camera: Camera
}>()

const emit = defineEmits<{
  openStream: [cameraId: number]
  openClips: [cameraId: number]
}>()

const settings = useSettingsStore()
const cameraStore = useCameraStore()
const imgSrc = ref('')
const isConnected = ref(false)
const imgRef = ref<HTMLImageElement | null>(null)
let refreshTimer: ReturnType<typeof setInterval> | null = null
let jpegRunning = false
let clickTimer: ReturnType<typeof setTimeout> | null = null

function refreshPreview() {
  if (props.camera.status === 'Connected') {
    isConnected.value = true
    imgSrc.value = `/camera/preview/${props.camera.id}?t=${Date.now()}`
  } else {
    isConnected.value = false
  }
}

// Continuous JPEG: reload as soon as previous frame loads
function startJpegLoop() {
  jpegRunning = true
  loadNextFrame()
}

function loadNextFrame() {
  if (!jpegRunning) return
  const img = imgRef.value
  if (!img) {
    refreshTimer = setTimeout(loadNextFrame, 100)
    return
  }
  imgSrc.value = `/camera/preview/${props.camera.id}?t=${Date.now()}`
}

function onJpegLoad() {
  if (jpegRunning) {
    refreshTimer = setTimeout(loadNextFrame, 33)
  }
}

function onJpegError() {
  if (jpegRunning) {
    refreshTimer = setTimeout(loadNextFrame, 1000)
  }
}

// Single click = toggle recording, double click = open stream / exit fullscreen
function onSingleClick() {
  if (clickTimer) { clearTimeout(clickTimer); clickTimer = null }
  clickTimer = setTimeout(() => {
    clickTimer = null
    cameraStore.toggleRecording(props.camera.id)
  }, 250)
}

function onDoubleClick() {
  if (clickTimer) { clearTimeout(clickTimer); clickTimer = null }
  if (settings.fullscreenMode) {
    settings.toggleFullscreen()
  } else {
    emit('openStream', props.camera.id)
  }
}

onMounted(() => {
  refreshPreview()
  if (settings.streamingMode === 'jpeg') {
    startJpegLoop()
  }
})

onUnmounted(() => {
  jpegRunning = false
  if (refreshTimer) clearTimeout(refreshTimer)
  if (clickTimer) clearTimeout(clickTimer)
})
</script>

<template>
  <div class="camera-card" :class="{ 'camera-card-fullscreen': settings.fullscreenMode }">
    <div class="camera-preview">
      <div class="camera-video-wrap" @click.prevent="onSingleClick" @dblclick.prevent="onDoubleClick">
        <!-- HLS preview mode -->
        <HlsPlayer
          v-if="settings.streamingMode === 'hls' && isConnected"
          :camera-id="camera.id"
          :low-latency="camera.lowLatencyHLS"
        />

        <!-- JPEG preview mode -->
        <img
          v-else-if="imgSrc && settings.streamingMode === 'jpeg'"
          ref="imgRef"
          :src="imgSrc"
          :alt="camera.name"
          @load="onJpegLoad"
          @error="onJpegError"
        />

        <!-- Disconnected state -->
        <div v-if="!isConnected" class="camera-overlay connection-lost">
          <div class="text-center">
            <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <line x1="1" y1="1" x2="23" y2="23"/><path d="M16.72 11.06A10.94 10.94 0 0 1 19 12.55"/><path d="M5 12.55a10.94 10.94 0 0 1 5.17-2.39"/>
            </svg>
            <div class="mt-1">Disconnected</div>
          </div>
        </div>

        <!-- REC overlay -->
        <div v-if="camera.isRecording" class="rec-overlay">
          <span class="rec-dot" /> REC
        </div>

        <!-- Camera name overlay in fullscreen -->
        <div v-if="settings.fullscreenMode" class="camera-name-overlay">
          {{ camera.name }}
        </div>
      </div>
    </div>

    <!-- Info bar: hidden in fullscreen -->
    <div v-if="!settings.fullscreenMode" class="camera-info">
      <div>
        <div class="camera-name">{{ camera.name }}</div>
        <div class="camera-status">
          <span>{{ camera.status }}</span>
        </div>
      </div>
      <div class="d-flex gap-1">
        <button
          class="btn btn-sm btn-outline-secondary"
          @click.stop="emit('openClips', camera.id)"
          title="Clips"
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polygon points="23 7 16 12 23 17 23 7"/><rect x="1" y="5" width="15" height="14" rx="2"/>
          </svg>
        </button>
        <button
          class="btn btn-sm btn-outline-secondary"
          @click.stop="emit('openStream', camera.id)"
          title="Live stream"
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polygon points="5 3 19 12 5 21 5 3"/>
          </svg>
        </button>
      </div>
    </div>
  </div>
</template>
