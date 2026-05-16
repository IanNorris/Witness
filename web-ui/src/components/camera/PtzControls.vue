<script setup lang="ts">
import { ref } from 'vue'
import { api } from '../../composables/useApi'

const props = defineProps<{
  cameraId: number
}>()

const isMoving = ref(false)
const error = ref('')
const speed = ref(32)

async function sendPtz(command: string) {
  error.value = ''
  try {
    await api(`/ptz/${props.cameraId}/${command}`, {
      method: 'POST',
      body: { speed: speed.value },
    })
  } catch (e: any) {
    error.value = e.message
  }
}

function startMove(command: string) {
  isMoving.value = true
  sendPtz(command)
}

function stopMove() {
  if (isMoving.value) {
    isMoving.value = false
    sendPtz('stop')
  }
}
</script>

<template>
  <div class="ptz-controls">
    <!-- D-pad -->
    <div class="ptz-dpad">
      <button
        class="ptz-btn ptz-up"
        @mousedown="startMove('up')"
        @mouseup="stopMove"
        @mouseleave="stopMove"
        @touchstart.prevent="startMove('up')"
        @touchend.prevent="stopMove"
        title="Tilt Up"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M12 4l-8 8h16z"/>
        </svg>
      </button>

      <button
        class="ptz-btn ptz-left"
        @mousedown="startMove('left')"
        @mouseup="stopMove"
        @mouseleave="stopMove"
        @touchstart.prevent="startMove('left')"
        @touchend.prevent="stopMove"
        title="Pan Left"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M4 12l8-8v16z"/>
        </svg>
      </button>

      <button
        class="ptz-btn ptz-center"
        @click="sendPtz('stop')"
        title="Stop"
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
          <rect x="6" y="6" width="12" height="12" rx="2"/>
        </svg>
      </button>

      <button
        class="ptz-btn ptz-right"
        @mousedown="startMove('right')"
        @mouseup="stopMove"
        @mouseleave="stopMove"
        @touchstart.prevent="startMove('right')"
        @touchend.prevent="stopMove"
        title="Pan Right"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M20 12l-8-8v16z"/>
        </svg>
      </button>

      <button
        class="ptz-btn ptz-down"
        @mousedown="startMove('down')"
        @mouseup="stopMove"
        @mouseleave="stopMove"
        @touchstart.prevent="startMove('down')"
        @touchend.prevent="stopMove"
        title="Tilt Down"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M12 20l8-8H4z"/>
        </svg>
      </button>
    </div>

    <!-- Zoom controls -->
    <div class="ptz-zoom">
      <button
        class="ptz-btn ptz-zoom-btn"
        @mousedown="startMove('zoomin')"
        @mouseup="stopMove"
        @mouseleave="stopMove"
        @touchstart.prevent="startMove('zoomin')"
        @touchend.prevent="stopMove"
        title="Zoom In"
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5">
          <line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/>
        </svg>
      </button>
      <button
        class="ptz-btn ptz-zoom-btn"
        @mousedown="startMove('zoomout')"
        @mouseup="stopMove"
        @mouseleave="stopMove"
        @touchstart.prevent="startMove('zoomout')"
        @touchend.prevent="stopMove"
        title="Zoom Out"
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5">
          <line x1="5" y1="12" x2="19" y2="12"/>
        </svg>
      </button>
    </div>

    <!-- Speed slider -->
    <div class="ptz-speed">
      <label class="ptz-speed-label">Speed</label>
      <input
        type="range"
        v-model.number="speed"
        min="1"
        max="64"
        step="1"
        class="ptz-speed-slider"
      />
      <span class="ptz-speed-value">{{ speed }}</span>
    </div>

    <!-- Error display -->
    <div v-if="error" class="ptz-error">{{ error }}</div>
  </div>
</template>

<style scoped>
.ptz-controls {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
  padding: 8px;
  background: rgba(0, 0, 0, 0.7);
  border-radius: 8px;
  backdrop-filter: blur(4px);
  user-select: none;
}

.ptz-dpad {
  display: grid;
  grid-template-columns: 32px 32px 32px;
  grid-template-rows: 32px 32px 32px;
  gap: 2px;
}

.ptz-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  border: none;
  border-radius: 4px;
  background: rgba(255, 255, 255, 0.15);
  color: white;
  cursor: pointer;
  transition: background 0.1s;
}

.ptz-btn:hover {
  background: rgba(255, 255, 255, 0.3);
}

.ptz-btn:active {
  background: rgba(59, 130, 246, 0.6);
}

.ptz-up { grid-column: 2; grid-row: 1; }
.ptz-left { grid-column: 1; grid-row: 2; }
.ptz-center { grid-column: 2; grid-row: 2; }
.ptz-right { grid-column: 3; grid-row: 2; }
.ptz-down { grid-column: 2; grid-row: 3; }

.ptz-zoom {
  display: flex;
  gap: 4px;
}

.ptz-zoom-btn {
  width: 36px;
  height: 28px;
}

.ptz-speed {
  display: flex;
  align-items: center;
  gap: 4px;
  width: 100%;
}

.ptz-speed-label {
  color: rgba(255, 255, 255, 0.7);
  font-size: 10px;
  white-space: nowrap;
}

.ptz-speed-slider {
  flex: 1;
  height: 4px;
  accent-color: #3b82f6;
}

.ptz-speed-value {
  color: rgba(255, 255, 255, 0.9);
  font-size: 10px;
  min-width: 16px;
  text-align: right;
}

.ptz-error {
  color: #ef4444;
  font-size: 10px;
  text-align: center;
  max-width: 120px;
  word-break: break-word;
}
</style>
