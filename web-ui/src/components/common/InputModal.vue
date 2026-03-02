<script setup lang="ts">
import { ref, watch, nextTick, computed } from 'vue'

const props = defineProps<{
  show: boolean
  title?: string
  label: string
  modelValue?: string
  submitText?: string
  inputType?: string
  confirm?: boolean
}>()

const emit = defineEmits<{
  submit: [value: string]
  cancel: []
}>()

const inputVal = ref('')
const confirmVal = ref('')
const inputRef = ref<HTMLInputElement | null>(null)

const mismatch = computed(() => props.confirm && inputVal.value !== confirmVal.value)

watch(() => props.show, async (v) => {
  if (v) {
    inputVal.value = props.modelValue ?? ''
    confirmVal.value = ''
    await nextTick()
    inputRef.value?.focus()
    inputRef.value?.select()
  }
})

function onSubmit() {
  if (mismatch.value) return
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
                :type="inputType ?? 'text'"
                class="form-control form-control-sm"
                required
              />
              <div v-if="confirm" class="mt-2">
                <label class="form-label small">Confirm {{ label.toLowerCase() }}</label>
                <input
                  v-model="confirmVal"
                  :type="inputType ?? 'text'"
                  class="form-control form-control-sm"
                  :class="{ 'is-invalid': confirmVal && mismatch }"
                  required
                />
                <div v-if="confirmVal && mismatch" class="invalid-feedback">
                  Values do not match
                </div>
              </div>
            </div>
            <div class="modal-footer border-secondary py-2">
              <button type="button" class="btn btn-sm btn-secondary" @click="emit('cancel')">Cancel</button>
              <button type="submit" class="btn btn-sm btn-primary" :disabled="!!(confirm && mismatch)">
                {{ submitText ?? 'OK' }}
              </button>
            </div>
          </form>
        </div>
      </div>
    </div>
  </Teleport>
</template>
