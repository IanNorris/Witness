<script setup lang="ts">
import { ref } from 'vue'
import type { Clip } from '../../types/clip'
import { useClipStore } from '../../stores/clips'

const props = defineProps<{
  clip: Clip
}>()

const emit = defineEmits<{ close: [] }>()

const clipStore = useClipStore()
const videoSrc = ref(clipStore.videoUrl(props.clip.camera, props.clip.timestamp))

function handleKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') emit('close')
}
</script>

<template>
  <Teleport to="body">
    <div class="clip-modal-overlay" @click.self="emit('close')" @keydown="handleKeydown" tabindex="0">
      <div class="clip-modal">
        <div class="clip-modal-header">
          <span>Clip {{ clip.uid }}</span>
          <button class="btn btn-sm btn-outline-secondary" @click="emit('close')">✕</button>
        </div>
        <div class="clip-modal-body">
          <video :src="videoSrc" controls autoplay class="clip-video" />
        </div>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.clip-modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.85);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9999;
}
.clip-modal {
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  max-width: 90vw;
  max-height: 90vh;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.clip-modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 0.5rem 1rem;
  border-bottom: 1px solid var(--bs-border-color, #333);
}
.clip-modal-body {
  padding: 0;
}
.clip-video {
  width: 100%;
  max-height: 80vh;
  display: block;
}
</style>
