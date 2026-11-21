<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useDashboardStore } from '../../stores/dashboards'

// Compact picker for every dashboard the viewer can see. "Overview" (null)
// is the zero-configuration grid of all accessible cameras.
const props = defineProps<{ modelValue: number | null; allowOverview?: boolean; dark?: boolean }>()
const emit = defineEmits<{ 'update:modelValue': [id: number | null] }>()
const store = useDashboardStore()
const value = computed(() => props.modelValue === null ? '' : String(props.modelValue))
function change(event: Event) {
  const raw = (event.target as HTMLSelectElement).value
  emit('update:modelValue', raw === '' ? null : Number(raw))
}
onMounted(() => { if (!store.loaded) void store.fetchList().catch(() => undefined) })
</script>

<template>
  <select class="form-select form-select-sm dashboard-switcher" :class="{ dark }" :value="value" aria-label="Dashboard" @change="change">
    <option v-if="allowOverview" value="">Overview (all cameras)</option>
    <optgroup v-if="store.personal.length" label="My dashboards">
      <option v-for="d in store.personal" :key="d.id" :value="String(d.id)">{{ d.name }}</option>
    </optgroup>
    <optgroup v-if="store.shared.length" label="Shared">
      <option v-for="d in store.shared" :key="d.id" :value="String(d.id)">{{ d.name }}</option>
    </optgroup>
  </select>
</template>

<style scoped>
.dashboard-switcher { width: auto; max-width: 260px; }
.dashboard-switcher.dark { background-color: #142235; color: #dceafa; border-color: #456; }
</style>
