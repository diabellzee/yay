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
  const loading = ref(true)   // config file still being read (first time only)
  const loaded = ref(false)
  const saving = ref(false)

  // Whatever else is in dns_config.json (debounce_ms, wifi_ifaces,
  // cellular_prefixes, _comment, ...) — kept and written back untouched, so
  // saving from the UI never wipes advanced hand-edited options.
  let rawConfig = {}

  // `--status` spawns `cmd settings` a few times (~hundreds of ms). It is
  // display-only, so it never blocks the toggle/inputs, and repeat visits
  // inside STATUS_FRESH_MS reuse the last answer.
  const STATUS_FRESH_MS = 5000
  let statusAt = 0

  async function refreshStatus({ force = false } = {}) {
    if (!force && Date.now() - statusAt < STATUS_FRESH_MS) return
    status.value = await getDnsStatus()
    statusAt = Date.now()
  }

  // True once the daemon has put Private DNS where it should be for the
  // network it sees (or has nothing to do because there is no network).
  function settled(st) {
    if (!st?.running) return false
    if (st.network === 'none') return true
    const want = st.network === 'wifi' ? st.wifi_host : st.mobile_host
    return st.current?.mode === 'hostname' && String(st.current.host).toLowerCase() === want
  }

  // Replaces a fixed 1.8 s sleep: poll until the daemon reports it is done
  // (usually the first poll), give up after maxMs (e.g. manual override
  // active — it will deliberately never "settle").
  async function waitForSettle(maxMs = 2500) {
    const deadline = Date.now() + maxMs
    while (Date.now() < deadline) {
      const st = await getDnsStatus()
      status.value = st
      statusAt = Date.now()
      if (settled(st)) return
      await new Promise((r) => setTimeout(r, 250))
    }
  }

  async function load() {
    loading.value = true
    try {
      rawConfig = await readConfig(CONFIG_NAME).catch(() => ({}))
      enabled.value = rawConfig.enabled === true
      wifiHost.value = rawConfig.wifi_host ?? ''
      mobileHost.value = rawConfig.mobile_host || DNS_DEFAULT_MOBILE_HOST
    } finally {
      loading.value = false
      loaded.value = true
    }
    refreshStatus({ force: true }) // background — not awaited
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
        await waitForSettle()
      } else {
        await stopDnsDaemon()
        await refreshStatus({ force: true })
      }
    } finally {
      saving.value = false
    }
  }

  return { enabled, wifiHost, mobileHost, status, loading, loaded, saving, load, validate, save, refreshStatus }
})
