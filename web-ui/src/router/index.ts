import { createRouter, createWebHistory, type RouteLocationNormalized } from 'vue-router'
import { useAuthStore } from '../stores/auth'

const routes = [
  {
    path: '/',
    component: () => import('../views/DashboardView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/clips',
    component: () => import('../views/ClipsView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/clips/:cameraId',
    component: () => import('../views/ClipsView.vue'),
    meta: { requiresAuth: true },
  },
  {
    path: '/stream/:cameraId',
    component: () => import('../views/StreamView.vue'),
    meta: { requiresAuth: true },
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
      { path: 'detection', component: () => import('../components/admin/DetectionSettings.vue') },
      { path: 'storage', component: () => import('../components/admin/StorageSettings.vue') },
      { path: 'streaming', component: () => import('../components/admin/StreamingSettings.vue') },
      { path: 'tags', component: () => import('../components/admin/TagManager.vue') },
      { path: 'actions', component: () => import('../components/admin/ActionManager.vue') },
      { path: 'faces', component: () => import('../components/admin/FaceManager.vue') },
      { path: 'debug', component: () => import('../components/admin/DebugValues.vue') },
    ],
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
