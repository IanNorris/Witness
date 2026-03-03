import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { api } from '../composables/useApi'
import type { Tag } from '../types/clip'

export interface TagGroup {
  display: string
  icon: string
  names: string[]  // all raw tag names in this group
  clipCount: number
  hidden: boolean
}

export const useTagStore = defineStore('tags', () => {
  const tags = ref<Tag[]>([])
  const loading = ref(false)

  async function fetchTags() {
    loading.value = true
    try {
      const data = await api<{ tags: Record<string, unknown>[] }>('/clip/tags')
      tags.value = (data.tags ?? []).map(raw => ({
        id: raw.id as number,
        name: raw.name as string,
        display: raw.display as string,
        icon: raw.icon as string,
        sortOrder: raw.sortOrder as number,
        hidden: (raw.hidden as number) === 1,
        clipCount: raw.clipCount as number,
      }))
    } catch {
      tags.value = []
    } finally {
      loading.value = false
    }
  }

  // Group tags by display name for de-duplication in filters
  const groupedTags = computed<TagGroup[]>(() => {
    const groups = new Map<string, TagGroup>()
    for (const t of tags.value) {
      const key = t.display.toLowerCase()
      const existing = groups.get(key)
      if (existing) {
        existing.names.push(t.name)
        existing.clipCount += t.clipCount
        existing.hidden = existing.hidden && t.hidden
      } else {
        groups.set(key, {
          display: t.display,
          icon: t.icon,
          names: [t.name],
          clipCount: t.clipCount,
          hidden: t.hidden,
        })
      }
    }
    return Array.from(groups.values())
  })

  async function updateTag(id: number, display: string, icon: string, hidden: boolean) {
    await api('/clip/tags/update', {
      method: 'POST',
      body: { id, display, icon, hidden: hidden ? 1 : 0 },
    })
    const tag = tags.value.find(t => t.id === id)
    if (tag) {
      tag.display = display
      tag.icon = icon
      tag.hidden = hidden
    }
  }

  function getTagDisplay(name: string): { display: string; icon: string } {
    const tag = tags.value.find(t => t.name === name)
    return tag ? { display: tag.display, icon: tag.icon } : { display: name, icon: '' }
  }

  // Get de-duped display tags for a clip's raw tag string
  function getDisplayTags(tagString: string): { display: string; icon: string }[] {
    if (!tagString) return []
    const rawTags = tagString.split(/[;,]/).map(t => t.trim()).filter(Boolean)
    const seen = new Set<string>()
    const result: { display: string; icon: string }[] = []
    for (const raw of rawTags) {
      const info = getTagDisplay(raw)
      const key = info.display.toLowerCase()
      if (!seen.has(key)) {
        seen.add(key)
        result.push(info)
      }
    }
    return result
  }

  return { tags, loading, groupedTags, fetchTags, updateTag, getTagDisplay, getDisplayTags }
})
