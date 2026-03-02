import { defineStore } from 'pinia'
import { computed, ref, watch } from 'vue'

export interface ActiveFilters {
  reviewed?: number
  saved?: number
  mode?: number
  lighting?: number
  minDuration?: number
  tags: string[]
}

const STORAGE_KEY = 'clipFilters'

function loadFilters(): ActiveFilters {
  try {
    const stored = localStorage.getItem(STORAGE_KEY)
    if (stored) return { tags: [], ...JSON.parse(stored) }
  } catch { /* ignore */ }
  return { tags: [] }
}

export const useFilterStore = defineStore('filters', () => {
  const activeFilters = ref<ActiveFilters>(loadFilters())
  const activePreset = ref<string | null>(null)

  watch(activeFilters, (val) => {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(val))
  }, { deep: true })

  const filterQueryString = computed(() => {
    const f = activeFilters.value
    const parts: string[] = []
    if (f.reviewed !== undefined) parts.push(`reviewed=${f.reviewed}`)
    if (f.saved !== undefined) parts.push(`saved=${f.saved}`)
    if (f.mode !== undefined) parts.push(`mode=${f.mode}`)
    if (f.lighting !== undefined) parts.push(`lighting=${f.lighting}`)
    if (f.minDuration !== undefined && f.minDuration > 0) parts.push(`minDuration=${f.minDuration}`)
    if (f.tags.length > 0) parts.push(`tags=${encodeURIComponent(f.tags.join(','))}`)
    return parts.length ? '?' + parts.join('&') : ''
  })

  const hasActiveFilters = computed(() => {
    const f = activeFilters.value
    return f.reviewed !== undefined || f.saved !== undefined ||
      f.mode !== undefined || f.lighting !== undefined ||
      (f.minDuration !== undefined && f.minDuration > 0) ||
      f.tags.length > 0
  })

  function setPreset(preset: string) {
    clearFilters()
    activePreset.value = preset
    switch (preset) {
      case 'unreviewed':
        activeFilters.value.reviewed = 0
        break
      case 'saved':
        activeFilters.value.saved = 1
        break
      case 'manual':
        activeFilters.value.mode = 0
        break
      default:
        break
    }
  }

  function clearFilters() {
    activeFilters.value = { tags: [] }
    activePreset.value = null
  }

  function toggleTag(tag: string) {
    const idx = activeFilters.value.tags.indexOf(tag)
    if (idx >= 0) {
      activeFilters.value.tags.splice(idx, 1)
    } else {
      activeFilters.value.tags.push(tag)
    }
    activePreset.value = null
  }

  function setFilter<K extends keyof ActiveFilters>(key: K, value: ActiveFilters[K]) {
    activeFilters.value[key] = value as never
    activePreset.value = null
  }

  function clearFilter(key: keyof ActiveFilters) {
    if (key === 'tags') {
      activeFilters.value.tags = []
    } else {
      delete activeFilters.value[key]
    }
    activePreset.value = null
  }

  return {
    activeFilters,
    activePreset,
    filterQueryString,
    hasActiveFilters,
    setPreset,
    clearFilters,
    toggleTag,
    setFilter,
    clearFilter,
  }
})
