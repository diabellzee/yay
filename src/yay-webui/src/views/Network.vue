<template>
  <div class="page h-full flex flex-col">
    <div class="sticky top-0 z-10 bg-background">
      <div class="max-w-2xl mx-auto px-5 pt-5 pb-3 flex items-center justify-between gap-3">
        <h1 class="text-xl font-semibold text-on-surface">Network</h1>
        <LoadingSpinner v-if="store.saving" :size="20" />
      </div>
    </div>

    <div class="scrollbar-hidden pb-safe-nav flex-1 min-h-0 overflow-y-auto">
      <div class="max-w-2xl mx-auto px-5 py-2 space-y-4">
        <Skeleton v-if="store.loading" :rows="4" />

        <template v-else>
          <!-- Engine -->
          <div class="bg-surface-container rounded-2xl">
            <div class="flex items-center justify-between px-4 py-4 gap-4">
              <div class="min-w-0 flex-1">
                <p class="text-sm font-medium text-on-surface">Network engine</p>
                <p class="text-xs text-on-surface-variant mt-1">
                  Kernel network tuning, per-app priority and blocking, driven by what you are doing.
                  Event-driven — it sleeps between events. Turning it off puts every value back to
                  what your phone had.
                </p>
              </div>
              <ToggleSwitch :modelValue="store.enabled" @update:modelValue="set('enabled', $event)" class="shrink-0" />
            </div>
            <p v-if="store.statusLine" class="px-4 pb-3 -mt-1 text-xs text-on-surface-variant break-all">
              {{ store.statusLine }}
            </p>
          </div>

          <div class="space-y-4 transition-opacity duration-200" :class="store.enabled ? '' : 'opacity-45 pointer-events-none'">
            <!-- Smart Auto -->
            <h2 class="px-1 text-xs font-medium text-primary uppercase tracking-wide">Smart Auto</h2>
            <div class="bg-surface-container rounded-2xl divide-y divide-outline-variant/15">
              <div class="flex items-center justify-between px-4 py-3.5 gap-4">
                <div class="min-w-0 flex-1">
                  <p class="text-sm font-medium text-on-surface">Switch profile by app</p>
                  <p class="text-xs text-on-surface-variant mt-0.5">
                    Games get the Game profile, video apps Streaming, chat apps Social. Reads Android’s
                    own foreground-app log — no polling, nothing runs while you stay in one app.
                  </p>
                </div>
                <ToggleSwitch :modelValue="store.auto" @update:modelValue="set('auto', $event)" class="shrink-0" />
              </div>
              <div class="flex items-center justify-between px-4 py-3.5 gap-4" :class="store.auto ? '' : 'opacity-45 pointer-events-none'">
                <div class="min-w-0 flex-1">
                  <p class="text-sm font-medium text-on-surface">Detect games automatically</p>
                  <p class="text-xs text-on-surface-variant mt-0.5">
                    Your Games list plus apps that declare themselves as games.
                  </p>
                </div>
                <ToggleSwitch :modelValue="store.autoDetectGames" @update:modelValue="set('autoDetectGames', $event)" class="shrink-0" />
              </div>
            </div>

            <!-- Tuning -->
            <h2 class="px-1 text-xs font-medium text-primary uppercase tracking-wide">Tuning</h2>
            <div class="bg-surface-container rounded-2xl divide-y divide-outline-variant/15">
              <SelectField label="Profile"
                :description="store.auto ? 'Used when no app-specific profile applies' : ''"
                :modelValue="store.profile" :options="PROFILES" @update:modelValue="set('profile', $event)" />
              <SelectField label="TCP congestion control"
                :modelValue="store.tcpAlgo" :options="store.tcpOptions" @update:modelValue="set('tcpAlgo', $event)" />
            </div>

            <!-- Per app -->
            <h2 class="px-1 text-xs font-medium text-primary uppercase tracking-wide">Per app</h2>
            <div class="bg-surface-container rounded-2xl divide-y divide-outline-variant/15">
              <Ripple class="flex items-center gap-3 px-4 py-3.5 cursor-pointer" @click="picker = 'boost'">
                <div class="min-w-0 flex-1">
                  <p class="text-sm font-medium text-on-surface">Boost apps</p>
                  <p class="text-xs text-on-surface-variant mt-0.5">Mark their traffic high-priority (DSCP) for routers that honour it.</p>
                </div>
                <span class="text-sm text-primary">{{ store.boostApps.length }}</span>
                <ChevronRight class="text-on-surface-variant shrink-0" />
              </Ripple>
              <Ripple class="flex items-center gap-3 px-4 py-3.5 cursor-pointer" @click="picker = 'jail'">
                <div class="min-w-0 flex-1">
                  <p class="text-sm font-medium text-on-surface">Isolate apps</p>
                  <p class="text-xs text-on-surface-variant mt-0.5">Block their internet access completely.</p>
                </div>
                <span class="text-sm text-primary">{{ store.jailApps.length }}</span>
                <ChevronRight class="text-on-surface-variant shrink-0" />
              </Ripple>
            </div>

            <!-- Advanced -->
            <h2 class="px-1 text-xs font-medium text-primary uppercase tracking-wide">Advanced</h2>
            <div class="bg-surface-container rounded-2xl divide-y divide-outline-variant/15">
              <div v-for="t in TOGGLES" :key="t.key" class="flex items-center justify-between px-4 py-3.5 gap-4">
                <div class="min-w-0 flex-1">
                  <p class="text-sm font-medium text-on-surface">{{ t.title }}</p>
                  <p class="text-xs text-on-surface-variant mt-0.5">{{ t.desc }}</p>
                  <p v-if="unsupported(t.key)" class="text-xs text-error mt-0.5">Not available on this ROM</p>
                </div>
                <ToggleSwitch :modelValue="store.toggles[t.key]" @update:modelValue="setToggle(t.key, $event)" class="shrink-0" />
              </div>
            </div>

            <!-- Diagnostics -->
            <h2 class="px-1 text-xs font-medium text-primary uppercase tracking-wide">Diagnostics</h2>
            <div class="bg-surface-container rounded-2xl">
              <Ripple class="flex items-center justify-between gap-3 px-4 py-3.5 cursor-pointer" @click="runSelftest">
                <div class="min-w-0 flex-1">
                  <p class="text-sm font-medium text-on-surface">Check Smart Auto on this ROM</p>
                  <p class="text-xs text-on-surface-variant mt-0.5 break-words">
                    {{ selftestText || 'Looks for the foreground-app events Smart Auto relies on.' }}
                  </p>
                </div>
                <LoadingSpinner v-if="selftesting" :size="20" class="shrink-0" />
              </Ripple>
            </div>
          </div>
        </template>
      </div>
    </div>

    <AppPicker :show="picker === 'boost'" title="Boost apps"
      description="High-priority traffic for these apps."
      :selected="store.boostApps" :other="store.jailApps" other-label="isolated"
      @toggle="store.setBoost" @close="picker = ''" />
    <AppPicker :show="picker === 'jail'" title="Isolate apps"
      description="These apps lose internet access."
      :selected="store.jailApps" :other="store.boostApps" other-label="boosted"
      @toggle="store.setJail" @close="picker = ''" />

    <Snackbar :show="!!toast" :message="toast" />
  </div>
</template>

<script setup>
import { ref, watch, onActivated } from 'vue'
import { useNetStore, PROFILES, TOGGLES } from '@/stores/net'
import Skeleton from '@/components/ui/Skeleton.vue'
import LoadingSpinner from '@/components/ui/LoadingSpinner.vue'
import ToggleSwitch from '@/components/ui/ToggleSwitch.vue'
import SelectField from '@/components/ui/SelectField.vue'
import AppPicker from '@/components/ui/AppPicker.vue'
import Ripple from '@/components/ui/Ripple.vue'
import Snackbar from '@/components/ui/Snackbar.vue'
import ChevronRight from '@/components/icons/ChevronRight.vue'

const store = useNetStore()
const picker = ref('')
const toast = ref('')
const selftesting = ref(false)
const selftestText = ref('')

function showToast(msg) {
  toast.value = msg
  setTimeout(() => (toast.value = ''), 2800)
}

// Config loads once (re-reading would clobber an in-flight edit); the
// read-only status line refreshes whenever the tab is shown again.
store.load()
onActivated(() => {
  if (store.loaded) store.refreshStatus()
})

watch(() => store.saveError, (m) => m && showToast(m))

function set(field, value) {
  store[field] = value
  store.queueSave()
}

function setToggle(key, value) {
  store.toggles[key] = value
  store.queueSave()
}

const unsupported = (key) => store.status?.toggles_unsupported?.includes(key)

async function runSelftest() {
  if (selftesting.value) return
  selftesting.value = true
  try {
    const r = await store.selftest()
    selftestText.value = !r
      ? 'Could not run the check (is the engine installed?)'
      : r.smart_auto_supported
        ? `Supported — saw ${r.foreground_events} foreground events (last: ${r.last_foreground})`
        : 'Not supported: this ROM did not log any foreground-app events, so Smart Auto cannot work'
  } finally {
    selftesting.value = false
  }
}
</script>
