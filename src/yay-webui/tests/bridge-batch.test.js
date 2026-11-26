import { describe, it, expect, vi, beforeAll } from 'vitest'
import { execFile } from 'node:child_process'
import { mkdirSync, writeFileSync, mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

// Emulate KernelSU: window.ksu exists, and exec() runs the command in a REAL
// `sh` — so the generated batch script is tested for real, not mocked.
// Every command the bridge builds hard-codes /data/adb/...; the mock exec
// redirects that prefix into a throw-away directory, so the tests exercise the
// real generated shell script on real files without ever touching (or needing)
// a rooted device.
const calls = vi.hoisted(() => ({ n: 0, cmds: [], root: '' }))
vi.mock('kernelsu', () => ({
  toast: () => {},
  exec: (cmd) => new Promise((res) => {
    calls.n++; calls.cmds.push(cmd)
    execFile('sh', ['-c', cmd.split('/data/adb').join(calls.root)], { maxBuffer: 1 << 26 }, (err, stdout, stderr) =>
      res({ errno: err ? (err.code ?? 1) : 0, stdout, stderr }))
  }),
}))
vi.mock('webuix', () => ({ Intent: class {}, WebUI: class {}, WXEventHandler: class {} }))
globalThis.window = { ksu: {} }

const RULES = { version: 1, components: [{ package: 'a.b', component: 'x.Y', note: `quote " ' \\ ünïcode 🎮`, enabled: true }], appops: [] }

let CFG
beforeAll(() => {
  calls.root = mkdtempSync(join(tmpdir(), 'yay-webui-'))
  CFG = `${calls.root}/yay/config`
  mkdirSync(CFG, { recursive: true })
  writeFileSync(`${CFG}/rules.json`, JSON.stringify(RULES))
  writeFileSync(`${CFG}/io_config.json`, '')              // present but EMPTY
  writeFileSync(`${CFG}/dns_config.json`, '{"enabled":true,"wifi_host":"a.b.io"}')
  // game_config.json deliberately MISSING; watcher flag only at the LEGACY path
  mkdirSync(`${calls.root}/modules/yay`, { recursive: true })
  writeFileSync(`${calls.root}/modules/yay/enable_watch`, '')
})

describe('config snapshot over a real shell', () => {
  it('one exec primes everything; every later read is free', async () => {
    const b = await import('@/helpers/bridge')
    const t0 = calls.n
    const p = b.primeConfigs()
    expect(b.primeConfigs()).toBe(p)                      // idempotent
    await p
    expect(calls.n - t0).toBe(1)

    // all of these arrive while/after priming and must not spawn anything
    const rules = await b.readConfig('rules.json')
    expect(rules).toEqual(RULES)                          // quotes/unicode/backslash survive base64
    await expect(b.readConfig('game_config.json')).rejects.toThrow(/not found/)
    await expect(b.readConfig('io_config.json')).rejects.toThrow()   // empty file is not valid JSON
    expect((await b.readConfig('dns_config.json')).wifi_host).toBe('a.b.io')
    expect(await b.isWatcherEnabled()).toBe(true)         // legacy flag location honoured
    const snap = await b.readSnapshot(['rules.json', 'game_config.json', 'io_config.json'])
    expect(snap.configs['game_config.json']).toBeNull()
    expect(snap.watcher).toBe(true)
    expect(calls.n - t0).toBe(1)                          // STILL one exec in total
  })

  it('writeConfig updates the cache; fresh snapshot is exactly one more exec', async () => {
    const b = await import('@/helpers/bridge')
    const before = calls.n
    await b.writeConfig('game_config.json', { games: [{ package: 'g.h' }] })
    expect(calls.n - before).toBe(1)                      // the write itself
    expect((await b.readConfig('game_config.json')).games[0].package).toBe('g.h')
    expect(calls.n - before).toBe(1)                      // read-after-write: no exec
    const fresh = await b.readSnapshot(['rules.json', 'game_config.json'], { fresh: true })
    expect(calls.n - before).toBe(2)                      // ONE batch, not one per file
    expect(fresh.configs['game_config.json'].games[0].package).toBe('g.h')   // disk really has it
  })

  it('watcher toggle: new flag path, removes both, cache follows', async () => {
    const b = await import('@/helpers/bridge')
    await b.setWatcherEnabled(false)
    const gone = await b.readSnapshot(['rules.json'], { fresh: true })
    expect(gone.watcher).toBe(false)                      // legacy flag really removed
    await b.setWatcherEnabled(true)
    const on = await b.readSnapshot(['rules.json'], { fresh: true })
    expect(on.watcher).toBe(true)
    expect(calls.cmds.some((c) => c.includes('/data/adb/yay/enable_watch'))).toBe(true)
  })
})
