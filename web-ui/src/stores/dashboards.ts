import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { api, ApiError } from '../composables/useApi'
import { useEventStream } from '../composables/useEventStream'
import { normaliseDocument, type DashboardDocument, type DashboardRecord, type DashboardSummary } from '../types/dashboard'

export type SharePrincipal = { type: 'everyone' | 'user' | 'group'; id: number }
export type SaveResult =
  | { ok: true; revision: number }
  | { ok: false; conflict: true; revision: number }
  | { ok: false; conflict: false; message: string; issues?: { tile: string; code: string }[] }

const LAST_DASHBOARD_KEY = 'witness-dashboard-last'

export const useDashboardStore = defineStore('dashboards', () => {
  const list = ref<DashboardSummary[]>([])
  const admin = ref(false)
  const loaded = ref(false)
  // Monotonic counter bumped by dashboard:changed so open views can refetch.
  const changes = ref(new Map<number, number>())
  let unsubscribe: (() => void) | null = null

  const personal = computed(() => list.value.filter(d => d.scope === 'personal'))
  const shared = computed(() => list.value.filter(d => d.scope === 'shared'))

  async function fetchList() {
    const data = await api<{ dashboards: DashboardSummary[]; admin: boolean }>('/dashboard/enum')
    list.value = data?.dashboards ?? []
    admin.value = !!data?.admin
    loaded.value = true
    return list.value
  }

  async function get(id: number): Promise<DashboardRecord | null> {
    try {
      const record = await api<DashboardRecord>(`/dashboard/get/${id}`)
      if (!record) return null
      return { ...record, document: normaliseDocument(record.document) }
    } catch (error) {
      if (error instanceof ApiError && error.status === 404) return null
      throw error
    }
  }

  function failure(error: unknown): SaveResult {
    if (error instanceof ApiError) {
      const body = error.body as { revision?: number; issues?: { tile: string; code: string }[] } | null
      if (error.status === 409) return { ok: false, conflict: true, revision: Number(body?.revision) || 0 }
      return { ok: false, conflict: false, message: describeError(error.message), issues: body?.issues }
    }
    return { ok: false, conflict: false, message: error instanceof Error ? error.message : String(error) }
  }

  async function create(name: string, document: DashboardDocument, scope: 'personal' | 'shared' = 'personal') {
    const result = await api<{ id: number; revision: number }>('/dashboard/create', { method: 'POST', body: { name, scope, document } })
    await fetchList()
    return result
  }

  async function save(id: number, baseRevision: number, changes: { name?: string; document?: DashboardDocument }): Promise<SaveResult> {
    try {
      const result = await api<{ revision: number }>('/dashboard/update', { method: 'POST', body: { id, baseRevision, ...changes } })
      void fetchList()
      return { ok: true, revision: result.revision }
    } catch (error) {
      return failure(error)
    }
  }

  async function remove(id: number) {
    await api('/dashboard/delete', { method: 'POST', body: { id } })
    if (lastDashboardId() === id) localStorage.removeItem(LAST_DASHBOARD_KEY)
    await fetchList()
  }

  async function share(id: number, principals: SharePrincipal[]) {
    return api<{ shares: SharePrincipal[] }>('/dashboard/share', { method: 'POST', body: { id, principals } })
  }

  async function history(id: number) {
    const data = await api<{ revisions: { revision: number; updatedAt: number; updatedBy: number; name: string }[] }>(`/dashboard/history/${id}`)
    return data?.revisions ?? []
  }

  async function revision(id: number, rev: number) {
    const data = await api<{ document: unknown; redacted: number }>(`/dashboard/revision/${id}/${rev}`)
    return data ? { document: normaliseDocument(data.document), redacted: data.redacted } : null
  }

  async function accessPreview(id: number) {
    const data = await api<{ viewers: { userId: number; displayName: string; redacted: number }[] }>(`/dashboard/access_preview/${id}`)
    return data?.viewers ?? []
  }

  function lastDashboardId(): number | null {
    try {
      const value = Number(localStorage.getItem(LAST_DASHBOARD_KEY))
      return Number.isInteger(value) && value > 0 ? value : null
    } catch { return null }
  }

  function rememberDashboard(id: number | null) {
    try {
      if (id === null) localStorage.removeItem(LAST_DASHBOARD_KEY)
      else localStorage.setItem(LAST_DASHBOARD_KEY, String(id))
    } catch { /* storage unavailable */ }
  }

  function listen() {
    if (unsubscribe) return
    unsubscribe = useEventStream().onEvent(evt => {
      if (evt.event === 'dashboard:changed') {
        const id = Number(evt.data.id)
        const next = new Map(changes.value)
        next.set(id, (next.get(id) ?? 0) + 1)
        changes.value = next
        void fetchList().catch(() => undefined)
      } else if (evt.event === 'access:changed') {
        // Access changes alter what every dashboard shows; refetch all.
        const next = new Map<number, number>()
        for (const d of list.value) next.set(d.id, (changes.value.get(d.id) ?? 0) + 1)
        changes.value = next
        void fetchList().catch(() => undefined)
      }
    })
  }

  return {
    list, admin, loaded, changes, personal, shared,
    fetchList, get, create, save, remove, share, history, revision, accessPreview,
    lastDashboardId, rememberDashboard, listen,
  }
})

const ERROR_TEXT: Record<string, string> = {
  'camera-not-available': 'A camera on this dashboard is not available to your account.',
  'overlap': 'Two tiles overlap.',
  'out-of-bounds': 'A tile is outside the canvas.',
  'duplicate-camera': 'A camera is placed more than once.',
  'invalid-name': 'Enter a name (up to 80 characters).',
  'read-only': 'You can view this dashboard but not change it.',
  'too-many-dashboards': 'You have reached the limit of 50 dashboards.',
  'too-many-tiles': 'Dashboards can have up to 64 tiles.',
  'too-large': 'This dashboard is too large to save.',
  'shared-requires-admin': 'Only administrators can create shared dashboards.',
  'not-found': 'This dashboard no longer exists.',
}

export function describeError(code: string) {
  return ERROR_TEXT[code] ?? `Could not save (${code}).`
}
