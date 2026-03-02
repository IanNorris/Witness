import { defineStore } from 'pinia'
import { ref } from 'vue'
import { api } from '../composables/useApi'
import type { Tag } from '../types/clip'

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

  return { tags, loading, fetchTags, updateTag, getTagDisplay }
})
