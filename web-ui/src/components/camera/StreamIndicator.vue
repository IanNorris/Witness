<script setup lang="ts">
import { ref } from 'vue'
import type { StreamInfo } from '../../utils/streamChoice'
import { streamSummary } from '../../utils/streamChoice'
import { useSettingsStore } from '../../stores/settings'
defineProps<{ cameraId: number; label: string; info: StreamInfo; hasSub?: boolean; replay?: boolean; jpeg?: boolean; codecFallback?: boolean }>()
const settings = useSettingsStore()
const dialog = ref<HTMLDialogElement | null>(null)
const dialogOpen = ref(false)
function openDetails() { if (dialog.value && !dialog.value.open) { dialog.value.showModal(); dialogOpen.value = true } }
function closeDetails() { dialog.value?.close(); dialogOpen.value = false }
</script>
<template>
  <button type="button" class="latency-overlay stream-indicator" :title="jpeg ? `JPEG preview · Camera stream: ${streamSummary(info)}` : streamSummary(info)"
    :aria-label="`Stream details: ${streamSummary(info)}`" aria-haspopup="dialog" :aria-expanded="dialogOpen"
    @click.stop="openDetails" @dblclick.stop @pointerdown.stop>{{ label || 'Stream' }}</button>
  <Teleport to="body">
    <dialog ref="dialog" class="stream-dialog" aria-label="Playback stream details" @click.self="closeDetails" @close="dialogOpen = false">
      <section class="stream-information">
        <header><strong>Playback stream</strong><button type="button" class="btn-close" aria-label="Close stream details" @click="closeDetails" /></header>
        <div>{{ jpeg ? 'Camera stream: ' : '' }}{{ streamSummary(info) }}</div>
        <p v-if="jpeg" class="mb-0">Showing JPEG previews, not video playback. The stream choice remains available below; choose Auto or Sub to try a supported video stream.</p>
        <p v-if="codecFallback" class="small text-warning mb-0" role="status">Video playback is unavailable for this stream in this browser. Its codec or decoder may not be supported on this viewing machine. Live previews can use a supported sub-stream; DVR retains the recorded primary stream.</p>
        <p v-if="replay" class="mb-0">Recorded footage: live stream choices do not change this video.</p>
        <template v-else>
          <label>Live stream in this browser
            <select class="form-select form-select-sm" :value="settings.getCameraStreamChoice(cameraId)" aria-label="Live stream choice"
              @change="settings.setCameraStreamChoice(cameraId, ($event.target as HTMLSelectElement).value)">
              <option value="auto">Auto</option><option value="main">Main</option><option value="sub" :disabled="!hasSub">Sub{{ hasSub ? '' : ' — unavailable' }}</option>
            </select>
          </label>
          <p class="small mb-0">Auto follows tile size where supported. Your choice is saved in this browser only; recording and detection are unchanged.</p>
        </template>
      </section>
    </dialog>
  </Teleport>
</template>
<style scoped>
.stream-indicator { pointer-events: auto !important; z-index: 20 !important; max-width: calc(100% - 1rem); border: 0; cursor: pointer; }
.stream-indicator:focus-visible { outline: 2px solid #93c5fd; outline-offset: 3px; }
.stream-dialog { width: 23rem; max-width: calc(100vw - 2rem); max-height: calc(100vh - 2rem); padding: 0; border: 1px solid var(--bs-border-color); border-radius: 12px; background: var(--bs-body-bg); color: var(--bs-body-color); box-shadow: 0 12px 48px #0008; }
.stream-dialog::backdrop { background: #0008; }
.stream-information { padding: 1rem; display: grid; gap: .8rem; overflow-wrap: anywhere; }
.stream-information header { display: flex; align-items: center; justify-content: space-between; gap: 1rem; }
.stream-information label { display: grid; gap: .4rem; }
</style>
