import { defineStore } from 'pinia'
import { ref } from 'vue'
import {
  readConfig,
  writeConfig,
  startDnsDaemon,
  stopDnsDaemon,
  getDnsStatus,
  isValidDnsHost,
  DNS_DEFAULT_MOBILE_HOST,
} from '@/helpers/bridge'

const CONFIG_NAME = 'dns_config.json'

export const useDnsStore = defineStore('dns', () => {
  const enabled = ref(false)
  const wifiHost = ref('')
  const mobileHost = ref(DNS_DEFAULT_MOBILE_HOST)
  const status = ref(null)
  const loading = ref(true)
  const saving = ref(false)

  // Whatever else is in dns_config.json (debounce_ms, wifi_ifaces,
  // cellular_prefixes, _comment, ...) — kept and written back untouched, so
  // saving from the UI never wipes advanced hand-edited options.
  let rawConfig = {}

  async function refreshStatus() {
    status.value = await getDnsStatus()
  }

  async function load() {
    loading.value = true
    try {
      rawConfig = await readConfig(CONFIG_NAME).catch(() => ({}))
      enabled.value = rawConfig.enabled === true
      wifiHost.value = rawConfig.wifi_host ?? ''
      mobileHost.value = rawConfig.mobile_host || DNS_DEFAULT_MOBILE_HOST
      await refreshStatus()
    } finally {
      loading.value = false
    }
  }

  /** '' when fine, otherwise a short message for the toast. */
  function validate(forEnabled = enabled.value) {
    if (!forEnabled) return ''
    if (!wifiHost.value.trim()) return 'Enter your NextDNS host first (e.g. abc123.dns.nextdns.io)'
    if (!isValidDnsHost(wifiHost.value)) return 'Wi-Fi host is not a valid hostname'
    if (!isValidDnsHost(mobileHost.value)) return 'Mobile data host is not a valid hostname'
    return ''
  }

  /**
   * Persist the config, then make the daemon match it: start/reload when
   * enabled, stop when not. Throws with a user-readable message on failure.
   */
  async function save(nextEnabled = enabled.value) {
    const problem = validate(nextEnabled)
    if (problem) throw new Error(problem)

    saving.value = true
    try {
      rawConfig = {
        ...rawConfig,
        version: 1,
        enabled: nextEnabled,
        wifi_host: wifiHost.value.trim().toLowerCase(),
        mobile_host: mobileHost.value.trim().toLowerCase(),
      }
      await writeConfig(CONFIG_NAME, rawConfig)
      enabled.value = nextEnabled

      if (nextEnabled) {
        await startDnsDaemon()
        // Give the daemon a moment to debounce + write before reading state.
        await new Promise((r) => setTimeout(r, 1800))
      } else {
        await stopDnsDaemon()
      }
      await refreshStatus()
    } finally {
      saving.value = false
    }
  }

  return { enabled, wifiHost, mobileHost, status, loading, saving, load, validate, save, refreshStatus }
})
