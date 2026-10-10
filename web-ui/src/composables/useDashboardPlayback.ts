import { computed, reactive, ref, onUnmounted, watch, type InjectionKey, type Ref } from 'vue'
import { useSettingsStore } from '../stores/settings'
import { createPlaybackPosition, returnToLive, seekPosition, inactivitySeconds, shouldReturnToLive, type PlaybackPosition } from '../utils/dashboardPlayback'

export function useDashboardPlayback(cameraIds: Ref<number[]>, held: Ref<boolean>, save: (id: number, timestamp: number) => void) {
  const settings = useSettingsStore()
  // Playback mode and Sync deliberately never persist: every visit starts live.
  const sync = ref(false), selected = ref<number | null>(null)
  const positions = reactive<Record<number, PlaybackPosition>>({})
  const holds = reactive(new Set<number>())
  let sharedTime: number | null = null, sharedWall = Date.now(), sharedPaused = false
  let sharedInteraction = Date.now()
  const get = (id: number) => positions[id] ??= createPlaybackPosition(Date.now())
  const anyReplay = computed(() => cameraIds.value.some(id => get(id).requestedAt !== null))
  function touch(id: number) {
    get(id).lastInteraction = sharedInteraction = Date.now()
  }
  function hold(id: number, value: boolean) {
    if (value) holds.add(id); else holds.delete(id)
    touch(id)
  }
  function live(id: number) {
    const now = Date.now()
    for (const target of sync.value ? cameraIds.value : [id]) returnToLive(get(target), now)
    if (sync.value) { sharedTime = null; sharedPaused = false }
    sharedInteraction = now
  }
  function seek(id: number, timestamp: number) {
    if (!Number.isFinite(timestamp)) return
    selected.value = id
    const now = Date.now(), target = Math.min(timestamp, now / 1000)
    if (target >= now / 1000 - 1) { live(id); return }
    for (const camera of sync.value ? cameraIds.value : [id]) seekPosition(get(camera), target, now)
    if (sync.value) { sharedTime = target; sharedWall = now; sharedPaused = get(id).paused }
    touch(id)
  }
  function setSync(value: boolean) {
    if (value === sync.value) return
    const id = selected.value !== null && cameraIds.value.includes(selected.value) ? selected.value : cameraIds.value[0]
    sync.value = value
    if (value && id !== undefined) {
      const source = get(id)
      const target = source.viewedAt, paused = source.paused
      if (target === null) live(id)
      else {
        for (const camera of cameraIds.value) get(camera).paused = paused
        seek(id, target)
      }
    }
    if (!value) {
      // Freeze the current shared position before independent clocks take over.
      for (const id of cameraIds.value) {
        const position = get(id)
        if (position.viewedAt !== null) seekPosition(position, position.viewedAt, Date.now())
      }
    }
  }
  function pause(id: number) {
    const source = get(id)
    if (source.requestedAt === null) return
    const paused = !source.paused
    for (const target of sync.value ? cameraIds.value : [id]) get(target).paused = paused
    if (sync.value) { sharedTime = source.viewedAt; sharedWall = Date.now(); sharedPaused = paused }
    touch(id)
  }
  function observed(id: number, timestamp: number) {
    if (!sync.value && get(id).requestedAt !== null && Number.isFinite(timestamp)) get(id).viewedAt = timestamp
  }
  watch(cameraIds, ids => {
    for (const id of Object.keys(positions).map(Number)) if (!ids.includes(id)) { delete positions[id]; holds.delete(id) }
    if (selected.value !== null && !ids.includes(selected.value)) selected.value = null
    for (const id of ids) {
      if (!positions[id] && sync.value && sharedTime !== null) {
        const position = get(id)
        position.paused = sharedPaused
        seekPosition(position, sharedTime + (sharedPaused ? 0 : (Date.now() - sharedWall) / 1000), Date.now())
      } else get(id)
    }
  }, { immediate: true })
  watch(held, () => {
    for (const id of cameraIds.value) touch(id)
  })
  const timer = setInterval(() => {
    const now = Date.now(), timeout = inactivitySeconds(settings.dashboardReturnLiveSeconds)
    if (sync.value && sharedTime !== null) {
      const timestamp = sharedTime + (sharedPaused ? 0 : (now - sharedWall) / 1000)
      if (timestamp >= now / 1000 - 1) { const id = cameraIds.value[0]; if (id !== undefined) live(id); return }
      for (const id of cameraIds.value) get(id).viewedAt = timestamp
      if (timeout > 0 && !held.value && !holds.size && now - sharedInteraction >= timeout * 1000) {
        const id = cameraIds.value[0]; if (id !== undefined) live(id)
      }
    } else {
      for (const id of cameraIds.value) if (shouldReturnToLive(get(id), now, timeout, held.value || holds.has(id))) live(id)
    }
  }, 500)
  onUnmounted(() => clearInterval(timer))
  return { sync, selected, positions, get, anyReplay, seek, live, pause, touch, hold, isHeld: (id: number) => holds.has(id), setSync, observed, save }
}

export type DashboardPlayback = ReturnType<typeof useDashboardPlayback>
export const dashboardPlaybackKey: InjectionKey<DashboardPlayback> = Symbol('dashboard-playback')
