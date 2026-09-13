import { computed, onUnmounted, ref, watch } from 'vue'

export type LiveAudioMode = 'off' | 'on' | 'motion'

const MOTION_AUDIO_HOLD_MS = 5000

export function useLiveAudio(cameraId: () => number, motionActive: () => boolean) {
  const mode = ref<LiveAudioMode>('off')
  const motionHeld = ref(false)
  let holdTimer: ReturnType<typeof setTimeout> | null = null

  function loadPreference() {
    if (holdTimer) {
      clearTimeout(holdTimer)
      holdTimer = null
    }
    motionHeld.value = motionActive()
    const saved = localStorage.getItem(`witness-live-audio-${cameraId()}`)
    mode.value = saved === 'motion' ? 'motion' : saved === '1' ? 'on' : 'off'
  }

  function savePreference(next: LiveAudioMode) {
    mode.value = next
    localStorage.setItem(
      `witness-live-audio-${cameraId()}`,
      next === 'on' ? '1' : next === 'motion' ? 'motion' : '0',
    )
  }

  function toggleAudio() {
    savePreference(mode.value === 'on' ? 'off' : 'on')
  }

  function toggleMotionAudio() {
    savePreference(mode.value === 'motion' ? 'off' : 'motion')
  }

  watch(cameraId, loadPreference, { immediate: true })
  watch(motionActive, active => {
    if (holdTimer) {
      clearTimeout(holdTimer)
      holdTimer = null
    }
    if (active) {
      motionHeld.value = true
    } else if (motionHeld.value) {
      holdTimer = setTimeout(() => {
        motionHeld.value = false
        holdTimer = null
      }, MOTION_AUDIO_HOLD_MS)
    }
  }, { immediate: true })

  const enabled = computed(() =>
    mode.value === 'on' || (mode.value === 'motion' && motionHeld.value)
  )

  onUnmounted(() => {
    if (holdTimer) clearTimeout(holdTimer)
  })

  return { mode, enabled, toggleAudio, toggleMotionAudio }
}
