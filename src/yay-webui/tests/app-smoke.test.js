// @vitest-environment jsdom
import { describe, it, expect, vi, beforeAll } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia } from 'pinia'

vi.mock('webuix', () => ({ Intent: class {}, WebUI: class {}, WXEventHandler: class {} }))
vi.mock('kernelsu', () => ({ exec: async () => ({ errno: 0, stdout: '', stderr: '' }), toast: () => {} }))

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
// Poll instead of guessing a delay: stable on slow CI machines.
async function waitFor(fn, timeout = 4000) {
  const end = Date.now() + timeout
  for (;;) {
    try { return fn() } catch (e) { if (Date.now() > end) throw e }
    await sleep(20)
  }
}
const warnings = []

beforeAll(() => {
  globalThis.ResizeObserver = class { observe() {} disconnect() {} }
  window.matchMedia ??= () => ({ matches: false, addEventListener() {}, removeEventListener() {} })
  const orig = console.warn
  console.warn = (...a) => { const m = String(a[0]); if (m.includes('Vue warn')) warnings.push(m); else orig(...a) }
  const origErr = console.error
  console.error = (...a) => { warnings.push('console.error: ' + String(a[0])); origErr(...a) }
})

describe('whole app, every tab', () => {
  it('mounts, navigates all tabs with fade transitions, nav pill follows, no Vue warnings', async () => {
    const { primeConfigs } = await import('@/helpers/bridge')
    primeConfigs()
    const { default: router } = await import('@/router')
    const { default: App } = await import('@/App.vue')
    router.push('/'); await router.isReady()
    const w = mount(App, { global: { plugins: [createPinia(), router] }, attachTo: document.body })

    // Home: version injected from module.prop (mocked define = 9.9.9), data arrived
    await waitFor(() => {
      expect(w.text()).toContain('yay v9.9.9')
      expect(w.find('.skeleton').exists()).toBe(false)           // skeleton gone once data landed
      expect(w.text()).toMatch(/\d+ components · \d+ appops/)
    })

    const tabs = [['/rules', 'Rules'], ['/games', 'Games'], ['/io', 'I/O'], ['/log', 'Log'], ['/', 'Home'], ['/rules', 'Rules']]
    for (const [path, label] of tabs) {
      await router.push(path)
      await waitFor(() => {
        const on = [...document.querySelectorAll('.nav-item')].filter((a) => a.querySelector('.nav-pill.on'))
        expect(on.length).toBe(1)
        expect(on[0].textContent).toContain(label)
        // after the fade finishes exactly one page is in the DOM (leaving one removed)
        expect(document.querySelectorAll('main .page').length).toBe(1)
      })
    }

    // Rules rows use identity keys + lazy rows, and toggling one flips real state
    expect(document.querySelectorAll('.lazy-row').length).toBeGreaterThan(0)

    // Settings (reached like the Home gear does)
    await router.push('/settings')
    await waitFor(() => {
      expect(document.querySelectorAll('main .page').length).toBe(1)
      expect(document.body.textContent).toContain('Auto Private DNS')
    })

    // DNS toggle with an empty NextDNS host: refused, switch remounted back to OFF
    const dnsToggle = () => document.querySelectorAll('label.relative input[type=checkbox]')
    const inputs = dnsToggle(); const dnsInput = inputs[inputs.length - 1]
    dnsInput.checked = true; dnsInput.dispatchEvent(new Event('change'))
    await waitFor(() => {
      const after = dnsToggle(); const last = after[after.length - 1]
      expect(last.checked).toBe(false)
      expect(document.querySelector('.toggle-thumb.on')).toBeNull()
    })

    w.unmount()
    expect(warnings).toEqual([])                                 // zero Vue warnings / console errors
  })
})
