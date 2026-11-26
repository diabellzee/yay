import { defineStore } from 'pinia'
import { ref } from 'vue'
import { readSnapshot, bridgeName, PATHS } from '@/helpers/bridge'
import { YAY_VERSION } from '@/helpers/version'

// Re-visiting Home inside this window reuses what it already has instead of
// re-reading three config files (each a root-shell exec on KernelSU).
const FRESH_MS = 3000

export const useHomeStore = defineStore('home', () => {
  const version = ref(YAY_VERSION)
  const componentCount = ref(0)
  const appopCount = ref(0)
  const gameCount = ref(0)
  const schedulerPreference = ref([])
  const watcherEnabled = ref(false)
  const bridge = ref('')
  const loading = ref(false) // true only until the FIRST load finishes
  const loaded = ref(false)

  let lastLoad = 0
  let inflight = null

  /** Config files changed on disk (saved/applied elsewhere) — next load() re-reads. */
  function invalidate() {
    lastLoad = 0
  }

  async function doLoad() {
    bridge.value = bridgeName()
    try {
      // First load reuses the startup snapshot (no exec). Later refreshes do
      // ONE batched read for all three files + the watcher flag, instead of
      // four separate root-shell spawns.
      const { configs, watcher } = await readSnapshot(
        ['rules.json', 'game_config.json', 'io_config.json'],
        { fresh: loaded.value },
      )
      const rules = configs['rules.json'] ?? { components: [], appops: [] }
      const games = configs['game_config.json'] ?? { games: [] }
      const io = configs['io_config.json'] ?? { scheduler_preference: [] }
      componentCount.value = (rules.components ?? []).filter((c) => c.enabled !== false).length
      appopCount.value = (rules.appops ?? []).filter((a) => a.enabled !== false).length
      gameCount.value = (games.games ?? []).filter((g) => g.enabled !== false).length
      schedulerPreference.value = io.scheduler_preference ?? []
      watcherEnabled.value = !!watcher
      lastLoad = Date.now()
    } finally {
      loading.value = false
      loaded.value = true
    }
  }

  /**
   * Stale-while-revalidate: once there is data, refreshing never shows the
   * spinner — old numbers stay on screen and update in place. Concurrent
   * callers share one in-flight read.
   */
  function load({ force = false } = {}) {
    if (inflight) return inflight
    if (!force && loaded.value && Date.now() - lastLoad < FRESH_MS) return Promise.resolve()
    if (!loaded.value) loading.value = true
    inflight = doLoad().finally(() => {
      inflight = null
    })
    return inflight
  }

  return {
    version,
    componentCount,
    appopCount,
    gameCount,
    schedulerPreference,
    watcherEnabled,
    bridge,
    loading,
    loaded,
    load,
    invalidate,
    PATHS,
  }
})
