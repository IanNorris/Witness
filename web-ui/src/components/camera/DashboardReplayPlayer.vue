<script setup lang="ts">
import { computed, inject, onMounted, onUnmounted, ref, watch } from 'vue'
import DvrPlayer from '../clips/DvrPlayer.vue'
import { dashboardPlaybackKey } from '../../composables/useDashboardPlayback'
const props = defineProps<{ cameraId: number; cameraName: string; audioEnabled: boolean }>()
const playback = inject(dashboardPlaybackKey)!
const position = computed(() => playback.get(props.cameraId))
const player = ref<InstanceType<typeof DvrPlayer> | null>(null)
const startAt = ref(position.value.requestedAt!)
const from = ref(Math.floor(position.value.requestedAt! - 300)), to = ref(Math.floor(Date.now() / 1000))
let timer: ReturnType<typeof setInterval> | undefined
watch(() => position.value.revision, () => {
  const target = position.value.requestedAt
  if (target === null) return
  if (target === startAt.value) player.value?.seekToTimestamp(target)
  startAt.value = target
  if (target < from.value || target >= to.value) { from.value = Math.floor(target - 300); to.value = Math.floor(Date.now() / 1000) }
}, { flush: 'post' })
watch(() => position.value.paused, paused => player.value?.setPlaying(!paused))
onMounted(() => {
  // Reconcile against the shared absolute clock, not against another player's
  // timeupdate cadence. Gaps stay gaps and are retried as that clock progresses.
  timer = setInterval(() => {
    if (!playback.sync.value || position.value.viewedAt === null || !player.value || player.value.loading || player.value.isSwapping()) return
    const target = position.value.viewedAt
    if (target < from.value || target >= to.value) return
    if (player.value.error || Math.abs(player.value.currentTimestamp - target) > 1.5) {
      player.value.seekToTimestamp(target)
      if (!player.value.error) player.value.setPlaying(!position.value.paused)
    }
  }, 2000)
})
onUnmounted(() => clearInterval(timer))
defineExpose({ toggleOverlay: () => player.value?.toggleDetectionWithSave(), overlayEnabled: computed(() => player.value?.overlayEnabled ?? false), streamInfo: computed(() => player.value?.streamInfo ?? { tier: 'dvr' }) })
</script>
<template>
  <DvrPlayer ref="player" :camera-id="cameraId" :camera-name="cameraName" :from="from" :to="to"
    :start-at="startAt" compact embedded :initial-paused="position.paused" :muted="!audioEnabled"
    @time="playback.observed(cameraId, $event)" @live="playback.live(cameraId)" />
</template>
