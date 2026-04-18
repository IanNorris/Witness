import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { api } from '../composables/useApi'
import { useCameraStore } from './cameras'
import type { Group, GroupEnumResponse } from '../types/group'

export const useGroupStore = defineStore('groups', () => {
  const groups = ref<Group[]>([])
  const isLoading = ref(false)

  async function fetchGroups() {
    isLoading.value = true
    try {
      const data = await api<GroupEnumResponse>('/group/enum')
      groups.value = data.groups ?? []
    } catch (err) {
      console.error('Failed to fetch groups:', err)
    } finally {
      isLoading.value = false
    }
  }

  function getGroupById(id: number): Group | undefined {
    return groups.value.find(g => g.id === id)
  }

  /** Groups that have at least one camera assigned */
  const activeGroups = computed(() => {
    const cameraStore = useCameraStore()
    const groupIdsWithCameras = new Set<number>()
    for (const cam of cameraStore.cameras) {
      for (const gid of cam.groups) {
        groupIdsWithCameras.add(gid)
      }
    }
    return groups.value.filter(g => groupIdsWithCameras.has(g.id))
  })

  /** Get cameras belonging to a specific group */
  function camerasInGroup(groupId: number) {
    const cameraStore = useCameraStore()
    return cameraStore.cameras.filter(c => c.groups.includes(groupId))
  }

  return {
    groups,
    isLoading,
    fetchGroups,
    getGroupById,
    activeGroups,
    camerasInGroup,
  }
})
