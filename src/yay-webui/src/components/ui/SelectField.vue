<template>
  <div>
    <Ripple class="flex items-center justify-between gap-3 px-4 py-3.5"
      :class="disabled ? 'opacity-40 pointer-events-none' : 'cursor-pointer'" @click="open = true">
      <div class="min-w-0 flex-1">
        <p class="text-sm font-medium text-on-surface">{{ label }}</p>
        <p v-if="description" class="text-xs text-on-surface-variant mt-0.5">{{ description }}</p>
      </div>
      <span class="shrink-0 max-w-[45%] truncate text-sm text-primary">{{ currentLabel }}</span>
    </Ripple>

    <Modal :show="open" :title="label" @close="open = false">
      <div class="pb-2">
        <Ripple v-for="o in options" :key="o.value" @click="pick(o.value)"
          class="flex items-center gap-3 px-4 py-3 rounded-xl cursor-pointer">
          <div class="min-w-0 flex-1">
            <p class="text-sm text-on-surface break-words">{{ o.label }}</p>
            <p v-if="o.hint" class="text-xs text-on-surface-variant mt-0.5">{{ o.hint }}</p>
          </div>
          <svg v-if="o.value === modelValue" class="shrink-0 text-primary" width="22" height="22" viewBox="0 0 24 24" fill="currentColor">
            <path d="M9.55 18L3.85 12.3L5.275 10.875L9.55 15.15L18.725 5.975L20.15 7.4L9.55 18Z" />
          </svg>
        </Ripple>
      </div>
      <template #actions>
        <button @click="open = false" class="px-4 h-10 rounded-full text-sm font-medium text-primary">Close</button>
      </template>
    </Modal>
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
import Ripple from '@/components/ui/Ripple.vue'
import Modal from '@/components/ui/Modal.vue'

const props = defineProps({
  label: String,
  description: String,
  modelValue: { type: String, default: '' },
  options: { type: Array, required: true }, // [{ value, label, hint? }]
  disabled: Boolean,
})
const emit = defineEmits(['update:modelValue'])

const open = ref(false)
const currentLabel = computed(
  () => props.options.find((o) => o.value === props.modelValue)?.label ?? (props.modelValue || '—'),
)

function pick(value) {
  open.value = false
  if (value !== props.modelValue) emit('update:modelValue', value)
}
</script>
