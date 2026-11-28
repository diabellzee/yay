/**
 * Dual bridge: KernelSU WebUI + MMRL / WebUI X.
 *
 * Uses the official `kernelsu` and `webuix` npm packages (same approach as
 * Encore) rather than touching `window.ksu` / `window.webui` directly —
 * those packages already guard against the global being undefined and
 * expose a typed, promise-based API.
 *
 * WebUI X's file interface name is NOT a fixed global. Per MMRL's docs, a
 * module's own JS interfaces are exposed as `window.$<sanitizedModId>`
 * (fully deterministic — e.g. `yay` -> `window.$yay`), but its *file*
 * interface uses a short, host-assigned prefix (Encore -> `$EnFile`,
 * bindhosts -> `$BiFile`) that isn't derivable from modId by any documented
 * rule. So instead of guessing a prefix, we scan `window` for the
 * `$<Xx>File` pattern at runtime — exactly what `webuix`'s own internal
 * `WXClass` helper does — which works regardless of what prefix this
 * install happens to assign.
 *
 * Falls back to an in-memory mock when opened outside both bridges (e.g.
 * `vite dev` in a plain browser tab), so the whole UI stays clickable/
 * demoable without a rooted device.
 */

import { exec as ksuExec, toast as ksuToast } from 'kernelsu'
import { Intent, WebUI, WXEventHandler } from 'webuix'
import { mockFs, mockDelay } from './devMock'

const MOD_ID = 'yay'

// ── Runtime detection ───────────────────────────────────────────────────────

/** True when running inside KernelSU's (or a KSU-compatible) WebUI. */
export const isKSU = () => typeof window !== 'undefined' && typeof window.ksu !== 'undefined'

/**
 * True when running inside MMRL's WebUI X, detected via this module's own
 * scoped interface (`window.$yay`) — the one thing that's guaranteed to
 * exist under our exact module id, per MMRL's sanitizedModId contract.
 */
export const isWebUIX = () =>
  typeof window !== 'undefined' &&
  typeof window[`$${MOD_ID}`] !== 'undefined' &&
  Object.keys(window[`$${MOD_ID}`] ?? {}).length > 0

export const isLive = () => isKSU() || isWebUIX()

/** Human-readable bridge name for a small debug/status line in Settings. */
export function bridgeName() {
  if (isWebUIX()) return 'MMRL (WebUI X)'
  if (isKSU()) return 'KernelSU'
  return 'Browser (dev)'
}

// ── WebUI X file interface discovery ────────────────────────────────────────
// Mirrors webuix's own WXClass: find whichever `window.$XxFile` key exists,
// rather than assuming a specific prefix. Cached after first successful scan
// since the interface can't change mid-session.
let _fileInterfaceCache
function findFileInterface() {
  if (_fileInterfaceCache !== undefined) return _fileInterfaceCache
  if (typeof window === 'undefined') return (_fileInterfaceCache = null)

  const key = Object.keys(window).find((k) => /^\$\w{2}File$/.test(k))
  _fileInterfaceCache = key ? window[key] : null
  return _fileInterfaceCache
}

// ── exec ─────────────────────────────────────────────────────────────────
/**
 * Runs a shell command as root. Works when either bridge exposes a
 * KSU-compatible `window.ksu.exec` (true for KernelSU WebUI, and also true
 * for some MMRL WebView configurations) — falls back to a dev mock
 * otherwise so nothing throws outside a module context.
 */
export async function exec(cmd) {
  if (isKSU()) {
    try {
      const { errno, stdout, stderr } = await ksuExec(cmd)
      return { errno, stdout: stdout ?? '', stderr: stderr ?? '' }
    } catch (e) {
      return { errno: -1, stdout: '', stderr: String(e?.message ?? e) }
    }
  }

  console.debug('[bridge:dev] exec:', cmd)
  await mockDelay()
  return { errno: 0, stdout: '', stderr: '' }
}

/** Best-effort toast; silently does nothing outside KSU/dev. */
export function toast(message) {
  if (isKSU()) {
    try {
      ksuToast(message)
      return
    } catch (e) {
      console.warn('[bridge] toast failed:', e)
    }
  }
  console.debug('[bridge:dev] toast:', message)
}

/** Base64 helpers used to move file content across the exec boundary safely. */
function toBase64(str) {
  return btoa(unescape(encodeURIComponent(str)))
}
function fromBase64(b64) {
  return decodeURIComponent(escape(atob(b64)))
}

// ── File I/O ──────────────────────────────────────────────────────────────
export async function readFile(path) {
  const fi = findFileInterface()
  if (fi) {
    if (!fi.exists(path)) throw new Error(`readFile: not found: ${path}`)
    const content = fi.read(path)
    if (content === null) throw new Error(`readFile: failed to read: ${path}`)
    return content
  }

  if (isKSU()) {
    // base64 round-trip avoids any shell-quoting corruption on read.
    const { errno, stdout, stderr } = await exec(
      `[ -f "${path}" ] && base64 -w0 "${path}" || exit 3`,
    )
    if (errno !== 0) throw new Error(`readFile: not found: ${path}${stderr ? ` (${stderr})` : ''}`)
    try {
      return fromBase64(stdout.trim())
    } catch {
      const raw = await exec(`cat "${path}"`)
      if (raw.errno !== 0) throw new Error(`readFile: ${raw.stderr}`)
      return raw.stdout
    }
  }

  return mockFs.read(path)
}

export async function writeFile(path, content) {
  const fi = findFileInterface()
  if (fi) {
    fi.write(path, content)
    return
  }

  if (isKSU()) {
    const dir = path.slice(0, path.lastIndexOf('/'))
    const b64 = toBase64(content)
    // mkdir -p defensively, then decode base64 back into place — avoids all
    // shell-quoting pitfalls for JSON containing quotes/newlines/backslashes.
    const { errno, stderr } = await exec(
      `mkdir -p "${dir}" 2>/dev/null; echo '${b64}' | base64 -d > "${path}"`,
    )
    if (errno !== 0) throw new Error(`writeFile: ${stderr}`)
    return
  }

  return mockFs.write(path, content)
}

export async function fileExists(path) {
  const fi = findFileInterface()
  if (fi) return fi.exists(path)

  if (isKSU()) {
    const { errno } = await exec(`[ -f "${path}" ]`)
    return errno === 0
  }

  return mockFs.exists(path)
}

/**
 * Open a URL in the external browser. Prefers WebUI X's native Intent API
 * when available, falls back to `am start` via exec, then a plain
 * `window.open` in dev.
 */
export async function openWebsite(link) {
  if (isWebUIX()) {
    try {
      const webui = new WebUI()
      const intent = new Intent(Intent.ACTION_VIEW)
      intent.setData(link)
      webui.startActivity(intent)
      return
    } catch (e) {
      console.error('[bridge] openWebsite via WebUI X failed:', e)
    }
  }
  if (isKSU()) {
    const { errno } = await exec(`am start -a android.intent.action.VIEW -d "${link}"`)
    if (errno === 0) return
  }
  if (typeof window !== 'undefined') window.open(link, '_blank')
}

/**
 * List installed 3rd-party packages, for the "add game/app" pickers.
 * Returns [{ packageName }] — label is resolved lazily per-row via
 * getAppLabel, since batch label lookup differs a lot bridge-to-bridge.
 */
export async function listApps() {
  if (isKSU() && typeof window.ksu.listUserPackages === 'function') {
    try {
      return JSON.parse(window.ksu.listUserPackages()).map((packageName) => ({ packageName }))
    } catch (e) {
      console.error('[bridge] listUserPackages failed:', e)
    }
  }

  const { errno, stdout, stderr } = await exec('pm list packages -3')
  if (errno !== 0) throw new Error(`listApps: ${stderr}`)
  return stdout
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.startsWith('package:'))
    .map((l) => ({ packageName: l.slice(8).trim() }))
    .filter((p) => p.packageName)
}

/** Best-effort app label lookup; falls back to the package name. */
export async function getAppLabel(packageName) {
  try {
    if (isKSU() && typeof window.ksu.getPackagesInfo === 'function') {
      const result = JSON.parse(window.ksu.getPackagesInfo(JSON.stringify([packageName])))
      if (result?.[0] && !result[0].error) return result[0].appLabel || packageName
    }
  } catch (e) {
    console.warn('[bridge] getAppLabel failed for', packageName, e)
  }
  return packageName
}

/**
 * Registers a hardware/gesture back handler via WebUI X's WXEventHandler.
 * No-op (returns a no-op unsubscribe) outside WebUI X.
 * @param {() => void} onBack
 * @returns {() => void} unsubscribe
 */
export function onHardwareBack(onBack) {
  if (!isWebUIX() || typeof window === 'undefined') return () => {}
  try {
    const handler = new WXEventHandler()
    return handler.on(window, 'back', onBack)
  } catch (e) {
    console.debug('[bridge] WXEventHandler back listener not available:', e)
    return () => {}
  }
}

// ── yay-specific helpers ────────────────────────────────────────────────────
const CONFIG = '/data/adb/yay/config'
const LOG = '/data/adb/yay/run.log'
const LOG1 = '/data/adb/yay/run.log.1'
const BIN = '/data/adb/modules/yay/bin/yay_apply'
const CACHE = '/data/adb/yay/cache/rules.hash'
// Persistent across module updates. The pre-1.4 location inside the module
// directory is wiped by every update; it is still honoured (read, and removed
// on disable) so a not-yet-migrated install keeps showing the right state.
const WATCH_FLAG = '/data/adb/yay/enable_watch'
const WATCH_FLAG_LEGACY = '/data/adb/modules/yay/enable_watch'

export const PATHS = { CONFIG, LOG, LOG1, BIN, CACHE, WATCH_FLAG }

// ── Config snapshot: ONE root exec for everything ───────────────────────────
// On KernelSU every readFile()/fileExists() is its own root-shell spawn
// (tens of ms each). Opening each tab used to cost its own spawn(s) —
// ~11 in total across the app, each one a visible "loading…" the first time
// a tab was opened. Instead the whole config set + the watcher flag is read
// in a single exec at startup (primeConfigs) and served from memory.
//
// `configCache` always holds "the last known on-disk text" per file
// (null = file missing). It is refreshed by every batch read and updated by
// every writeConfig, so readConfig() never returns something older than the
// app itself last wrote or read. The only thing it cannot see is someone
// editing the files from a terminal while the WebUI is open — a restart of
// the WebUI re-reads.
const CONFIG_FILES = [
  'rules.json',
  'game_config.json',
  'io_config.json',
  'dns_config.json',
  'net_config.json',
]

const configCache = new Map()
let watcherCache = null
let primePromise = null

function parseBatch(stdout) {
  const configs = new Map()
  let watcher = false
  let current = null
  for (const line of stdout.split('\n')) {
    if (line.startsWith('@@F ')) {
      current = line.slice(4).trim()
      configs.set(current, '') // present; content follows (empty file => '')
    } else if (line.startsWith('@@M ')) {
      configs.set(line.slice(4).trim(), null) // missing
      current = null
    } else if (line.startsWith('@@W ')) {
      watcher = line.slice(4).trim() === '1'
      current = null
    } else if (current && line) {
      try {
        configs.set(current, fromBase64(line.trim()))
      } catch {
        configs.set(current, null)
      }
      current = null
    }
  }
  return { configs, watcher }
}

async function batchRead(names) {
  const fi = findFileInterface()

  if (!fi && isKSU()) {
    const loop = names
      .map(
        (n) =>
          `if [ -f "${n}" ]; then echo "@@F ${n}"; base64 -w0 "${n}"; echo; else echo "@@M ${n}"; fi`,
      )
      .join('; ')
    const { errno, stdout } = await exec(
      `cd "${CONFIG}" 2>/dev/null || exit 4; ${loop}; ` +
        `if [ -f "${WATCH_FLAG}" ] || [ -f "${WATCH_FLAG_LEGACY}" ]; then echo "@@W 1"; else echo "@@W 0"; fi`,
    )
    if (errno === 0) return parseBatch(stdout)
    // fall through to per-file reads if the combined command failed
  }

  // WebUI X (synchronous file interface) / dev mock: reads are cheap and
  // involve no root shell, so per-file is fine.
  const configs = new Map()
  await Promise.all(
    names.map(async (n) => {
      try {
        configs.set(n, await readFile(`${CONFIG}/${n}`))
      } catch {
        configs.set(n, null)
      }
    }),
  )
  const watcher =
    (await fileExists(WATCH_FLAG).catch(() => false)) ||
    (await fileExists(WATCH_FLAG_LEGACY).catch(() => false))
  return { configs, watcher }
}

function absorb({ configs, watcher }) {
  for (const [name, text] of configs) configCache.set(name, text)
  watcherCache = watcher
}

/**
 * Kick off the single startup read. Fire-and-forget: it never rejects, and
 * any readConfig()/readSnapshot() that arrives while it is in flight simply
 * waits for it instead of spawning its own exec.
 */
export function primeConfigs() {
  if (!primePromise) {
    primePromise = batchRead(CONFIG_FILES).then(absorb, () => {})
  }
  return primePromise
}

/**
 * Parsed config objects (null = missing/unreadable) + watcher flag.
 * `fresh: true` forces a new batch read (one exec) — used to refresh Home;
 * otherwise the primed/cached data is returned without touching disk.
 */
export async function readSnapshot(names, { fresh = false } = {}) {
  if (!fresh) {
    await primePromise
    if (names.every((n) => configCache.has(n)) && watcherCache !== null) {
      return snapshotOf(names)
    }
  }
  absorb(await batchRead(CONFIG_FILES))
  return snapshotOf(names)
}

function parseOrNull(text) {
  if (text == null) return null
  try {
    return JSON.parse(text)
  } catch {
    return null
  }
}

function snapshotOf(names) {
  const configs = {}
  for (const n of names) configs[n] = parseOrNull(configCache.get(n))
  return { configs, watcher: watcherCache === true }
}

export async function readConfig(name) {
  await primePromise
  if (configCache.has(name)) {
    const text = configCache.get(name)
    if (text === null) throw new Error(`readFile: not found: ${CONFIG}/${name}`)
    return JSON.parse(text)
  }
  const raw = await readFile(`${CONFIG}/${name}`)
  configCache.set(name, raw)
  return JSON.parse(raw)
}

export async function writeConfig(name, obj) {
  const text = JSON.stringify(obj, null, 2)
  await writeFile(`${CONFIG}/${name}`, text)
  configCache.set(name, text)
}

export async function applyRules(extra = '') {
  return exec(`${BIN} --rules ${extra}`.trim())
}

export async function applyGame() {
  return exec(`${BIN} --game`)
}

export async function applyIO() {
  return exec(`${BIN} --io`)
}

export async function applyFull() {
  return exec(`${BIN} --full`)
}

export async function dryRunRules() {
  const { stdout, stderr } = await exec(`${BIN} --rules --dry-run 2>&1`)
  return stdout || stderr
}

export async function readLog() {
  try {
    return await readFile(LOG)
  } catch {
    return ''
  }
}

export async function readPrevLog() {
  try {
    return await readFile(LOG1)
  } catch {
    return ''
  }
}

/** Whether the inotify watcher is enabled (presence of a flag file). */
export async function isWatcherEnabled() {
  await primePromise
  if (watcherCache !== null) return watcherCache
  try {
    return (await fileExists(WATCH_FLAG)) || (await fileExists(WATCH_FLAG_LEGACY))
  } catch {
    return false
  }
}

export async function setWatcherEnabled(enabled) {
  if (enabled) {
    await writeFile(WATCH_FLAG, '')
  } else {
    await exec(`rm -f "${WATCH_FLAG}" "${WATCH_FLAG_LEGACY}"`)
  }
  watcherCache = enabled
}

// ── Auto Private DNS (yay_dns) ──────────────────────────────────────────────
// The daemon owns its own lifecycle: `--start` detaches it (or makes an
// already-running one reload dns_config.json), `--stop` stops it. Both are
// idempotent, so the UI just calls them after every save.
const DNS_BIN = '/data/adb/modules/yay/bin/yay_dns'

export const DNS_DEFAULT_MOBILE_HOST = '1dot1dot1dot1.cloudflare-dns.com'

/**
 * Same rule as valid_hostname() in src/dns_policy.cpp: >= 2 labels of
 * [a-z0-9-] (1..63 chars, no leading/trailing '-'), total <= 253, and a TLD
 * containing a letter (rejects IP literals — Private DNS needs a hostname).
 */
export function isValidDnsHost(host) {
  const h = String(host ?? '').trim().toLowerCase()
  if (h.length < 1 || h.length > 253) return false
  const labels = h.split('.')
  if (labels.length < 2) return false
  if (!labels.every((l) => /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/.test(l))) return false
  return /[a-z]/.test(labels[labels.length - 1])
}

/** Start the daemon, or make a running one reload its config. */
export async function startDnsDaemon() {
  const { errno, stderr } = await exec(`${DNS_BIN} --start`)
  if (errno !== 0) throw new Error(stderr.trim() || `yay_dns --start failed (${errno})`)
}

export async function stopDnsDaemon() {
  await exec(`${DNS_BIN} --stop`)
}

/**
 * Live state straight from the binary: { running, enabled, network,
 * current: {mode, host}, wifi_host, mobile_host } — or null if unavailable
 * (older install without yay_dns, dev browser, unparsable output).
 */
export async function getDnsStatus() {
  try {
    const { errno, stdout } = await exec(`${DNS_BIN} --status`)
    if (errno !== 0) return null
    return JSON.parse(stdout)
  } catch {
    return null
  }
}

// ── Network engine (yay_net) ────────────────────────────────────────────────
// Same contract as yay_dns: `--start` detaches it or makes a running one
// reload net_config.json (idempotent), `--restore` stops it AND puts every
// value back to what the device had.
const NET_BIN = '/data/adb/modules/yay/bin/yay_net'

export async function startNetDaemon() {
  const { errno, stderr } = await exec(`${NET_BIN} --start`)
  if (errno !== 0) throw new Error(stderr.trim() || `yay_net --start failed (${errno})`)
}

export async function restoreNet() {
  await exec(`${NET_BIN} --restore`)
}

/** { running, enabled, profile, foreground, events_ok, tcp, rules, … } or null. */
export async function getNetStatus() {
  try {
    const { errno, stdout } = await exec(`${NET_BIN} --status`)
    if (errno !== 0) return null
    return JSON.parse(stdout)
  } catch {
    return null
  }
}

/** Does this ROM emit the logcat events Smart Auto depends on? */
export async function netSelftest() {
  try {
    const { stdout } = await exec(`${NET_BIN} --selftest`)
    return JSON.parse(stdout)
  } catch {
    return null
  }
}

// ── App lists for the Boost / Isolate pickers ───────────────────────────────

const DEV_APPS = {
  user: ['com.mobile.legends', 'com.android.chrome', 'org.telegram.messenger', 'com.whatsapp', 'com.spotify.music'],
  system: ['com.android.settings', 'com.android.systemui', 'com.google.android.gms'],
}

/** Package names for the 'user' or 'system' tab. */
export async function listAppsDetailed(kind = 'user') {
  if (!isLive()) return DEV_APPS[kind].map((packageName) => ({ packageName }))

  if (isKSU()) {
    try {
      const raw =
        kind === 'system' && typeof window.ksu.listSystemPackages === 'function'
          ? window.ksu.listSystemPackages()
          : kind === 'user' && typeof window.ksu.listUserPackages === 'function'
            ? window.ksu.listUserPackages()
            : null
      if (raw) return JSON.parse(raw).map((packageName) => ({ packageName }))
    } catch (e) {
      console.error('[bridge] listPackages failed:', e)
    }
  }
  const { errno, stdout, stderr } = await exec(`pm list packages ${kind === 'system' ? '-s' : '-3'}`)
  if (errno !== 0) throw new Error(`listApps: ${stderr}`)
  return stdout
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.startsWith('package:'))
    .map((l) => ({ packageName: l.slice(8).trim() }))
    .filter((p) => p.packageName)
}

/**
 * Resolve display labels in small batches, yielding to the event loop between
 * them: getPackagesInfo is a synchronous JNI call, and doing 300 packages at
 * once would freeze the picker as it opens. `onChunk({pkg: label})` fires per
 * batch so labels appear progressively.
 */
export async function getAppLabels(names, onChunk) {
  if (!(isKSU() && typeof window.ksu.getPackagesInfo === 'function')) return
  const BATCH = 40
  for (let i = 0; i < names.length; i += BATCH) {
    const slice = names.slice(i, i + BATCH)
    try {
      const res = JSON.parse(window.ksu.getPackagesInfo(JSON.stringify(slice)))
      const chunk = {}
      res.forEach((x, idx) => {
        const name = x?.packageName || slice[idx]
        if (name && x && !x.error) chunk[name] = x.appLabel || name
      })
      onChunk(chunk)
    } catch (e) {
      console.warn('[bridge] getAppLabels batch failed:', e)
    }
    await new Promise((r) => setTimeout(r, 0))
  }
}

/** KernelSU serves launcher icons itself; the <img> falls back to a letter. */
export const appIconUrl = (packageName) => `ksu://icon/${packageName}`

// ── yay_inspect: on-demand component/appops browser ────────────────────────
// Backs the "Add component" / "Add appops override" pickers so the person
// doesn't have to manually dumpsys+grep over a terminal. Both read-only,
// invoked lazily (only when a picker modal opens), never during boot apply.
const INSPECT_BIN = '/data/adb/modules/yay/bin/yay_inspect'

// Basic Android package-name shape check before it ever reaches a shell
// command — not a security boundary (exec_util on the native side doesn't
// interpolate through a shell for this arg either), just a fast client-side
// guard against obviously-wrong input before spawning a process for it.
const PACKAGE_RE = /^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$/

export async function inspectComponents(packageName) {
  if (!PACKAGE_RE.test(packageName)) {
    throw new Error(`invalid package name: ${packageName}`)
  }

  if (!isLive()) {
    await mockDelay()
    return mockInspectComponents(packageName)
  }

  const { errno, stdout, stderr } = await exec(`${INSPECT_BIN} --components "${packageName}"`)
  if (errno !== 0) throw new Error(`inspectComponents: ${stderr || 'yay_inspect failed'}`)

  let parsed
  try {
    parsed = JSON.parse(stdout)
  } catch {
    throw new Error('inspectComponents: could not parse yay_inspect output')
  }
  if (parsed.error) throw new Error(parsed.error)
  return parsed.components ?? []
}

export async function inspectAppops(packageName = '') {
  if (packageName && !PACKAGE_RE.test(packageName)) {
    throw new Error(`invalid package name: ${packageName}`)
  }

  if (!isLive()) {
    await mockDelay()
    return mockInspectAppops()
  }

  const cmd = packageName
    ? `${INSPECT_BIN} --appops "${packageName}"`
    : `${INSPECT_BIN} --appops`
  const { errno, stdout, stderr } = await exec(cmd)
  if (errno !== 0) throw new Error(`inspectAppops: ${stderr || 'yay_inspect failed'}`)

  let parsed
  try {
    parsed = JSON.parse(stdout)
  } catch {
    throw new Error('inspectAppops: could not parse yay_inspect output')
  }
  if (parsed.error) throw new Error(parsed.error)
  return parsed.ops ?? []
}

// Small representative fixture so the picker UI is fully clickable/
// demoable in a plain browser tab, without a device to actually read a
// manifest from.
function mockInspectComponents(packageName) {
  return [
    { type: 'activity', name: `${packageName}.MainActivity`, exported: true, exported_explicit: true, enabled: true },
    { type: 'activity', name: `${packageName}.SettingsActivity`, exported: false, exported_explicit: false, enabled: true },
    { type: 'service', name: `${packageName}.BackgroundSyncService`, exported: false, exported_explicit: true, enabled: true },
    { type: 'receiver', name: `${packageName}.BootReceiver`, exported: true, exported_explicit: true, enabled: true },
    { type: 'provider', name: `${packageName}.FileProvider`, exported: false, exported_explicit: true, enabled: true },
  ]
}

function mockInspectAppops() {
  return [
    { op: 'android:coarse_location', label: 'Coarse location', category: 'Location', active: true },
    { op: 'android:fine_location', label: 'Fine location', category: 'Location', active: false },
    { op: 'android:camera', label: 'Camera', category: 'Camera & Mic', active: false },
    { op: 'android:get_usage_stats', label: 'Get usage stats', category: 'Usage & Background', active: true },
    { op: 'android:system_alert_window', label: 'Draw over other apps', category: 'UI & Windows', active: false },
  ]
}
