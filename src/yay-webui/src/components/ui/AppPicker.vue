<template>
  <Modal :show="show" :title="title" :description="description" @close="$emit('close')">
    <!-- search + tabs stay put while the list scrolls -->
    <div class="sticky top-0 z-10 bg-surface px-2 pb-2">
      <div class="bg-surface-container rounded-full flex items-center gap-2 px-3 py-2">
        <SearchIcon class="text-on-surface-variant shrink-0" />
        <input v-model="rawQuery" placeholder="Search apps…"
          class="bg-transparent border-none outline-none text-sm text-on-surface placeholder-on-surface-variant flex-1" />
      </div>
      <div class="mt-2 grid grid-cols-2 rounded-full bg-surface-container p-1 text-sm font-medium">
        <button v-for="t in TABS" :key="t.key" @click="tab = t.key"
          class="rounded-full py-1.5"
          :class="tab === t.key ? 'bg-secondary-container text-on-surface' : 'text-on-surface-variant'">
          {{ t.label }}
        </button>
      </div>
    </div>

    <Skeleton v-if="loadingList" :rows="5" />
    <div v-else-if="error" class="text-error text-sm py-6 text-center">{{ error }}</div>
    <div v-else-if="!rows.length" class="text-on-surface-variant text-sm py-8 text-center">No apps found</div>
    <div v-else class="pb-2">
      <div v-for="r in rows" :key="r.pkg" class="lazy-row flex items-center gap-3 px-3 py-2">
        <div class="relative w-10 h-10 shrink-0 rounded-xl overflow-hidden bg-surface-container-high
                    flex items-center justify-center text-on-surface-variant font-medium">
          <span>{{ (r.label || r.pkg).charAt(0).toUpperCase() }}</span>
          <img v-if="!iconFailed[r.pkg]" :src="appIconUrl(r.pkg)" loading="lazy" decoding="async"
            class="absolute inset-0 w-full h-full object-cover" @error="iconFailed[r.pkg] = true" alt="" />
        </div>
        <div class="min-w-0 flex-1">
          <p class="text-sm text-on-surface truncate" :class="r.selected ? 'font-medium' : ''">{{ r.label || r.pkg }}</p>
          <p class="text-xs text-on-surface-variant truncate">{{ r.pkg }}<span v-if="r.other" class="text-primary"> · {{ otherLabel }}</span></p>
        </div>
        <ToggleSwitch :modelValue="r.selected" @update:modelValue="(v) => $emit('toggle', r.pkg, v)" />
      </div>
    </div>

    <template #actions>
      <button @click="$emit('close')" class="px-4 h-10 rounded-full bg-primary text-on-primary text-sm font-medium">Done</button>
    </template>
  </Modal>
</template>

<script setup>
import { ref, reactive, computed, watch, onBeforeUnmount } from 'vue'
import Modal from '@/components/ui/Modal.vue'
import ToggleSwitch from '@/components/ui/ToggleSwitch.vue'
import Skeleton from '@/components/ui/Skeleton.vue'
import SearchIcon from '@/components/icons/Search.vue'
import { listAppsDetailed, getAppLabels, appIconUrl } from '@/helpers/bridge'

const props = defineProps({
  show: Boolean,
  title: String,
  description: String,
  selected: { type: Array, default: () => [] },
  other: { type: Array, default: () => [] },       // packages chosen in the sibling list
  otherLabel: { type: String, default: '' },
})
defineEmits(['toggle', 'close'])

const TABS = [
  { key: 'user', label: 'User apps' },
  { key: 'system', label: 'System apps' },
]

const tab = ref('user')
const rawQuery = ref('')
const query = ref('')
const lists = reactive({ user: null, system: null })   // null = not fetched yet
const labels = ref({})
const iconFailed = reactive({})
const loadingList = ref(false)
const error = ref('')

// Debounce typing so a keystroke doesn't re-sort/re-render hundreds of rows.
let t = null
watch(rawQuery, (v) => {
  clearTimeout(t)
  t = setTimeout(() => (query.value = v.trim().toLowerCase()), 200)
})
onBeforeUnmount(() => clearTimeout(t))

async function ensure(kind) {
  if (lists[kind]) return
  loadingList.value = true
  error.value = ''
  try {
    const apps = await listAppsDetailed(kind)
    lists[kind] = apps.map((a) => a.packageName)
    // labels fill in progressively, after the list is already on screen
    getAppLabels(lists[kind], (chunk) => (labels.value = { ...labels.value, ...chunk }))
  } catch (e) {
    error.value = e.message
  } finally {
    loadingList.value = false
  }
}

watch(() => [props.show, tab.value], ([shown, kind]) => shown && ensure(kind), { immediate: true })

const rows = computed(() => {
  const base = lists[tab.value] ?? []
  const known = new Set([...(lists.user ?? []), ...(lists.system ?? [])])
  // chosen apps that are no longer installed still need a way to be removed
  const stale = tab.value === 'user' ? props.selected.filter((p) => !known.has(p)) : []
  const sel = new Set(props.selected)
  const oth = new Set(props.other)
  const q = query.value

  const out = []
  for (const pkg of [...base, ...stale]) {
    const label = labels.value[pkg] ?? ''
    if (q && !pkg.toLowerCase().includes(q) && !label.toLowerCase().includes(q)) continue
    out.push({ pkg, label, selected: sel.has(pkg), other: oth.has(pkg) })
  }
  out.sort((a, b) =>
    a.selected !== b.selected
      ? (a.selected ? -1 : 1)
      : (a.label || a.pkg).toLowerCase().localeCompare((b.label || b.pkg).toLowerCase()),
  )
  return out
})
</script>
