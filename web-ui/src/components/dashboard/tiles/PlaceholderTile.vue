<script setup lang="ts">
// Quiet placeholders: a tile the viewer cannot access, a camera currently
// shown in a spotlight, a missing camera, or a spotlight waiting for motion.
defineProps<{ kind: 'unavailable' | 'borrowed' | 'waiting' | 'missing'; label?: string }>()
</script>

<template>
  <div class="placeholder-tile" :class="kind" role="note">
    <svg v-if="kind === 'unavailable'" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">
      <rect x="5" y="11" width="14" height="10" rx="2" /><path d="M8 11V8a4 4 0 0 1 8 0v3" />
    </svg>
    <svg v-else-if="kind === 'waiting'" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">
      <circle cx="12" cy="12" r="9" /><path d="M12 7v5l3 2" />
    </svg>
    <span>{{ label ?? (kind === 'unavailable' ? 'Not available for your account' : kind === 'borrowed' ? 'Shown in spotlight' : kind === 'missing' ? 'Camera unavailable' : 'Waiting for motion') }}</span>
  </div>
</template>

<style scoped>
.placeholder-tile { width: 100%; height: 100%; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: .45rem; background: #0b1017; color: var(--witness-muted, #a5b6cc); font-size: .75rem; text-align: center; padding: .5rem; }
.placeholder-tile.waiting { background: #070b10; }
</style>
