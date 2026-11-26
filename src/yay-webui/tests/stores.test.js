import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const bridge = vi.hoisted(() => ({
  readConfig: vi.fn(), writeConfig: vi.fn(), readSnapshot: vi.fn(), bridgeName: vi.fn(() => 'test'),
  readLog: vi.fn(), readPrevLog: vi.fn(), getDnsStatus: vi.fn(), startDnsDaemon: vi.fn(), stopDnsDaemon: vi.fn(),
  isValidDnsHost: (h) => /\./.test(h), DNS_DEFAULT_MOBILE_HOST: 'cf.example.com', PATHS: {},
  applyRules: vi.fn(), applyIO: vi.fn(), applyGame: vi.fn(), dryRunRules: vi.fn(),
}))
vi.mock('@/helpers/bridge', () => bridge)
beforeEach(() => { setActivePinia(createPinia()); vi.clearAllMocks()
  bridge.readSnapshot.mockImplementation(async () => { await sleep(20); return { configs: { 'rules.json': { components: [{enabled:true},{enabled:false}], appops: [] }, 'game_config.json': null, 'io_config.json': { scheduler_preference: ['bfq'] } }, watcher: true } })
  bridge.readConfig.mockImplementation(async (n) => n === 'dns_config.json' ? { enabled: true, wifi_host: 'a.b.io' } : {})
})
describe('home store', () => {
  it('first load = primed snapshot (fresh:false); refresh = ONE fresh batch; SWR, dedupe, TTL, invalidate', async () => {
    const { useHomeStore } = await import('@/stores/home'); const s = useHomeStore()
    const p1 = s.load(), p2 = s.load(); expect(s.loading).toBe(true); await Promise.all([p1, p2])
    expect(bridge.readSnapshot).toHaveBeenCalledTimes(1)
    expect(bridge.readSnapshot.mock.calls[0][1]).toEqual({ fresh: false })
    expect(s.componentCount).toBe(1); expect(s.gameCount).toBe(0); expect(s.watcherEnabled).toBe(true); expect(s.schedulerPreference).toEqual(['bfq'])
    await s.load(); expect(bridge.readSnapshot).toHaveBeenCalledTimes(1)          // TTL
    s.invalidate(); const p = s.load(); expect(s.loading).toBe(false); await p     // refresh: no spinner
    expect(bridge.readSnapshot).toHaveBeenCalledTimes(2)
    expect(bridge.readSnapshot.mock.calls[1][1]).toEqual({ fresh: true })          // exactly one batch
  })
  it('saving rules invalidates home', async () => {
    const { useHomeStore } = await import('@/stores/home'); const { useRulesStore } = await import('@/stores/rules')
    const h = useHomeStore(); await h.load(); await useRulesStore().save(); await h.load()
    expect(bridge.readSnapshot).toHaveBeenCalledTimes(2)
  })
})
describe('log store', () => {
  it('previous log read only when its tab opens; refresh has no spinner', async () => {
    bridge.readLog.mockImplementation(async () => { await sleep(10); return 'cur' }); bridge.readPrevLog.mockResolvedValue('prev')
    const { useLogStore } = await import('@/stores/log'); const s = useLogStore()
    const p = s.load(); expect(s.loading).toBe(true); await p
    expect(bridge.readPrevLog).not.toHaveBeenCalled()
    const r = s.load({ force: true }); expect(s.loading).toBe(false); await r
    s.tab = 'previous'; await s.load(); await s.load()
    expect(bridge.readPrevLog).toHaveBeenCalledTimes(1)
  })
})
describe('dns store', () => {
  it('toggle not blocked by --status; save returns once the daemon settles', async () => {
    bridge.getDnsStatus.mockImplementation(async () => { await sleep(300); return { running: true, network: 'cellular', wifi_host: 'a.b.io', mobile_host: 'cf.example.com', current: { mode: 'hostname', host: 'CF.example.com' } } })
    bridge.writeConfig.mockResolvedValue(); bridge.startDnsDaemon.mockResolvedValue()
    const { useDnsStore } = await import('@/stores/dns'); const s = useDnsStore()
    const t0 = Date.now(); await s.load(); expect(Date.now() - t0).toBeLessThan(200)
    const t1 = Date.now(); await s.save(true); expect(Date.now() - t1).toBeLessThan(900)
  })
  it('validation failure throws before touching anything', async () => {
    const { useDnsStore } = await import('@/stores/dns'); const s = useDnsStore(); s.wifiHost = ''
    await expect(s.save(true)).rejects.toThrow(/NextDNS host/); expect(bridge.writeConfig).not.toHaveBeenCalled()
  })
})
