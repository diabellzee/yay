<template>
  <!-- Placeholder shaped like the content that is about to appear, so the
       page doesn't jump when data lands (a centred spinner does). -->
  <div class="bg-surface-container rounded-2xl overflow-hidden divide-y divide-outline-variant/15"
    aria-hidden="true">
    <div v-for="i in rows" :key="i" class="flex items-center gap-3 px-4 py-3.5">
      <div class="skeleton w-9 h-9 !rounded-full shrink-0" :style="{ animationDelay: `${i * 90}ms` }" />
      <div class="flex-1 space-y-2">
        <div class="skeleton h-3.5" :style="{ width: `${52 + ((i * 17) % 34)}%`, animationDelay: `${i * 90}ms` }" />
        <div class="skeleton h-3 w-2/5" :style="{ animationDelay: `${i * 90 + 45}ms` }" />
      </div>
    </div>
  </div>
</template>
<script setup>
defineProps({ rows: { type: Number, default: 4 } })
</script>
