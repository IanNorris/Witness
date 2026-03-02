<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useTagStore } from '../../stores/tags'
import type { Tag } from '../../types/clip'

const tagStore = useTagStore()

const editingTag = ref<Tag | null>(null)
const editDisplay = ref('')
const editIcon = ref('')
const editHidden = ref(false)
const saving = ref(false)

const sortedTags = computed(() =>
  [...tagStore.tags].sort((a, b) => a.sortOrder - b.sortOrder || a.display.localeCompare(b.display))
)

function startEdit(tag: Tag) {
  editingTag.value = tag
  editDisplay.value = tag.display
  editIcon.value = tag.icon
  editHidden.value = tag.hidden
}

function cancelEdit() {
  editingTag.value = null
}

async function saveEdit() {
  if (!editingTag.value) return
  saving.value = true
  try {
    await tagStore.updateTag(editingTag.value.id, editDisplay.value, editIcon.value, editHidden.value)
    editingTag.value = null
  } finally {
    saving.value = false
  }
}

onMounted(() => {
  tagStore.fetchTags()
})
</script>

<template>
  <div>
    <h6 class="mb-3">Tag Management</h6>
    <p class="text-muted-custom small mb-3">
      Customize how tags appear in the UI. Hidden tags won't show in clip filters.
    </p>

    <div v-if="tagStore.loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <table v-else class="table table-sm table-dark table-hover align-middle">
      <thead>
        <tr>
          <th style="width: 3rem">Icon</th>
          <th>Name</th>
          <th>Display</th>
          <th style="width: 5rem" class="text-center">Clips</th>
          <th style="width: 5rem" class="text-center">Visible</th>
          <th style="width: 5rem"></th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="tag in sortedTags" :key="tag.id" :class="{ 'text-muted': tag.hidden }">
          <template v-if="editingTag?.id === tag.id">
            <td>
              <input
                v-model="editIcon"
                class="form-control form-control-sm tag-icon-input"
                placeholder="emoji"
              />
            </td>
            <td class="small text-muted-custom">{{ tag.name }}</td>
            <td>
              <input
                v-model="editDisplay"
                class="form-control form-control-sm"
              />
            </td>
            <td class="text-center small">{{ tag.clipCount }}</td>
            <td class="text-center">
              <input type="checkbox" class="form-check-input" v-model="editHidden"
                :true-value="false" :false-value="true" />
            </td>
            <td class="text-end">
              <button class="btn btn-sm btn-primary me-1" :disabled="saving" @click="saveEdit">
                {{ saving ? '...' : '✓' }}
              </button>
              <button class="btn btn-sm btn-outline-secondary" @click="cancelEdit">✕</button>
            </td>
          </template>
          <template v-else>
            <td class="tag-icon-cell">{{ tag.icon }}</td>
            <td class="small text-muted-custom">{{ tag.name }}</td>
            <td>{{ tag.display }}</td>
            <td class="text-center small">{{ tag.clipCount }}</td>
            <td class="text-center">
              <input type="checkbox" class="form-check-input"
                :checked="!tag.hidden"
                @change="tagStore.updateTag(tag.id, tag.display, tag.icon, !tag.hidden)" />
            </td>
            <td class="text-end">
              <button class="btn btn-sm btn-outline-secondary" @click="startEdit(tag)">Edit</button>
            </td>
          </template>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<style scoped>
.tag-icon-cell {
  font-size: 1.2rem;
  text-align: center;
}
.tag-icon-input {
  width: 3rem;
  text-align: center;
  font-size: 1rem;
}
</style>
