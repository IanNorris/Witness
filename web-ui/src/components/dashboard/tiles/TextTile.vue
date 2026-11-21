<script setup lang="ts">
defineProps<{ text: string; size: 's' | 'm' | 'l' }>()
</script>

<template>
  <div class="text-tile" :class="`size-${size}`">{{ text }}</div>
</template>

<style scoped>
.text-tile { container-type: size; width: 100%; height: 100%; display: flex; align-items: center; justify-content: center; padding: .4rem .7rem; background: #0b1119; color: var(--witness-ink, #edf3fc); font-weight: 600; text-align: center; overflow: hidden; overflow-wrap: anywhere; }
.size-s { font-size: clamp(.7rem, 30cqh, 1.1rem); }
.size-m { font-size: clamp(.8rem, 40cqh, 2rem); }
.size-l { font-size: clamp(1rem, 55cqh, 4rem); }
</style>
