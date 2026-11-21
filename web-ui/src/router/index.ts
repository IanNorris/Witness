import { createRouter, createWebHistory, type RouteLocationNormalized, type RouteRecordRaw } from 'vue-router'
import { useAuthStore } from '../stores/auth'

const routes: RouteRecordRaw[] = [
  {
    path: '/',
    component: () => import('../views/OverviewView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/dashboards',
    component: () => import('../views/DashboardManagerView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/d/:id(\\d+)',
    component: () => import('../views/DashboardHostView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/d/:id(\\d+)/edit',
    component: () => import('../views/DashboardEditView.vue'),
    meta: { requiresAuth: true },
  },
  {
    // Chromeless wall display. All state is in the URL so reloads restore it.
    path: '/kiosk/:id(\\d+)',
    component: () => import('../views/DashboardHostView.vue'),
    meta: { requiresAuth: true, kiosk: true },
  },
  {
    path: '/clips',
    component: () => import('../views/ClipsView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/dvr',
    component: () => import('../views/DvrView.vue'),
    meta: { requiresAuth: true },
  },
  {
    // "Clips for this dashboard" (replaces the old per-group clip pages).
    path: '/clips/dashboard/:dashboardId(\\d+)',
    component: () => import('../views/ClipsView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/clips/group/:groupId',
    redirect: '/clips',
  },
  {
    path: '/clips/:cameraId',
    component: () => import('../views/ClipsView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/live/:cameraId',
    component: () => import('../views/StreamView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/stream/:cameraId',
    redirect: to => ({ path: `/live/${to.params.cameraId}`, query: to.query }),
  },
  {
    path: '/trails',
    redirect: '/clips',
  },
  {
    path: '/admin',
    component: () => import('../views/AdminView.vue'),
    meta: { requiresAuth: true, requiresAdmin: true },
    children: [
      { path: '', redirect: '/admin/cameras' },
      { path: 'cameras', component: () => import('../components/admin/CameraManager.vue') },
      { path: 'users', component: () => import('../components/admin/UserManager.vue') },
      { path: 'groups', component: () => import('../components/admin/GroupManager.vue') },
      { path: 'dashboards', component: () => import('../views/DashboardManagerView.vue'), props: { embedded: true } },
      { path: 'detection', component: () => import('../components/admin/DetectionSettings.vue') },
      { path: 'storage', component: () => import('../components/admin/StorageSettings.vue') },
      { path: 'streaming', component: () => import('../components/admin/StreamingSettings.vue') },
      { path: 'tags', component: () => import('../components/admin/TagManager.vue') },
      { path: 'actions', component: () => import('../components/admin/ActionManager.vue') },
      { path: 'faces', component: () => import('../components/admin/FaceManager.vue') },
      { path: 'queue', component: () => import('../components/admin/QueueManager.vue') },
      { path: 'health', component: () => import('../components/admin/HealthDashboard.vue') },
      { path: 'debug', component: () => import('../components/admin/DebugValues.vue') },
    ],
  },
  {
    path: '/health-monitor',
    component: () => import('../views/HealthMonitorView.vue'),
    meta: { requiresAuth: true, requiresAdmin: true },
  },
  {
    path: '/login',
    component: () => import('../views/LoginView.vue'),
  },
]

const router = createRouter({
  history: createWebHistory('/'),
  routes,
})

router.beforeEach(async (to: RouteLocationNormalized) => {
  const auth = useAuthStore()

  // Don't check auth for the login page
  if (to.path === '/login') {
    return
  }

  if (auth.isLoading) {
    await auth.fetchProfile()
  }

  if (to.meta.requiresAuth && !auth.isAuthenticated) {
    return '/login'
  }

  if (to.meta.requiresAdmin && !auth.isAdmin) {
    return '/'
  }
})

export default router
