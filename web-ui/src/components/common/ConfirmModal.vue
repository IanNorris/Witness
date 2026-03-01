<script setup lang="ts">
import { ref, watch, nextTick } from 'vue'

const props = defineProps<{
  show: boolean
  title?: string
  message: string
  confirmText?: string
  confirmClass?: string
}>()

const emit = defineEmits<{
  confirm: []
  cancel: []
}>()

const modalRef = ref<HTMLElement | null>(null)

watch(() => props.show, async (v) => {
  if (v) {
    await nextTick()
    modalRef.value?.focus()
  }
})

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') emit('cancel')
  if (e.key === 'Enter') emit('confirm')
}
</script>

<template>
  <Teleport to="body">
    <div v-if="show" class="modal-backdrop show" @click="emit('cancel')" />
    <div v-if="show" class="modal d-block" tabindex="-1" @keydown="onKeydown" ref="modalRef">
      <div class="modal-dialog modal-dialog-centered modal-sm" @click.stop>
        <div class="modal-content bg-dark text-light">
          <div class="modal-header border-secondary py-2">
            <h6 class="modal-title">{{ title ?? 'Confirm' }}</h6>
            <button type="button" class="btn-close btn-close-white" @click="emit('cancel')" />
          </div>
          <div class="modal-body py-3">
            <p class="mb-0">{{ message }}</p>
          </div>
          <div class="modal-footer border-secondary py-2">
            <button class="btn btn-sm btn-secondary" @click="emit('cancel')">Cancel</button>
            <button class="btn btn-sm" :class="confirmClass ?? 'btn-danger'" @click="emit('confirm')">
              {{ confirmText ?? 'Confirm' }}
            </button>
          </div>
        </div>
      </div>
    </div>
  </Teleport>
</template>
