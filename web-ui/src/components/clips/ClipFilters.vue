<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useFilterStore } from '../../stores/filters'
import { useTagStore } from '../../stores/tags'

const filterStore = useFilterStore()
const tagStore = useTagStore()

onMounted(() => {
  if (tagStore.tags.length === 0) tagStore.fetchTags()
})

const visibleTags = computed(() => tagStore.tags.filter(t => !t.hidden))

const presets = [
  { key: 'unreviewed', label: 'Unreviewed' },
  { key: 'saved', label: 'Saved' },
  { key: 'manual', label: 'Manual' },
] as const

function togglePreset(key: string) {
  if (filterStore.activePreset === key) {
    filterStore.clearFilters()
  } else {
    filterStore.setPreset(key)
  }
}

function cycleLighting() {
  const cur = filterStore.activeFilters.lighting
  if (cur === undefined) filterStore.setFilter('lighting', 1)        // → Day
  else if (cur === 1) filterStore.setFilter('lighting', 2)           // → Night
  else filterStore.clearFilter('lighting')                           // → All
}

const lightingLabel = computed(() => {
  const v = filterStore.activeFilters.lighting
  if (v === 1) return '☀ Day'
  if (v === 2) return '🌙 Night'
  return '💡 All'
})

function cycleMode() {
  const cur = filterStore.activeFilters.mode
  if (cur === undefined) filterStore.setFilter('mode', 1)            // → Auto
  else if (cur === 1) filterStore.setFilter('mode', 0)               // → Manual
  else filterStore.clearFilter('mode')                               // → All
}

const modeLabel = computed(() => {
  const v = filterStore.activeFilters.mode
  if (v === 1) return 'Auto'
  if (v === 0) return 'Manual'
  return 'Mode: All'
})

function onDurationInput(e: Event) {
  const val = Number((e.target as HTMLInputElement).value)
  if (val > 0) filterStore.setFilter('minDuration', val)
  else filterStore.clearFilter('minDuration')
}
</script>

<template>
  <div class="clip-filters">
    <!-- Quick presets -->
    <div class="filter-group">
      <button
        v-for="p in presets"
        :key="p.key"
        class="filter-btn"
        :class="{ active: filterStore.activePreset === p.key }"
        @click="togglePreset(p.key)"
      >{{ p.label }}</button>

      <button class="filter-btn" :class="{ active: filterStore.activeFilters.lighting !== undefined }" @click="cycleLighting">
        {{ lightingLabel }}
      </button>
      <button class="filter-btn" :class="{ active: filterStore.activeFilters.mode !== undefined }" @click="cycleMode">
        {{ modeLabel }}
      </button>

      <label class="filter-duration" title="Min duration (seconds)">
        <span class="duration-label">≥</span>
        <input
          type="number"
          min="0"
          max="60"
          :value="filterStore.activeFilters.minDuration ?? 0"
          @input="onDurationInput"
          class="duration-input"
        />
        <span class="duration-label">s</span>
      </label>

      <button
        v-if="filterStore.hasActiveFilters"
        class="filter-btn clear-btn"
        @click="filterStore.clearFilters()"
      >✕ Clear</button>
    </div>

    <!-- Tag chips -->
    <div v-if="visibleTags.length > 0" class="filter-tags">
      <button
        v-for="tag in visibleTags"
        :key="tag.id"
        class="tag-chip"
        :class="{ active: filterStore.activeFilters.tags.includes(tag.name) }"
        @click="filterStore.toggleTag(tag.name)"
      >
        <span v-if="tag.icon" class="tag-icon">{{ tag.icon }}</span>
        {{ tag.display || tag.name }}
      </button>
    </div>
  </div>
</template>

<style scoped>
.clip-filters {
  padding: 0.5rem 0;
  display: flex;
  flex-direction: column;
  gap: 0.4rem;
}

.filter-group {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.35rem;
}

.filter-btn {
  background: var(--bs-dark, #1e1e2e);
  color: var(--bs-body-color, #c9d1d9);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.35rem;
  padding: 0.2rem 0.6rem;
  font-size: 0.78rem;
  cursor: pointer;
  transition: background 0.15s, border-color 0.15s;
  white-space: nowrap;
}
.filter-btn:hover {
  border-color: #555;
}
.filter-btn.active {
  background: #2563eb;
  border-color: #2563eb;
  color: #fff;
}
.clear-btn {
  color: #ef4444;
  border-color: #ef4444;
}
.clear-btn:hover {
  background: #ef4444;
  color: #fff;
}

.filter-duration {
  display: inline-flex;
  align-items: center;
  gap: 0.15rem;
  font-size: 0.78rem;
  color: var(--bs-body-color, #c9d1d9);
}
.duration-input {
  width: 3rem;
  background: var(--bs-dark, #1e1e2e);
  color: var(--bs-body-color, #c9d1d9);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.25rem;
  padding: 0.15rem 0.25rem;
  font-size: 0.78rem;
  text-align: center;
}
.duration-input:focus {
  outline: none;
  border-color: #2563eb;
}
.duration-label {
  opacity: 0.6;
}

.filter-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 0.3rem;
}

.tag-chip {
  display: inline-flex;
  align-items: center;
  gap: 0.25rem;
  background: var(--bs-dark, #1e1e2e);
  color: var(--bs-body-color, #c9d1d9);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 1rem;
  padding: 0.15rem 0.55rem;
  font-size: 0.75rem;
  cursor: pointer;
  transition: background 0.15s, border-color 0.15s;
}
.tag-chip:hover {
  border-color: #555;
}
.tag-chip.active {
  background: #1d4ed8;
  border-color: #1d4ed8;
  color: #fff;
}
.tag-icon {
  font-size: 0.85rem;
}
</style>
