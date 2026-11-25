<template>
  <div class="page h-full flex flex-col">
    <div class="sticky top-0 z-10 bg-background">
      <div class="max-w-2xl mx-auto px-5 pt-5 pb-2">
        <div class="flex items-center justify-between">
          <h1 class="text-xl font-semibold text-on-surface">Log</h1>
          <button @click="store.load({ force: true })" title="Refresh"
            class="w-9 h-9 rounded-full bg-surface-container flex items-center justify-center
                   text-on-surface-variant active:bg-surface-container-high transition-colors">
            <RefreshIcon />
          </button>
        </div>
        <!-- Tab -->
        <div class="flex mt-3 bg-surface-container rounded-xl p-1 gap-1">
          <button @click="store.tab = 'current'"
            class="flex-1 py-1.5 rounded-lg text-sm font-medium transition-colors"
            :class="store.tab === 'current'
              ? 'bg-secondary-container text-on-secondary-container'
              : 'text-on-surface-variant'">
            Current
          </button>
          <button @click="store.tab = 'previous'"
            class="flex-1 py-1.5 rounded-lg text-sm font-medium transition-colors"
            :class="store.tab === 'previous'
              ? 'bg-secondary-container text-on-secondary-container'
              : 'text-on-surface-variant'">
            Previous
          </button>
        </div>
        <!-- Filter chips -->
        <div class="flex gap-2 mt-2 overflow-x-auto scrollbar-hidden pb-1">
          <button v-for="f in filters" :key="f.key"
            @click="activeFilter = activeFilter === f.key ? '' : f.key"
            class="shrink-0 px-3 py-1 rounded-full text-xs font-medium border transition-colors"
            :class="activeFilter === f.key
              ? `${f.active}`
              : 'border-outline-variant text-on-surface-variant'">
            {{ f.label }}
          </button>
        </div>
      </div>
    </div>

    <div ref="scroller" class="scrollbar-hidden pb-safe-nav flex-1 min-h-0 overflow-y-auto">
      <div class="max-w-2xl mx-auto px-3 py-2">
        <LoadingSpinner v-if="store.loading" />
        <div v-else-if="!logText" class="text-center py-8 text-on-surface-variant text-sm">
          No log available
        </div>
        <div v-else class="bg-surface-container rounded-2xl p-3 overflow-x-auto">
          <button v-if="hiddenCount" @click="showAll = true"
            class="w-full mb-2 py-1.5 rounded-lg text-xs font-medium bg-surface-container-high text-on-surface-variant active:opacity-80">
            Show {{ hiddenCount }} older lines
          </button>
          <div v-for="l in visibleLines" :key="l.i"
            class="text-xs font-mono leading-5 whitespace-pre-wrap break-all"
            :class="l.cls">{{ l.text }}</div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, watch, nextTick, onActivated } from 'vue'
import { useLogStore } from '@/stores/log'
import LoadingSpinner from '@/components/ui/LoadingSpinner.vue'
import RefreshIcon from '@/components/icons/Refresh.vue'

const store = useLogStore()
const activeFilter = ref('')
const showAll = ref(false)
const scroller = ref(null)

// Newest lines are what matters; a 256 KB log is ~2500 lines, and putting
// every one in the DOM makes the tab slow to open and to filter.
const TAIL_LINES = 300

const filters = [
  { key: 'E', label: '🔴 Error',   active: 'bg-error/15 border-error text-error' },
  { key: 'W', label: '🟡 Warn',    active: 'bg-yellow-400/15 border-yellow-400 text-yellow-400' },
  { key: 'I', label: '🔵 Info',    active: 'bg-primary/15 border-primary text-primary' },
  { key: 'D', label: '⚪ Debug',   active: 'bg-surface-container-high border-outline text-on-surface' },
]

const logText = computed(() =>
  store.tab === 'current' ? store.current : store.previous
)

// Matches yay_apply's logger.cpp format exactly:
// [MM-DD HH:MM:SS X file.cpp:123] message
const LEVEL_RE = /^\[\d{2}-\d{2} \d{2}:\d{2}:\d{2} ([EWID?]) /

const LEVEL_CLASS = {
  E: 'text-error',
  W: 'text-yellow-400',
  I: 'text-on-surface',
}

// Parsed ONCE per log text. Changing the filter or the tail window only
// slices this array — no more splitting + regex per line per render.
const parsed = computed(() => {
  const text = logText.value
  if (!text) return []
  const out = []
  for (const line of text.split('\n')) {
    if (!line) continue
    const level = LEVEL_RE.exec(line)?.[1] ?? null
    out.push({ i: out.length, text: line, level, cls: LEVEL_CLASS[level] ?? 'text-on-surface-variant' })
  }
  return out
})

const filtered = computed(() =>
  activeFilter.value ? parsed.value.filter((l) => l.level === activeFilter.value) : parsed.value
)

const hiddenCount = computed(() =>
  showAll.value ? 0 : Math.max(0, filtered.value.length - TAIL_LINES)
)

const visibleLines = computed(() =>
  hiddenCount.value ? filtered.value.slice(-TAIL_LINES) : filtered.value
)

function scrollToEnd() {
  nextTick(() => {
    const el = scroller.value
    if (el) el.scrollTop = el.scrollHeight
  })
}

// Land on the newest entries after a (re)load, tab switch or filter change.
watch([logText, activeFilter], () => {
  showAll.value = false
  scrollToEnd()
})

watch(() => store.tab, () => store.load())

// Shown again via keep-alive: pick up new log lines, without a spinner.
onActivated(() => store.load())
</script>
