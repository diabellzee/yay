import { defineStore } from 'pinia'
import { ref, reactive, computed } from 'vue'
import {
  readConfig,
  writeConfig,
  startNetDaemon,
  restoreNet,
  getNetStatus,
  netSelftest,
} from '@/helpers/bridge'

const CONFIG_NAME = 'net_config.json'

export const PROFILES = [
  { value: 'off', label: 'Off (device values)', hint: 'Never touch the kernel — use what the phone shipped with' },
  { value: 'balanced', label: 'Balanced', hint: 'PingPimp baseline: larger buffers, faster keepalive' },
  { value: 'game', label: 'Game', hint: 'Lowest latency; also forces Wi-Fi low-latency mode' },
  { value: 'download', label: 'Download', hint: 'Big buffers for throughput' },
  { value: 'streaming', label: 'Streaming', hint: 'Steady, buffered playback' },
  { value: 'social', label: 'Social / browsing', hint: 'Many short connections' },
  { value: 'outdoor', label: 'Outdoor', hint: 'Weak or moving signal' },
]

export const TOGGLES = [
  { key: 'ipv6_off', title: 'Disable IPv6', desc: 'Force IPv4. Can fix IPv6 routing quirks; breaks IPv6-only mobile networks.' },
  { key: 'wifi_ps_off', title: 'Wi-Fi power-save off', desc: 'Keeps the Wi-Fi radio awake for steadier ping. Uses more battery. Needs `iw`.' },
  { key: 'low_latency', title: 'Wi-Fi low-latency mode', desc: 'Android’s own low-latency Wi-Fi lock, always on (the Game profile enables it by itself).' },
  { key: 'conntrack', title: 'Connection-tracking tuning', desc: 'Bigger table and shorter UDP/TIME_WAIT timeouts.' },
  { key: 'netstats_off', title: 'Stop network-stats collection', desc: 'Disables Android’s traffic accounting. Data-usage screens stop updating.' },
  { key: 'data_saver', title: 'Ultra data saver', desc: 'Restricts background data and caps ingress at 1 MB/s.' },
  { key: 'irq_affinity', title: 'NIC IRQs on big cores', desc: 'Pins Wi-Fi/modem interrupts to the fast CPU cores. Uses more power.' },
  { key: 'ksoft_boost', title: 'Boost ksoftirqd priority', desc: 'Gives network softirq threads top priority.' },
]

const DEFAULTS = () => ({
  enabled: false,
  profile: 'balanced',
  auto: false,
  auto_detect_games: true,
  tcp_algo: '',
  boost_apps: [],
  jail_apps: [],
  toggles: {},
})

export const useNetStore = defineStore('net', () => {
  const enabled = ref(false)
  const profile = ref('balanced')
  const auto = ref(false)
  const autoDetectGames = ref(true)
  const tcpAlgo = ref('')
  const boostApps = ref([])
  const jailApps = ref([])
  const toggles = reactive(Object.fromEntries(TOGGLES.map((t) => [t.key, false])))

  const status = ref(null)
  const loading = ref(true)
  const loaded = ref(false)
  const saving = ref(false)
  const saveError = ref('')

  // Everything in net_config.json that this UI doesn't edit (app_profiles,
  // confirm_ms, debounce_ms, _comment, …) is carried through unchanged.
  let rawConfig = {}
  let saveTimer = null
  let saveChain = Promise.resolve()

  const tcpOptions = computed(() => {
    const avail = status.value?.tcp?.available ?? []
    return [
      { value: '', label: 'Leave as is', hint: status.value?.tcp?.current ? `Currently ${status.value.tcp.current}` : '' },
      { value: 'auto', label: 'Automatic', hint: 'Picks per profile: westwood/bbr for games, bbr for downloads, cubic otherwise' },
      ...avail.map((a) => ({ value: a, label: a })),
    ]
  })

  function adopt(cfg) {
    const d = { ...DEFAULTS(), ...cfg }
    enabled.value = d.enabled === true
    profile.value = d.profile
    auto.value = d.auto === true
    autoDetectGames.value = d.auto_detect_games !== false
    tcpAlgo.value = d.tcp_algo ?? ''
    boostApps.value = [...(d.boost_apps ?? [])]
    jailApps.value = [...(d.jail_apps ?? [])]
    for (const t of TOGGLES) toggles[t.key] = d.toggles?.[t.key] === true
  }

  async function refreshStatus() {
    status.value = await getNetStatus()
  }

  async function load() {
    if (!loaded.value) loading.value = true
    try {
      rawConfig = await readConfig(CONFIG_NAME).catch(() => ({}))
      adopt(rawConfig)
    } finally {
      loading.value = false
      loaded.value = true
    }
    refreshStatus()
  }

  function snapshot() {
    return {
      ...rawConfig,
      version: 1,
      enabled: enabled.value,
      profile: profile.value,
      auto: auto.value,
      auto_detect_games: autoDetectGames.value,
      tcp_algo: tcpAlgo.value,
      boost_apps: [...boostApps.value],
      jail_apps: [...jailApps.value],
      toggles: { ...toggles },
    }
  }

  /**
   * Persist the config and make the daemon match it. Calls are serialised, so
   * two quick taps can never interleave a write with a restart.
   */
  async function persist(wasEnabled) {
    const cfg = snapshot()
    await writeConfig(CONFIG_NAME, cfg)
    rawConfig = cfg
    if (cfg.enabled) {
      await startNetDaemon()
    } else if (wasEnabled) {
      await restoreNet() // turning the engine off puts the device back as it was
    }
    setTimeout(refreshStatus, 700)
  }

  // One path for every save, so a failure can never leave the chain rejected
  // (which would silently skip every later save).
  function enqueue(wasEnabled) {
    saveChain = saveChain
      .then(async () => {
        saveError.value = ''
        saving.value = true
        try {
          await persist(wasEnabled)
        } finally {
          saving.value = false
        }
      })
      .catch((e) => {
        saveError.value = e.message
      })
    return saveChain
  }

  let pendingWasEnabled = false

  /**
   * Controls call this after changing a ref. Rapid taps (flipping several
   * toggles) coalesce into one write + one daemon reload.
   */
  function queueSave() {
    clearTimeout(saveTimer)
    // Was the engine on BEFORE this burst of edits? Turning it off must restore
    // the device; the first tap of a burst is the one that knows.
    if (saveTimer === null) {
      pendingWasEnabled = status.value?.enabled === true || rawConfig.enabled === true
    }
    saveTimer = setTimeout(() => {
      saveTimer = null
      enqueue(pendingWasEnabled)
    }, 350)
  }

  /** Await any pending save (tests, and before navigating away). */
  async function flush() {
    if (saveTimer !== null) {
      clearTimeout(saveTimer)
      saveTimer = null
      enqueue(pendingWasEnabled)
    }
    await saveChain
  }

  function setApps(listRef, pkg, on) {
    const has = listRef.value.includes(pkg)
    if (on && !has) listRef.value = [...listRef.value, pkg]
    if (!on && has) listRef.value = listRef.value.filter((p) => p !== pkg)
    // An app is boosted OR isolated, never both.
    const other = listRef === boostApps ? jailApps : boostApps
    if (on && other.value.includes(pkg)) other.value = other.value.filter((p) => p !== pkg)
    queueSave()
  }

  const setBoost = (pkg, on) => setApps(boostApps, pkg, on)
  const setJail = (pkg, on) => setApps(jailApps, pkg, on)

  const selftest = () => netSelftest()

  const statusLine = computed(() => {
    const st = status.value
    if (!st) return ''
    if (!st.enabled) return st.config_error ? `Config error: ${st.config_error}` : 'Engine is off'
    const parts = [st.running ? 'Running' : 'Not running']
    if (st.profile) parts.push(`profile ${st.profile}`)
    if (st.auto) parts.push(st.events_ok ? `Smart Auto: ${st.foreground || 'waiting for an app'}` : 'Smart Auto: no events yet')
    if (st.rules?.active) parts.push(`${st.rules.boost} boosted · ${st.rules.jail} isolated`)
    return parts.join(' · ')
  })

  return {
    enabled, profile, auto, autoDetectGames, tcpAlgo, boostApps, jailApps, toggles,
    status, loading, loaded, saving, saveError, statusLine, tcpOptions,
    load, refreshStatus, queueSave, flush, setBoost, setJail, selftest,
  }
})
