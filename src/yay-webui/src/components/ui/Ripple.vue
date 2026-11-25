<template>
  <div
    class="ripple-wrapper"
    :class="{ 'non-touch': !IS_TOUCH }"
    :tabindex="tabindex"
    @pointerdown="onDown"
    @keydown="onKey"
    ref="el"
  >
    <slot />
  </div>
</template>

<script>
// Evaluated once per page load, not once per Ripple instance.
const IS_TOUCH =
  typeof window !== 'undefined' &&
  ('ontouchstart' in window || navigator.maxTouchPoints > 0)
</script>

<script setup>
import { ref } from 'vue'

defineProps({ tabindex: { default: '0' } })
const el = ref(null)

// Colours (hover tint + ripple) come from CSS `color-mix(currentColor …)`,
// so mounting a Ripple costs no getComputedStyle / forced style recalc —
// which previously ran once per instance (one per Games row) and again on
// every tap.
function onDown(e) {
  const node = el.value
  const r = node.getBoundingClientRect()
  const size = Math.max(r.width, r.height)
  const dur = Math.min(0.8, Math.max(0.2, 0.2 + (r.width / 800) * 0.3))
  const span = document.createElement('span')
  span.className = 'ripple'
  span.style.cssText =
    `width:${size}px;height:${size}px;` +
    `left:${e.clientX - r.left - size / 2}px;top:${e.clientY - r.top - size / 2}px;` +
    `animation-duration:${dur}s;transition:opacity ${dur}s ease;`
  node.appendChild(span)

  const end = () => {
    span.classList.add('end')
    setTimeout(() => span.remove(), dur * 1000)
    document.removeEventListener('pointerup', end)
    document.removeEventListener('pointercancel', end)
  }
  document.addEventListener('pointerup', end)
  document.addEventListener('pointercancel', end)
}

function onKey(e) {
  if (e.key === 'Enter' || e.key === ' ') {
    e.preventDefault()
    el.value.click()
  }
}
</script>
