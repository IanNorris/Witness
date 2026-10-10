<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import ClipFilters from './ClipFilters.vue'
import ClipCard from './ClipCard.vue'
import ClipPlayer from './ClipPlayer.vue'
import { useCameraStore } from '../../stores/cameras'
import { useClipStore } from '../../stores/clips'
import { useFilterStore } from '../../stores/filters'
import { useTagStore } from '../../stores/tags'
import { buildActivityGroups, type ActivityGroup } from '../../composables/useActivityGrouping'
import type { Clip } from '../../types/clip'

const props = defineProps<{ groupId?: number | null; cameraIds?: Set<number> | null }>()
const emit = defineEmits<{ close: [] }>()
const cameras = useCameraStore(), clips = useClipStore(), filters = useFilterStore(), tags = useTagStore()
const selectedCamera = ref<number | null>(null)
const tagQuery = ref(filters.activeFilters.tags.join(', '))
const initialTimeRange = filters.timeRange ? {...filters.timeRange} : null
const period = ref(filters.timeRange ? 'current' : 'all')
const grouping = ref(localStorage.getItem('witness-dashboard-search-grouping') !== '0')
const expanded = ref(new Set<string>())
const groups = ref<ActivityGroup[]>([])
const groupingPending = ref(false)
const playing = ref<Clip | null>(null)
const modal = ref<HTMLElement | null>(null)
const matchingCameras = computed(() => cameras.cameras.filter(c => !props.cameraIds || props.cameraIds.has(c.id)))
let previousFocus: HTMLElement | null = null
let generation = 0
let loadTimer: ReturnType<typeof setTimeout> | undefined

function reload() { return clips.fetchClips(selectedCamera.value, 0, false, props.groupId ?? null) }
function applyTags() {
  const next = tagQuery.value.split(',').map(t => t.trim()).filter(Boolean)
  if (next.join(',') === filters.activeFilters.tags.join(',')) reload()
  else filters.activeFilters.tags = next
}
function setPeriod() {
  const now = Math.floor(Date.now() / 1000)
  if (period.value === 'current') {
    if (initialTimeRange) filters.setTimeRange(initialTimeRange.from, initialTimeRange.to)
    return
  }
  if (period.value === 'all') {
    filters.clearFilter('timeFrom'); filters.clearFilter('timeTo')
  } else filters.setTimeRange(now - Number(period.value), now)
}
function filterTag(display: string) {
  const group = tags.groupedTags.find(g => g.display === display)
  filters.activeFilters.tags = group?.names ?? [display]
}
function toggleGroup(id: string) {
  const updated = new Set(expanded.value)
  if (updated.has(id)) updated.delete(id); else updated.add(id)
  expanded.value = updated
}
watch(() => filters.activeFilters.tags.join(', '), value => { tagQuery.value = value })
watch(() => filters.timeRange, range => { if (!range) period.value = 'all' })
watch([selectedCamera, () => props.groupId, () => filters.filterQueryString, () => filters.timeRange], () => {
  clearTimeout(loadTimer)
  loadTimer = setTimeout(reload, 200)
})
watch(grouping, value => {
  localStorage.setItem('witness-dashboard-search-grouping', value ? '1' : '0')
  expanded.value = new Set()
})
watch([() => clips.clips, grouping], async ([results, enabled]) => {
  const current = ++generation
  groupingPending.value = true
  const singles = () => results.map(clip => ({id: `clip-${clip.uid}`, representative: clip, clips: [clip], maximumDistance: 0}))
  groups.value = singles()
  try {
    const next = enabled ? await buildActivityGroups(results, clip => clip.recordMode === 'Capture' ? `/clip/captured/${clip.uid}/thumbnail` : clips.thumbnailUrl(clip.camera, clip.timestamp)) : singles()
    if (current === generation) groups.value = next
  } catch {
    if (current === generation) groups.value = singles()
  } finally { if (current === generation) groupingPending.value = false }
}, {immediate: true})
function keydown(event: KeyboardEvent) {
  event.stopPropagation()
  if (event.key === 'Escape') { event.preventDefault(); emit('close'); return }
  if (event.key !== 'Tab' || !modal.value) return
  const targets = [...modal.value.querySelectorAll<HTMLElement>('button:not(:disabled), input, select, [tabindex="0"]')]
  const first = targets[0], last = targets[targets.length - 1]
  if (event.shiftKey && (document.activeElement === first || document.activeElement === modal.value)) {event.preventDefault(); last?.focus()}
  else if (!event.shiftKey && (document.activeElement === last || document.activeElement === modal.value)) {event.preventDefault(); first?.focus()}
}
onMounted(() => {
  previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
  modal.value?.focus()
  reload()
})
onUnmounted(() => {++generation; clearTimeout(loadTimer); if (previousFocus?.isConnected) previousFocus.focus()})
</script>

<template>
  <Teleport to="body">
    <div ref="modal" class="dashboard-search-backdrop" role="dialog" aria-modal="true" aria-label="Search dashboard clips"
      tabindex="0" @keydown="keydown" @click.self="emit('close')">
      <section class="dashboard-search-panel">
        <header><div><span class="search-eyebrow">REVIEW WITHOUT LEAVING LIVE</span><h2>Find a clip</h2></div><button class="btn btn-sm btn-primary" @click="emit('close')">Back to dashboard ×</button></header>
        <div class="dashboard-search-tools">
          <label>Camera<select v-model="selectedCamera" class="form-select form-select-sm"><option :value="null">All cameras in this dashboard group</option><option v-for="camera in matchingCameras" :key="camera.id" :value="camera.id">{{ camera.name }}</option></select></label>
          <label>When<select v-model="period" class="form-select form-select-sm" @change="setPeriod"><option v-if="initialTimeRange" value="current">Original selected range</option><option value="all">All history</option><option value="3600">Last hour</option><option value="86400">Last 24 hours</option><option value="604800">Last 7 days</option></select></label>
          <form @submit.prevent="applyTags"><label>Tags (any match)<input v-model="tagQuery" class="form-control form-control-sm" placeholder="person, speech, vehicle" /></label><button class="btn btn-sm btn-outline-primary">Search</button></form>
          <label class="search-group-toggle"><input v-model="grouping" type="checkbox" /> Group similar activity</label>
        </div>
        <div class="dashboard-search-content">
          <aside><ClipFilters /><p class="search-note">Filters are shared with the Clips page. Groups apply to this results page only; no clips are merged or hidden permanently.</p></aside>
          <div class="dashboard-search-results" aria-live="polite">
            <p v-if="clips.loading">Searching…</p><p v-else-if="clips.loadError" role="alert">{{ clips.loadError }} <button class="btn btn-sm btn-outline-secondary" @click="reload">Retry</button></p>
            <template v-else>
              <p>{{ clips.totalCount.toLocaleString() }} matching clips · page {{ clips.totalPages ? clips.currentPage + 1 : 0 }} of {{ clips.totalPages }}<span v-if="groupingPending"> · comparing thumbnails…</span></p>
              <p v-if="!clips.clips.length">No clips match these filters.</p>
              <div class="search-clip-grid">
                <section v-for="group in groups" :key="group.id" class="search-clip-group">
                  <ClipCard :clip="group.representative" read-only @play="playing = $event" @tag-click="filterTag" />
                  <button v-if="group.clips.length > 1" class="btn btn-sm btn-outline-secondary w-100 mt-1" :aria-expanded="expanded.has(group.id)" @click="toggleGroup(group.id)">{{ expanded.has(group.id) ? 'Collapse' : 'Show' }} {{ group.clips.length }} original clips</button>
                  <div v-if="expanded.has(group.id)" class="search-group-originals"><ClipCard v-for="clip in group.clips.filter(c => c.uid !== group.representative.uid)" :key="clip.uid" :clip="clip" read-only @play="playing = $event" @tag-click="filterTag" /></div>
                </section>
              </div>
              <nav class="search-pages" aria-label="Clip search pages"><button class="btn btn-sm btn-outline-secondary" :disabled="clips.currentPage <= 0" @click="clips.prevPage">Previous</button><button class="btn btn-sm btn-outline-secondary" :disabled="clips.currentPage >= clips.totalPages - 1" @click="clips.nextPage">Next</button></nav>
            </template>
          </div>
        </div>
      </section>
    </div>
    <ClipPlayer v-if="playing" :clip="playing" @close="playing = null" />
  </Teleport>
</template>

<style scoped>
.dashboard-search-backdrop {position: fixed; inset: 0; z-index: 2200; background: #07101bd9; backdrop-filter: blur(8px); display: grid; place-items: center; padding: 20px; overflow: auto;}
.dashboard-search-panel {width: min(1300px, 100%); max-height: calc(100dvh - 40px); overflow: auto; color: #e7eff8; background: #111d2b; border: 1px solid #47617a; border-radius: 16px; box-shadow: 0 24px 80px #0008;}
header {display: flex; justify-content: space-between; align-items: center; gap: 16px; padding: 18px 20px; border-bottom: 1px solid #34485d;}
h2 {font-size: 1.3rem; margin: 3px 0 0;}.search-eyebrow {font-size: .65rem; letter-spacing: .14em; color: #8bb9dd;}
.dashboard-search-tools {display: flex; align-items: end; flex-wrap: wrap; gap: 12px; padding: 16px 20px;}
label {font-size: .8rem;}form {display: flex; align-items: end; gap: 8px;}.search-group-toggle {padding-bottom: 6px;}
.dashboard-search-content {display: grid; grid-template-columns: 220px minmax(0, 1fr); gap: 18px; padding: 0 20px 20px;}
.search-note {font-size: .75rem; color: #b1c3d4; margin-top: 12px;}.search-clip-grid {display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 14px;}
.search-group-originals {display: grid; gap: 8px; padding: 8px; border-left: 2px solid #67b5ff; margin-top: 8px;}.search-pages {display: flex; justify-content: center; gap: 12px; margin-top: 20px;}
@media(max-width: 700px) {.dashboard-search-content {grid-template-columns: 1fr;}.dashboard-search-backdrop {padding: 8px;}.dashboard-search-panel {max-height: calc(100dvh - 16px);}header {flex-wrap: wrap;}}
</style>
