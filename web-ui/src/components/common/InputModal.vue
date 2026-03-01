<script setup lang="ts">
import { ref, watch, nextTick } from 'vue'

const props = defineProps<{
  show: boolean
  title?: string
  label: string
  modelValue?: string
  submitText?: string
}>()

const emit = defineEmits<{
  submit: [value: string]
  cancel: []
}>()

const inputVal = ref('')
const inputRef = ref<HTMLInputElement | null>(null)

watch(() => props.show, async (v) => {
  if (v) {
    inputVal.value = props.modelValue ?? ''
    await nextTick()
    inputRef.value?.focus()
    inputRef.value?.select()
  }
})

function onSubmit() {
  emit('submit', inputVal.value)
}

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') emit('cancel')
}
</script>

<template>
  <Teleport to="body">
    <div v-if="show" class="modal-backdrop show" @click="emit('cancel')" />
    <div v-if="show" class="modal d-block" tabindex="-1" @keydown="onKeydown">
      <div class="modal-dialog modal-dialog-centered modal-sm" @click.stop>
        <div class="modal-content bg-dark text-light">
          <div class="modal-header border-secondary py-2">
            <h6 class="modal-title">{{ title ?? 'Input' }}</h6>
            <button type="button" class="btn-close btn-close-white" @click="emit('cancel')" />
          </div>
          <form @submit.prevent="onSubmit">
            <div class="modal-body py-3">
              <label class="form-label small">{{ label }}</label>
              <input
                ref="inputRef"
                v-model="inputVal"
                class="form-control form-control-sm"
                required
              />
            </div>
            <div class="modal-footer border-secondary py-2">
              <button type="button" class="btn btn-sm btn-secondary" @click="emit('cancel')">Cancel</button>
              <button type="submit" class="btn btn-sm btn-primary">
                {{ submitText ?? 'OK' }}
              </button>
            </div>
          </form>
        </div>
      </div>
    </div>
  </Teleport>
</template>
