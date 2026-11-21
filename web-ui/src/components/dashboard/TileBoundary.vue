<script setup lang="ts">
import { onErrorCaptured, ref } from 'vue'

// Contains a failing tile so one broken widget cannot blank a wall display.
const failed = ref(false)
onErrorCaptured(error => {
  console.error('[Dashboard] Tile failed:', error)
  failed.value = true
  return false
})
</script>

<template>
  <div v-if="failed" class="tile-failed" role="status">
    <span>This tile stopped working.</span>
    <button type="button" @click="failed = false">Retry</button>
  </div>
  <slot v-else />
</template>

<style scoped>
.tile-failed { width: 100%; height: 100%; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: .5rem; background: #0b1017; color: var(--witness-muted, #a5b6cc); font-size: .75rem; }
.tile-failed button { border: 1px solid #3a4b61; border-radius: .35rem; background: #142235; color: #dceafa; padding: .2rem .6rem; font-size: .72rem; }
</style>
