<template>
  <div class="page h-full flex flex-col">
    <div class="sticky top-0 z-10 bg-background">
      <div class="max-w-2xl mx-auto px-5 pt-5 pb-3">
        <h1 class="text-xl font-semibold text-on-surface">Settings</h1>
      </div>
    </div>

    <div class="scrollbar-hidden pb-safe-nav flex-1 min-h-0 overflow-y-auto">
      <div class="max-w-2xl mx-auto px-5 py-2 space-y-4">

        <!-- Watcher -->
        <div class="px-1 pb-1">
          <h2 class="text-xs font-medium text-on-surface-variant uppercase tracking-wide">Automation</h2>
        </div>
        <div class="bg-surface-container rounded-2xl">
          <div class="flex items-center justify-between px-4 py-4 gap-4">
            <div class="min-w-0 flex-1">
              <p class="text-sm font-medium text-on-surface">Inotify watcher</p>
              <p class="text-xs text-on-surface-variant mt-1">
                Re-applies rules/game config automatically the moment a JSON file changes.
                Takes effect on next boot.
              </p>
            </div>
            <ToggleSwitch :modelValue="watcherEnabled" :disabled="watcherLoading" @update:modelValue="toggleWatcher" class="shrink-0" />
          </div>
        </div>

        <!-- Auto Private DNS -->
        <div class="bg-surface-container rounded-2xl">
          <div class="flex items-center justify-between px-4 py-4 gap-4">
            <div class="min-w-0 flex-1">
              <p class="text-sm font-medium text-on-surface">Auto Private DNS</p>
              <p class="text-xs text-on-surface-variant mt-1">
                Wi-Fi uses your NextDNS host; mobile data switches to Cloudflare, which carrier
                filtering doesn't block. Event-driven (no polling). If you change Private DNS
                yourself it stays untouched until you set it back to one of these two hosts,
                or reboot.
              </p>
            </div>
            <LoadingSpinner v-if="dns.saving" :size="20" class="shrink-0" />
            <ToggleSwitch v-else :key="dnsToggleKey" :modelValue="dns.enabled" :disabled="dns.loading" @update:modelValue="toggleDns" class="shrink-0" />
          </div>

          <div class="px-4 pb-4 pt-4 space-y-3 border-t border-outline-variant/15">
            <div class="-mx-4 rounded-xl overflow-hidden">
              <SelectField label="Wi-Fi provider preset" :modelValue="presetFor(dns.wifiHost)"
                :options="PRESET_OPTIONS" @update:modelValue="pickPreset('wifiHost', $event)" />
            </div>
            <InputField v-model="dns.wifiHost" label="Wi-Fi host (your NextDNS endpoint)"
              placeholder="abc123.dns.nextdns.io" />
            <div class="-mx-4 rounded-xl overflow-hidden">
              <SelectField label="Mobile data provider preset" :modelValue="presetFor(dns.mobileHost)"
                :options="PRESET_OPTIONS" @update:modelValue="pickPreset('mobileHost', $event)" />
            </div>
            <InputField v-model="dns.mobileHost" label="Mobile data host"
              placeholder="1dot1dot1dot1.cloudflare-dns.com" />

            <p v-if="dnsStatusLine" class="text-xs text-on-surface-variant break-all">{{ dnsStatusLine }}</p>

            <div class="flex justify-end">
              <button @click="saveDns" :disabled="dns.saving || dns.loading"
                class="px-4 h-9 rounded-full bg-primary text-on-primary text-sm font-medium
                       disabled:opacity-50 active:opacity-80 transition-opacity">
                Save
              </button>
            </div>
          </div>
        </div>

        <!-- Log export -->
        <div class="px-1 pb-1 pt-2">
          <h2 class="text-xs font-medium text-on-surface-variant uppercase tracking-wide">Diagnostics</h2>
        </div>
        <div class="bg-surface-container rounded-2xl overflow-hidden">
          <Ripple @click="openExportModal" class="w-full">
            <div class="flex items-center gap-4 px-4 py-4">
              <div class="w-9 h-9 rounded-full bg-primary-container flex items-center justify-center shrink-0">
                <ContentSaveIcon class="text-on-primary-container" />
              </div>
              <div class="flex-1 min-w-0">
                <p class="text-sm font-medium text-on-surface">Export logs</p>
                <p class="text-xs text-on-surface-variant mt-0.5">Bundle run.log + configs to /sdcard</p>
              </div>
              <ChevronRightIcon class="text-on-surface-variant shrink-0" />
            </div>
          </Ripple>
        </div>

        <!-- About -->
        <div class="px-1 pb-1 pt-2">
          <h2 class="text-xs font-medium text-on-surface-variant uppercase tracking-wide">About</h2>
        </div>
        <div class="bg-surface-container rounded-2xl divide-y divide-outline-variant/15">
          <InfoRow icon="🧩" label="Version" :value="`yay v${YAY_VERSION}`" />
          <InfoRow icon="🔗" label="Bridge" :value="bridge" />
          <InfoRow icon="📁" label="Config path" value="/data/adb/yay/config/" mono />
          <InfoRow icon="📜" label="Log path" value="/data/adb/yay/run.log" mono />
        </div>

      </div>
    </div>

    <!-- Export modal -->
    <Modal :show="showExportModal" title="Export logs" @close="closeExportModal" :closeOnOutsideClick="false">
      <div class="px-4 pb-2">
        <div v-if="exportStatus === 'loading'" class="flex flex-col items-center gap-4 py-6">
          <LoadingSpinner :size="40" />
          <p class="text-on-surface-variant text-sm">Exporting…</p>
        </div>

        <div v-else-if="exportStatus === 'success'" class="flex flex-col items-center gap-3 py-4">
          <CheckIcon class="text-primary" />
          <p class="text-on-surface font-medium text-center">Exported successfully</p>
          <p class="text-on-surface-variant text-xs break-all text-center bg-surface-container-low px-4 py-3 rounded-xl w-full">
            {{ exportPath }}
          </p>
        </div>

        <div v-else-if="exportStatus === 'error'" class="flex flex-col items-center gap-3 py-4">
          <ErrorIcon class="text-error" />
          <p class="text-on-surface font-medium text-center">Export failed</p>
          <p class="text-on-surface-variant text-sm text-center">{{ exportErrorMsg }}</p>
        </div>
      </div>

      <template #actions>
        <div v-if="exportStatus !== 'loading'" class="flex gap-2">
          <button @click="closeExportModal" class="px-4 py-2 text-sm font-medium text-primary rounded-full hover:bg-primary/10 transition-colors">
            {{ exportStatus === 'success' ? 'Close' : 'OK' }}
          </button>
        </div>
      </template>
    </Modal>

    <Snackbar :show="!!toast" :message="toast" />
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onActivated } from 'vue'
import {
  isWatcherEnabled,
  setWatcherEnabled,
  bridgeName,
  exec,
  PATHS,
} from '@/helpers/bridge'

import { useDnsStore } from '@/stores/dns'
import { YAY_VERSION } from '@/helpers/version'

import ToggleSwitch from '@/components/ui/ToggleSwitch.vue'
import InputField from '@/components/ui/InputField.vue'
import SelectField from '@/components/ui/SelectField.vue'
import { DNS_PROVIDERS } from '@/helpers/dnsProviders'
import Ripple from '@/components/ui/Ripple.vue'
import Modal from '@/components/ui/Modal.vue'
import LoadingSpinner from '@/components/ui/LoadingSpinner.vue'
import Snackbar from '@/components/ui/Snackbar.vue'
import InfoRow from '@/components/ui/InfoRow.vue'
import ContentSaveIcon from '@/components/icons/ContentSave.vue'
import ChevronRightIcon from '@/components/icons/ChevronRight.vue'
import CheckIcon from '@/components/icons/Check.vue'
import ErrorIcon from '@/components/icons/Error.vue'

const dns = useDnsStore()
const watcherEnabled = ref(false)
const watcherLoading = ref(true)
const bridge = ref(bridgeName())
const toast = ref('')

const showExportModal = ref(false)
const exportStatus = ref('idle') // idle | loading | success | error
const exportPath = ref('')
const exportErrorMsg = ref('')

function showToast(msg) {
  toast.value = msg
  setTimeout(() => (toast.value = ''), 2500)
}

// Config fields load once (re-reading would clobber unsaved typing); only the
// read-only status line refreshes when the tab is shown again.
onActivated(() => {
  if (dns.loaded) dns.refreshStatus()
})

onMounted(async () => {
  dns.load()
  watcherLoading.value = true
  try {
    watcherEnabled.value = await isWatcherEnabled()
  } finally {
    watcherLoading.value = false
  }
})

// ── Auto Private DNS ────────────────────────────────────────────────────────
// Presets only FILL the host field; typing a custom host (your personal
// NextDNS endpoint) always works, and then the preset reads "Custom".
const PRESET_OPTIONS = [
  { value: '', label: 'Custom (type below)' },
  ...DNS_PROVIDERS.map((p) => ({ value: p.host, label: p.label, hint: p.host })),
]
const presetFor = (host) =>
  DNS_PROVIDERS.some((p) => p.host === String(host).trim().toLowerCase()) ? String(host).trim().toLowerCase() : ''
function pickPreset(field, value) {
  if (value) dns[field] = value // '' = "Custom": leave whatever is typed
}

const dnsToggleKey = ref(0) // bump to force the switch back to the real state
const NET_LABEL = { wifi: 'Wi-Fi', cellular: 'Mobile data', none: 'No network' }

const dnsStatusLine = computed(() => {
  const st = dns.status
  if (!st) return ''
  const parts = []
  parts.push(st.running ? 'Daemon running' : 'Daemon not running')
  if (st.network) parts.push(NET_LABEL[st.network] ?? st.network)
  const cur = st.current
  if (cur) {
    parts.push(
      cur.mode === 'hostname' ? `Private DNS: ${cur.host}` : `Private DNS: ${cur.mode || 'unset'}`,
    )
  }
  if (st.config_error) parts.push(`Config error: ${st.config_error}`)
  return parts.join(' · ')
})

async function toggleDns(value) {
  try {
    await dns.save(value)
    showToast(value ? 'Auto Private DNS enabled' : 'Auto Private DNS disabled')
  } catch (e) {
    // dns.enabled is only committed on success; re-mount the switch so the
    // native checkbox can't stay visually flipped when validation failed.
    dnsToggleKey.value++
    showToast(e.message)
  }
}

async function saveDns() {
  try {
    await dns.save()
    showToast(dns.enabled ? 'Saved — applied' : 'Saved (feature is off)')
  } catch (e) {
    showToast(e.message)
  }
}

async function toggleWatcher(value) {
  watcherEnabled.value = value
  try {
    await setWatcherEnabled(value)
    showToast(value ? 'Watcher enabled — takes effect next boot' : 'Watcher disabled')
  } catch (e) {
    watcherEnabled.value = !value
    showToast(`Error: ${e.message}`)
  }
}

function openExportModal() {
  exportStatus.value = 'loading'
  exportPath.value = ''
  exportErrorMsg.value = ''
  showExportModal.value = true

  const dest = `/sdcard/yay_logs_${Date.now()}.tar.gz`
  exec(`tar -czf "${dest}" -C /data/adb/yay run.log config 2>&1`)
    .then(({ errno, stderr }) => {
      if (errno !== 0) {
        exportStatus.value = 'error'
        exportErrorMsg.value = stderr.trim() || 'Unknown error'
      } else {
        exportStatus.value = 'success'
        exportPath.value = dest
      }
    })
    .catch((err) => {
      exportStatus.value = 'error'
      exportErrorMsg.value = err.message
    })
}

function closeExportModal() {
  showExportModal.value = false
  setTimeout(() => {
    if (exportStatus.value !== 'loading') {
      exportStatus.value = 'idle'
      exportPath.value = ''
      exportErrorMsg.value = ''
    }
  }, 200)
}
</script>
