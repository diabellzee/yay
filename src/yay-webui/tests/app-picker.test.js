// @vitest-environment jsdom
import { describe, it, expect, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'

vi.mock('webuix', () => ({ Intent: class {}, WebUI: class {}, WXEventHandler: class {} }))
vi.mock('kernelsu', () => ({ exec: async () => ({ errno: 0, stdout: '', stderr: '' }), toast: () => {} }))
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

describe('AppPicker', () => {
  it('lists apps, ranks chosen ones first, filters with a debounce, emits toggles, shows stale picks', async () => {
    const { default: AppPicker } = await import('@/components/ui/AppPicker.vue')
    const w = mount(AppPicker, {
      props: { show: true, title: 'Boost apps', selected: ['com.whatsapp', 'gone.app'], other: ['com.spotify.music'], otherLabel: 'isolated' },
      global: { stubs: { Teleport: true, Transition: false } }, attachTo: document.body,
    })
    await flushPromises(); await sleep(30); await flushPromises()

    const pkgs = () => [...document.querySelectorAll('.lazy-row p:nth-child(2)')].map((p) => p.textContent.split('·')[0].trim())
    // chosen apps first (whatsapp, and the uninstalled one so it can still be removed), then A-Z
    expect(pkgs().slice(0, 2).sort()).toEqual(['com.whatsapp', 'gone.app'])
    expect(pkgs()).toContain('com.mobile.legends')
    expect(document.body.textContent).toContain('isolated')        // sibling-list hint

    // search is debounced (200ms): not applied instantly, applied soon after
    const input = document.querySelector('input[placeholder="Search apps…"]')
    input.value = 'chrome'; input.dispatchEvent(new Event('input'))
    await sleep(20); expect(pkgs().length).toBeGreaterThan(2)
    await sleep(260); await flushPromises()
    expect(pkgs()).toEqual(['com.android.chrome'])

    // toggling a row emits (pkg, on)
    const box = document.querySelector('.lazy-row input[type=checkbox]')
    box.checked = true; box.dispatchEvent(new Event('change'))
    expect(w.emitted('toggle')[0]).toEqual(['com.android.chrome', true])

    // System tab shows system packages
    const tabs = [...document.querySelectorAll('button')].filter((b) => b.textContent.trim() === 'System apps')
    tabs[0].click(); input.value = ''; input.dispatchEvent(new Event('input'))
    await sleep(300); await flushPromises()
    expect(pkgs()).toContain('com.android.settings')
    w.unmount()
  })
})
