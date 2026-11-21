<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppLayout from '../components/layout/AppLayout.vue'
import ShareDialog from '../components/dashboard/ShareDialog.vue'
import { useIsPhone } from '../composables/useViewport'
import { describeError, useDashboardStore, type SharePrincipal } from '../stores/dashboards'
import { createEmptyDocument, type DashboardSummary } from '../types/dashboard'

const route = useRoute()
const router = useRouter()
const store = useDashboardStore()
const phone = useIsPhone()
const creating = ref(route.query.new === '1')
const newName = ref('')
const newScope = ref<'personal' | 'shared'>('personal')
const error = ref('')
const sharing = ref<{ id: number; shares: SharePrincipal[] } | null>(null)

async function create() {
  error.value = ''
  const name = newName.value.trim()
  if (!name) { error.value = describeError('invalid-name'); return }
  try {
    const created = await store.create(name, createEmptyDocument(), newScope.value)
    void router.push(`/d/${created.id}/edit`)
  } catch (e) {
    error.value = describeError(e instanceof Error ? e.message : String(e))
  }
}
async function duplicate(d: DashboardSummary) {
  const record = await store.get(d.id)
  if (!record) return
  const created = await store.create(`${d.name} (copy)`.slice(0, 80),
    { ...record.document, tiles: record.document.tiles.filter(t => t.type !== 'unavailable') })
  void router.push(`/d/${created.id}/edit`)
}
async function remove(d: DashboardSummary) {
  if (!confirm(`Delete “${d.name}”? This cannot be undone.`)) return
  await store.remove(d.id)
}
async function openShare(d: DashboardSummary) {
  const record = await store.get(d.id)
  sharing.value = { id: d.id, shares: (record?.shares ?? []) as SharePrincipal[] }
}
function kiosk(d: DashboardSummary) {
  void document.documentElement.requestFullscreen?.().catch(() => undefined)
  void router.push(`/kiosk/${d.id}`)
}
onMounted(() => { store.listen(); void store.fetchList() })
</script>

<template>
  <AppLayout>
    <template #title>Dashboards</template>
    <section class="view-heading" aria-label="Dashboards">
      <div><div class="ui-eyebrow">DASHBOARDS</div><h1>Dashboards</h1><p>Layouts for wall displays and desktops. On phones, a dashboard shows its cameras as a list.</p></div>
      <button v-if="!phone && !creating" class="btn btn-sm btn-primary" type="button" @click="creating = true">New dashboard</button>
    </section>

    <form v-if="creating && !phone" class="create-form" @submit.prevent="create">
      <input v-model="newName" class="form-control form-control-sm" placeholder="Name, e.g. Hallway wall" maxlength="80" aria-label="Dashboard name" autofocus>
      <select v-if="store.admin" v-model="newScope" class="form-select form-select-sm" aria-label="Visibility">
        <option value="personal">Personal (only me)</option>
        <option value="shared">Shared (I choose who sees it)</option>
      </select>
      <button class="btn btn-sm btn-primary" type="submit">Create and edit</button>
      <button class="btn btn-sm btn-outline-secondary" type="button" @click="creating = false">Cancel</button>
      <span v-if="error" class="text-danger small">{{ error }}</span>
    </form>

    <template v-for="section in [{ title: 'My dashboards', items: store.personal }, { title: store.admin ? 'Shared dashboards' : 'Shared with me', items: store.shared }]" :key="section.title">
      <h2 v-if="section.items.length" class="section-title">{{ section.title }}</h2>
      <div class="dashboard-list">
        <article v-for="d in section.items" :key="d.id" class="dashboard-row">
          <RouterLink class="dashboard-open" :to="`/d/${d.id}`">
            <strong>{{ d.name }}</strong>
            <small>{{ d.tiles ?? 0 }} tiles · {{ d.cameras?.length ?? 0 }} cameras<template v-if="d.redacted"> · {{ d.redacted }} not available to you</template></small>
          </RouterLink>
          <div class="dashboard-actions">
            <button v-if="!phone" class="btn btn-sm btn-outline-secondary" type="button" @click="kiosk(d)">Full screen</button>
            <RouterLink v-if="d.canEdit && !phone" class="btn btn-sm btn-outline-secondary" :to="`/d/${d.id}/edit`">Edit</RouterLink>
            <button v-if="d.scope === 'shared' && store.admin" class="btn btn-sm btn-outline-secondary" type="button" @click="openShare(d)">Share</button>
            <button v-if="!phone" class="btn btn-sm btn-outline-secondary" type="button" :title="d.canEdit ? 'Duplicate' : 'Save a personal copy you can edit'" @click="duplicate(d)">{{ d.canEdit ? 'Duplicate' : 'Save a copy' }}</button>
            <button v-if="d.canEdit" class="btn btn-sm btn-outline-danger" type="button" @click="remove(d)">Delete</button>
          </div>
        </article>
      </div>
    </template>

    <div v-if="store.loaded && store.list.length === 0 && !creating" class="ui-empty-state">
      <h2>No dashboards yet</h2>
      <p>{{ phone ? 'Create dashboards on a desktop; they appear here as camera lists.' : 'Create one to arrange cameras, spotlights and activity for a full-screen display.' }}</p>
    </div>

    <ShareDialog v-if="sharing" :dashboard-id="sharing.id" :shares="sharing.shares" @close="sharing = null" @saved="sharing = null" />
  </AppLayout>
</template>

<style scoped>
.create-form { display: flex; align-items: center; gap: .5rem; flex-wrap: wrap; margin-bottom: 1.25rem; padding: .75rem; border: 1px solid #2c4058; border-radius: .7rem; background: #111c2b; }
.create-form input { max-width: 320px; }
.create-form select { width: auto; }
.section-title { font-size: .72rem; text-transform: uppercase; letter-spacing: .12em; color: #8fa6bf; margin: 1.25rem 0 .5rem; }
.dashboard-list { display: flex; flex-direction: column; gap: .5rem; }
.dashboard-row { display: flex; align-items: center; justify-content: space-between; gap: .75rem; flex-wrap: wrap; padding: .65rem .85rem; border: 1px solid #263547; border-radius: .7rem; background: #121a26; }
.dashboard-open { display: flex; flex-direction: column; min-width: 0; color: inherit; text-decoration: none; }
.dashboard-open small { color: var(--witness-muted, #a5b6cc); }
.dashboard-actions { display: flex; gap: .35rem; flex-wrap: wrap; }
</style>
