import { defineStore } from 'pinia'
import { ref } from 'vue'
import { readLog, readPrevLog } from '@/helpers/bridge'

export const useLogStore = defineStore('log', () => {
  const current  = ref('')
  const previous = ref('')
  const loading  = ref(false) // true only until the first read of a given log finishes
  const tab      = ref('current') // 'current' | 'previous'

  let currentLoaded = false
  let previousLoaded = false
  let inflight = null

  // run.log can be up to 256 KB and, on KernelSU, every read is a root-shell
  // exec + base64 round-trip. So: read only the log being looked at (the
  // previous one is fetched the first time its tab opens), keep showing the
  // old text while a refresh runs, and never run two reads at once.
  function load({ force = false } = {}) {
    if (inflight) return inflight
    const wantPrevious = tab.value === 'previous'
    if (!force && (wantPrevious ? previousLoaded : false)) return Promise.resolve()

    const first = wantPrevious ? !previousLoaded : !currentLoaded
    if (first) loading.value = true

    inflight = (async () => {
      try {
        if (wantPrevious) {
          previous.value = await readPrevLog()
          previousLoaded = true
        } else {
          current.value = await readLog()
          currentLoaded = true
        }
      } finally {
        loading.value = false
      }
    })().finally(() => {
      inflight = null
    })
    return inflight
  }

  return { current, previous, loading, tab, load }
})
