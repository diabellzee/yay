import { createRouter, createWebHistory } from 'vue-router'
import { WebUI } from 'webuix'

import Home from '@/views/Home.vue'
import { isWebUIX, onHardwareBack } from '@/helpers/bridge'

// Only Home is in the entry chunk; the rest are split out so the first paint
// parses less JS. prefetchViews() (called once the app is idle) pulls them in
// the background, so tapping a tab never waits on a network/disk round-trip.
const loaders = [
  () => import('@/views/Rules.vue'),
  () => import('@/views/Games.vue'),
  () => import('@/views/Io.vue'),
  () => import('@/views/Log.vue'),
  () => import('@/views/Settings.vue'),
]
const [Rules, Games, Io, Log, Settings] = loaders

export function prefetchViews() {
  for (const load of loaders) load().catch(() => {})
}

const routes = [
  { path: '/', name: 'Home', component: Home },
  { path: '/rules', name: 'Rules', component: Rules },
  { path: '/games', name: 'Games', component: Games },
  { path: '/io', name: 'Io', component: Io },
  { path: '/log', name: 'Log', component: Log },
  { path: '/settings', name: 'Settings', component: Settings },

  // catch-all
  { path: '/:pathMatch(.*)*', redirect: '/' },
]

const router = createRouter({
  history: createWebHistory(),
  routes,
})

// Top-level tabs, used by App.vue to suppress page-open/close transitions
// between sibling tabs (only nested/detail navigation should slide).
export const topLevelRoutes = ['/', '/rules', '/games', '/io', '/log', '/settings']

// ── WebUI X hardware/gesture back-button support ────────────────────────────
// No-op outside WebUI X (plain KernelSU WebUI has its own native back
// handling; a browser tab just uses the browser's back button).
onHardwareBack(() => {
  const current = router.currentRoute.value.path || '/'
  if (topLevelRoutes.includes(current)) {
    if (isWebUIX()) {
      try {
        new WebUI().exit()
      } catch (e) {
        console.debug('[router] WebUI.exit() not available:', e)
      }
    }
    return
  }
  router.push('/')
})

export default router
