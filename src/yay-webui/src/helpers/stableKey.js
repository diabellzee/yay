import { toRaw } from 'vue'

const ids = new WeakMap()
let next = 0

/**
 * A key that follows the *object*, not its position. With `:key="index"`
 * Vue reuses the DOM of row N for whatever item lands at N after a filter
 * or delete — so toggles visibly animate between two different items and
 * every row below a deleted one gets patched. Identity keys remove both.
 */
export function stableKey(obj) {
  const raw = toRaw(obj)
  let id = ids.get(raw)
  if (id === undefined) {
    id = ++next
    ids.set(raw, id)
  }
  return id
}
