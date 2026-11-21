<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { api } from '../../composables/useApi'
import { useDashboardStore, type SharePrincipal } from '../../stores/dashboards'
import { useGroupStore } from '../../stores/groups'

// Admin-only: who can see a shared dashboard. Sharing never grants camera
// access; the preview shows how many items each recipient will not see.
const props = defineProps<{ dashboardId: number; shares: SharePrincipal[] }>()
const emit = defineEmits<{ close: []; saved: [shares: SharePrincipal[]] }>()
const store = useDashboardStore()
const groupStore = useGroupStore()
const users = ref<{ userid: number; displayName: string; username: string; enabled: number }[]>([])
const everyone = ref(props.shares.some(s => s.type === 'everyone'))
const groups = ref(new Set(props.shares.filter(s => s.type === 'group').map(s => s.id)))
const people = ref(new Set(props.shares.filter(s => s.type === 'user').map(s => s.id)))
const preview = ref<{ userId: number; displayName: string; redacted: number }[]>([])
const saving = ref(false)
const error = ref('')

const principals = computed<SharePrincipal[]>(() => everyone.value ? [{ type: 'everyone', id: 0 }] : [
  ...[...groups.value].map(id => ({ type: 'group' as const, id })),
  ...[...people.value].map(id => ({ type: 'user' as const, id })),
])
function toggle(kind: 'group' | 'user', id: number) {
  const target = kind === 'group' ? groups : people
  const next = new Set(target.value)
  if (next.has(id)) next.delete(id); else next.add(id)
  target.value = next
}
async function refreshPreview() {
  try { preview.value = await store.accessPreview(props.dashboardId) } catch { preview.value = [] }
}
async function save() {
  saving.value = true
  error.value = ''
  try {
    const result = await store.share(props.dashboardId, principals.value)
    emit('saved', result.shares)
    await refreshPreview()
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  } finally { saving.value = false }
}
onMounted(async () => {
  if (groupStore.groups.length === 0) await groupStore.fetchGroups()
  try { users.value = (await api<typeof users.value>('/auth/admin_enum')) ?? [] } catch { users.value = [] }
  await refreshPreview()
})
</script>

<template>
  <div class="share-backdrop" @click.self="emit('close')">
    <div class="share-dialog" role="dialog" aria-modal="true" aria-labelledby="share-title">
      <header><h2 id="share-title">Share dashboard</h2><button class="btn-close btn-close-white" aria-label="Close" @click="emit('close')" /></header>
      <p class="hint">Sharing lists the dashboard for these people. It never grants access to cameras: each viewer only sees cameras their access groups allow.</p>
      <label class="form-check"><input v-model="everyone" class="form-check-input" type="checkbox"> <span class="form-check-label">Everyone</span></label>
      <fieldset :disabled="everyone">
        <legend>Access groups</legend>
        <label v-for="group in groupStore.groups" :key="group.id" class="form-check">
          <input class="form-check-input" type="checkbox" :checked="groups.has(group.id)" @change="toggle('group', group.id)">
          <span class="form-check-label">{{ group.displayName }}</span>
        </label>
        <legend>People</legend>
        <label v-for="user in users.filter(u => u.enabled)" :key="user.userid" class="form-check">
          <input class="form-check-input" type="checkbox" :checked="people.has(user.userid)" @change="toggle('user', user.userid)">
          <span class="form-check-label">{{ user.displayName || user.username }}</span>
        </label>
      </fieldset>
      <section v-if="preview.length" class="preview">
        <h3>Currently visible to</h3>
        <ul>
          <li v-for="viewer in preview" :key="viewer.userId">
            {{ viewer.displayName }}
            <span v-if="viewer.redacted" class="redacted">{{ viewer.redacted }} hidden</span>
          </li>
        </ul>
      </section>
      <p v-if="error" class="text-danger small">{{ error }}</p>
      <footer>
        <button class="btn btn-sm btn-outline-secondary" type="button" @click="emit('close')">Close</button>
        <button class="btn btn-sm btn-primary" type="button" :disabled="saving" @click="save">Save sharing</button>
      </footer>
    </div>
  </div>
</template>

<style scoped>
.share-backdrop { position: fixed; inset: 0; z-index: 3000; display: flex; align-items: center; justify-content: center; padding: 16px; background: rgba(0, 0, 0, .6); }
.share-dialog { width: min(440px, 100%); max-height: calc(100dvh - 32px); overflow: auto; padding: 1rem 1.1rem; border: 1px solid #2d3b4e; border-radius: .8rem; background: #141c28; color: var(--witness-ink, #edf3fc); }
header { display: flex; align-items: center; justify-content: space-between; }
h2 { font-size: 1.05rem; margin: 0; }
h3 { font-size: .8rem; color: var(--witness-muted, #a5b6cc); margin: .75rem 0 .25rem; text-transform: uppercase; letter-spacing: .08em; }
.hint { font-size: .78rem; color: var(--witness-muted, #a5b6cc); margin: .5rem 0 .75rem; }
fieldset { border: 0; padding: 0; margin: .25rem 0 0; }
fieldset:disabled { opacity: .5; }
legend { font-size: .72rem; color: var(--witness-muted, #a5b6cc); text-transform: uppercase; letter-spacing: .08em; margin: .6rem 0 .2rem; }
.preview ul { list-style: none; padding: 0; margin: 0; font-size: .8rem; }
.preview li { display: flex; justify-content: space-between; padding: .15rem 0; }
.redacted { color: #f5c26b; font-size: .72rem; }
footer { display: flex; justify-content: flex-end; gap: .5rem; margin-top: 1rem; }
</style>
