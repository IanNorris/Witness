<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useFilterStore } from '../../stores/filters'
import { useTagStore } from '../../stores/tags'

const filterStore = useFilterStore()
const tagStore = useTagStore()

onMounted(() => {
  if (tagStore.tags.length === 0) tagStore.fetchTags()
})

const visibleGroups = computed(() =>
  tagStore.groupedTags.filter(g => !g.hidden && g.clipCount > 0)
)

function isGroupActive(names: string[]) {
  return names.some(n => filterStore.activeFilters.tags.includes(n))
}

function toggleGroup(names: string[]) {
  const active = isGroupActive(names)
  if (active) {
    // Remove all names in this group
    filterStore.activeFilters.tags = filterStore.activeFilters.tags.filter(t => !names.includes(t))
  } else {
    // Add all names in this group
    for (const n of names) {
      if (!filterStore.activeFilters.tags.includes(n)) {
        filterStore.activeFilters.tags.push(n)
      }
    }
  }
}
</script>

<template>
  <div class="filter-panel">
    <div class="filter-panel-header">
      <span class="filter-panel-title">Filters</span>
      <button
        v-if="filterStore.hasActiveFilters"
        class="filter-clear-btn"
        @click="filterStore.clearFilters()"
      >Clear all</button>
    </div>

    <!-- Status -->
    <div class="filter-section">
      <div class="filter-section-label">Status</div>
      <div class="filter-options">
        <button class="filter-opt" :class="{ active: filterStore.activeFilters.reviewed === 0 }"
          @click="filterStore.activeFilters.reviewed === 0 ? filterStore.clearFilter('reviewed') : filterStore.setFilter('reviewed', 0)">
          Unreviewed
        </button>
        <button class="filter-opt" :class="{ active: filterStore.activeFilters.saved === 1 }"
          @click="filterStore.activeFilters.saved === 1 ? filterStore.clearFilter('saved') : filterStore.setFilter('saved', 1)">
          ★ Saved
        </button>
      </div>
    </div>

    <!-- Recording Mode -->
    <div class="filter-section">
      <div class="filter-section-label">Recording</div>
      <div class="filter-options">
        <button class="filter-opt" :class="{ active: filterStore.activeFilters.mode === 1 }"
          @click="filterStore.activeFilters.mode === 1 ? filterStore.clearFilter('mode') : filterStore.setFilter('mode', 1)">
          Auto
        </button>
        <button class="filter-opt" :class="{ active: filterStore.activeFilters.mode === 0 }"
          @click="filterStore.activeFilters.mode === 0 ? filterStore.clearFilter('mode') : filterStore.setFilter('mode', 0)">
          Manual
        </button>
      </div>
    </div>

    <!-- Lighting -->
    <div class="filter-section">
      <div class="filter-section-label">Lighting</div>
      <div class="filter-options">
        <button class="filter-opt" :class="{ active: filterStore.activeFilters.lighting === 1 }"
          @click="filterStore.activeFilters.lighting === 1 ? filterStore.clearFilter('lighting') : filterStore.setFilter('lighting', 1)">
          ☀ Day
        </button>
        <button class="filter-opt" :class="{ active: filterStore.activeFilters.lighting === 2 }"
          @click="filterStore.activeFilters.lighting === 2 ? filterStore.clearFilter('lighting') : filterStore.setFilter('lighting', 2)">
          🌙 Night
        </button>
      </div>
    </div>

    <!-- Min Duration -->
    <div class="filter-section">
      <div class="filter-section-label">Min duration</div>
      <div class="filter-duration-row">
        <input
          type="range" min="0" max="30" step="1"
          :value="filterStore.activeFilters.minDuration ?? 0"
          @input="(e: Event) => { const v = Number((e.target as HTMLInputElement).value); v > 0 ? filterStore.setFilter('minDuration', v) : filterStore.clearFilter('minDuration') }"
          class="duration-slider"
        />
        <span class="duration-value">{{ filterStore.activeFilters.minDuration ?? 0 }}s</span>
      </div>
    </div>

    <!-- Tags -->
    <div v-if="visibleGroups.length > 0" class="filter-section">
      <div class="filter-section-label">Tags</div>
      <div class="filter-tags">
        <button
          v-for="group in visibleGroups"
          :key="group.display"
          class="tag-chip"
          :class="{ active: isGroupActive(group.names) }"
          @click="toggleGroup(group.names)"
        >
          <span v-if="group.icon" class="tag-icon">{{ group.icon }}</span>
          {{ group.display }}
        </button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.filter-panel {
  background: var(--bs-dark, #1e1e2e);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.5rem;
  padding: 0.75rem;
  margin-bottom: 0;
}
.filter-panel-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 0.5rem;
}
.filter-panel-title {
  font-size: 0.8rem;
  font-weight: 600;
}
.filter-clear-btn {
  background: none;
  border: none;
  color: #ef4444;
  font-size: 0.7rem;
  cursor: pointer;
  padding: 0;
}
.filter-clear-btn:hover {
  text-decoration: underline;
}
.filter-section {
  margin-bottom: 0.5rem;
}
.filter-section:last-child {
  margin-bottom: 0;
}
.filter-section-label {
  font-size: 0.65rem;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  color: rgba(255,255,255,0.4);
  margin-bottom: 0.25rem;
}
.filter-options {
  display: flex;
  gap: 0.3rem;
}
.filter-opt {
  flex: 1;
  background: rgba(255,255,255,0.05);
  color: var(--bs-body-color, #c9d1d9);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 0.3rem;
  padding: 0.25rem 0.5rem;
  font-size: 0.75rem;
  cursor: pointer;
  transition: all 0.15s;
  text-align: center;
}
.filter-opt:hover {
  border-color: #555;
  background: rgba(255,255,255,0.08);
}
.filter-opt.active {
  background: #2563eb;
  border-color: #2563eb;
  color: #fff;
}
.filter-duration-row {
  display: flex;
  align-items: center;
  gap: 0.5rem;
}
.duration-slider {
  flex: 1;
  accent-color: #2563eb;
  height: 4px;
}
.duration-value {
  font-size: 0.75rem;
  color: rgba(255,255,255,0.6);
  min-width: 2rem;
  text-align: right;
}
.filter-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 0.25rem;
}
.tag-chip {
  display: inline-flex;
  align-items: center;
  gap: 0.2rem;
  background: rgba(255,255,255,0.05);
  color: var(--bs-body-color, #c9d1d9);
  border: 1px solid var(--bs-border-color, #333);
  border-radius: 1rem;
  padding: 0.15rem 0.5rem;
  font-size: 0.7rem;
  cursor: pointer;
  transition: all 0.15s;
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
  font-size: 0.8rem;
}
</style>
