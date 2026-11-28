import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'

const bridge = vi.hoisted(() => ({
  readConfig: vi.fn(), writeConfig: vi.fn(), startNetDaemon: vi.fn(), restoreNet: vi.fn(),
  getNetStatus: vi.fn(), netSelftest: vi.fn(),
}))
vi.mock('@/helpers/bridge', () => bridge)

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
  bridge.readConfig.mockResolvedValue({ enabled: true, profile: 'game', auto: true, confirm_ms: 7777, app_profiles: { 'a.b': 'download' }, boost_apps: ['x.one'], jail_apps: ['y.two'], toggles: { conntrack: true } })
  bridge.getNetStatus.mockResolvedValue({ running: true, enabled: true, profile: 'game', auto: true, events_ok: true, foreground: 'x.one', rules: { active: true, boost: 1, jail: 1 }, tcp: { current: 'cubic', available: ['reno', 'bbr', 'cubic'] } })
  bridge.writeConfig.mockResolvedValue(); bridge.startNetDaemon.mockResolvedValue(); bridge.restoreNet.mockResolvedValue()
})

const settle = async (s) => { await s.flush(); await new Promise((r) => setTimeout(r, 0)) }

describe('net store', () => {
  it('loads the config and the live status; the status line is readable', async () => {
    const { useNetStore } = await import('@/stores/net'); const s = useNetStore()
    await s.load(); await new Promise((r) => setTimeout(r, 10))
    expect(s.enabled && s.auto).toBe(true); expect(s.profile).toBe('game')
    expect(s.boostApps).toEqual(['x.one']); expect(s.toggles.conntrack).toBe(true); expect(s.toggles.ipv6_off).toBe(false)
    expect(s.statusLine).toContain('Smart Auto: x.one'); expect(s.statusLine).toContain('1 boosted · 1 isolated')
    expect(s.tcpOptions.map((o) => o.value)).toEqual(['', 'auto', 'reno', 'bbr', 'cubic'])
  })

  it('rapid taps coalesce into ONE write and ONE daemon reload; unknown keys survive', async () => {
    const { useNetStore } = await import('@/stores/net'); const s = useNetStore(); await s.load()
    s.profile = 'download'; s.queueSave(); s.toggles.ipv6_off = true; s.queueSave(); s.tcpAlgo = 'bbr'; s.queueSave()
    await settle(s)
    expect(bridge.writeConfig).toHaveBeenCalledTimes(1); expect(bridge.startNetDaemon).toHaveBeenCalledTimes(1)
    const [name, cfg] = bridge.writeConfig.mock.calls[0]
    expect(name).toBe('net_config.json')
    expect(cfg.profile).toBe('download'); expect(cfg.tcp_algo).toBe('bbr'); expect(cfg.toggles.ipv6_off).toBe(true); expect(cfg.toggles.conntrack).toBe(true)
    expect(cfg.confirm_ms).toBe(7777); expect(cfg.app_profiles).toEqual({ 'a.b': 'download' })   // hand-edited keys preserved
    expect(bridge.restoreNet).not.toHaveBeenCalled()
  })

  it('turning the engine OFF restores device values; turning it ON again does not', async () => {
    const { useNetStore } = await import('@/stores/net'); const s = useNetStore(); await s.load()
    s.enabled = false; s.queueSave(); await settle(s)
    expect(bridge.writeConfig.mock.calls[0][1].enabled).toBe(false)
    expect(bridge.restoreNet).toHaveBeenCalledTimes(1); expect(bridge.startNetDaemon).not.toHaveBeenCalled()
    s.enabled = true; s.queueSave(); await settle(s)
    expect(bridge.startNetDaemon).toHaveBeenCalledTimes(1); expect(bridge.restoreNet).toHaveBeenCalledTimes(1)
  })

  it('an app is boosted OR isolated, never both', async () => {
    const { useNetStore } = await import('@/stores/net'); const s = useNetStore(); await s.load()
    s.setJail('x.one', true)                       // x.one was boosted
    expect(s.jailApps).toContain('x.one'); expect(s.boostApps).not.toContain('x.one')
    s.setBoost('x.one', true)
    expect(s.boostApps).toContain('x.one'); expect(s.jailApps).not.toContain('x.one')
    s.setBoost('x.one', true); expect(s.boostApps.filter((p) => p === 'x.one').length).toBe(1)   // no dupes
    s.setBoost('x.one', false); expect(s.boostApps).not.toContain('x.one')
    await settle(s)
  })

  it('a failed daemon start surfaces as saveError instead of being swallowed', async () => {
    bridge.startNetDaemon.mockRejectedValue(new Error('yay_net: wifi bad'))
    const { useNetStore } = await import('@/stores/net'); const s = useNetStore(); await s.load()
    s.profile = 'social'; s.queueSave(); await settle(s)
    expect(s.saveError).toContain('yay_net')
  })
})
