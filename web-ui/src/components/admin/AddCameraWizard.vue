<script setup lang="ts">
import { ref, computed, watch, nextTick } from 'vue'

const props = defineProps<{
  show: boolean
}>()

const emit = defineEmits<{
  create: [data: { displayName: string; connectionString: string; connectionStringSub: string; description: string }]
  cancel: []
}>()

interface Brand {
  id: number
  name: string
  template: string
  subTemplate: string
  fields: string[]
}

const brands: Brand[] = [
  {
    id: 0, name: 'Manual',
    template: '{StreamPath}',
    subTemplate: '{SubStreamPath}',
    fields: ['StreamPath', 'SubStreamPath'],
  },
  {
    id: 1, name: 'Hikvision',
    template: 'rtsp://{Username}:{Password}@{Hostname}:554/Streaming/Channels/101?transportmode=unicast&profile=Profile_1',
    subTemplate: 'rtsp://{Username}:{Password}@{Hostname}:554/Streaming/Channels/102?transportmode=unicast&profile=Profile_1',
    fields: ['Hostname', 'Username', 'Password'],
  },
  {
    id: 2, name: 'Tapo',
    template: 'rtsp://{Username}:{Password}@{Hostname}:554/stream1',
    subTemplate: 'rtsp://{Username}:{Password}@{Hostname}:554/stream2',
    fields: ['Hostname', 'Username', 'Password'],
  },
  {
    id: 3, name: 'Generic RTSP',
    template: 'rtsp://{Username}:{Password}@{Hostname}:554/{Path}',
    subTemplate: '',
    fields: ['Hostname', 'Username', 'Password', 'Path'],
  },
]

const step = ref(1)
const name = ref('')
const description = ref('')
const brandId = ref(0)
const fieldValues = ref<Record<string, string>>({})
const nameRef = ref<HTMLInputElement | null>(null)

const brand = computed(() => brands.find(b => b.id === brandId.value)!)

const totalSteps = computed(() => {
  if (brandId.value === 0) return 3 // Basics → Stream URLs → Confirm
  return 4 // Basics → Brand/Host → Auth → Confirm
})

const connectionString = computed(() => {
  let s = brand.value.template
  for (const field of brand.value.fields) {
    s = s.split(`{${field}}`).join(fieldValues.value[field] ?? '')
  }
  return s
})

const connectionStringSub = computed(() => {
  let s = brand.value.subTemplate
  for (const field of brand.value.fields) {
    s = s.split(`{${field}}`).join(fieldValues.value[field] ?? '')
  }
  return s
})

const canNext = computed(() => {
  if (step.value === 1) return !!name.value.trim()
  if (step.value === totalSteps.value - 1) {
    // All required fields filled
    return brand.value.fields.every(f => {
      if (f === 'SubStreamPath' || f === 'Path') return true
      return !!fieldValues.value[f]?.trim()
    })
  }
  return true
})

watch(() => props.show, async (v) => {
  if (v) {
    step.value = 1
    name.value = ''
    description.value = ''
    brandId.value = 0
    fieldValues.value = {}
    await nextTick()
    nameRef.value?.focus()
  }
})

watch(brandId, () => {
  fieldValues.value = {}
})

function next() {
  if (step.value < totalSteps.value) step.value++
}

function back() {
  if (step.value > 1) step.value--
}

function finish() {
  emit('create', {
    displayName: name.value.trim(),
    connectionString: connectionString.value,
    connectionStringSub: connectionStringSub.value,
    description: description.value.trim(),
  })
}

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') emit('cancel')
}

function fieldLabel(field: string): string {
  const labels: Record<string, string> = {
    Hostname: 'IP Address / Hostname',
    Username: 'Username',
    Password: 'Password',
    StreamPath: 'Main Stream RTSP URL',
    SubStreamPath: 'Sub Stream RTSP URL (optional)',
    Path: 'Stream Path',
  }
  return labels[field] ?? field
}

function fieldPlaceholder(field: string): string {
  const placeholders: Record<string, string> = {
    Hostname: '192.168.1.100',
    Username: 'admin',
    Password: '',
    StreamPath: 'rtsp://user:pass@192.168.1.100/stream1',
    SubStreamPath: 'rtsp://user:pass@192.168.1.100/stream2',
    Path: 'stream1',
  }
  return placeholders[field] ?? ''
}

function isPasswordField(field: string): boolean {
  return field === 'Password'
}
</script>

<template>
  <Teleport to="body">
    <div v-if="show" class="modal-backdrop show" @click="emit('cancel')" />
    <div v-if="show" class="modal d-block" tabindex="-1" @keydown="onKeydown">
      <div class="modal-dialog modal-dialog-centered" @click.stop>
        <div class="modal-content bg-dark text-light">
          <div class="modal-header border-secondary py-2">
            <h6 class="modal-title">Add Camera — Step {{ step }} of {{ totalSteps }}</h6>
            <button type="button" class="btn-close btn-close-white" @click="emit('cancel')" />
          </div>
          <div class="modal-body py-3">
            <!-- Step 1: Basics -->
            <div v-if="step === 1">
              <div class="mb-3">
                <label class="form-label small">Camera Name</label>
                <input ref="nameRef" v-model="name" class="form-control form-control-sm" required placeholder="Front Door" />
              </div>
              <div class="mb-3">
                <label class="form-label small">Description (optional)</label>
                <input v-model="description" class="form-control form-control-sm" placeholder="Optional description" />
              </div>
              <div>
                <label class="form-label small">Camera Brand</label>
                <select v-model="brandId" class="form-select form-select-sm">
                  <option v-for="b in brands" :key="b.id" :value="b.id">{{ b.name }}</option>
                </select>
              </div>
            </div>

            <!-- Middle steps: brand-specific fields -->
            <div v-else-if="step < totalSteps">
              <div v-for="field in brand.fields" :key="field" class="mb-3">
                <label class="form-label small">{{ fieldLabel(field) }}</label>
                <input
                  v-model="fieldValues[field]"
                  :type="isPasswordField(field) ? 'password' : 'text'"
                  class="form-control form-control-sm"
                  :class="{ 'font-monospace': !isPasswordField(field) }"
                  :placeholder="fieldPlaceholder(field)"
                />
              </div>
            </div>

            <!-- Final step: Confirm -->
            <div v-else>
              <p class="small text-muted mb-2">Review and create:</p>
              <table class="table table-dark table-sm small mb-0">
                <tr><td class="text-muted">Name</td><td>{{ name }}</td></tr>
                <tr v-if="description"><td class="text-muted">Description</td><td>{{ description }}</td></tr>
                <tr><td class="text-muted">Brand</td><td>{{ brand.name }}</td></tr>
                <tr><td class="text-muted">Main Stream</td><td class="font-monospace text-break">{{ connectionString }}</td></tr>
                <tr v-if="connectionStringSub"><td class="text-muted">Sub Stream</td><td class="font-monospace text-break">{{ connectionStringSub }}</td></tr>
              </table>
            </div>
          </div>
          <div class="modal-footer border-secondary py-2">
            <button v-if="step > 1" type="button" class="btn btn-sm btn-secondary me-auto" @click="back">← Back</button>
            <button type="button" class="btn btn-sm btn-secondary" @click="emit('cancel')">Cancel</button>
            <button
              v-if="step < totalSteps"
              type="button"
              class="btn btn-sm btn-primary"
              :disabled="!canNext"
              @click="next"
            >Next →</button>
            <button
              v-else
              type="button"
              class="btn btn-sm btn-success"
              :disabled="!canNext"
              @click="finish"
            >Create Camera</button>
          </div>
        </div>
      </div>
    </div>
  </Teleport>
</template>
