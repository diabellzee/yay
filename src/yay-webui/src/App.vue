<template>
  <div id="app" class="h-screen flex flex-col bg-background text-on-surface overflow-hidden">
    <main class="main-content flex-1 min-h-0 md:ml-20 overflow-hidden relative">
      <router-view v-slot="{ Component, route }">
        <!-- Every destination (tabs and Settings) is a sibling: fade-through. -->
        <transition name="tab">
          <keep-alive>
            <component :is="Component" :key="route.name" />
          </keep-alive>
        </transition>
      </router-view>
    </main>
    <Navigation />
  </div>
</template>

<script setup>
import Navigation from '@/components/ui/Navigation.vue'
</script>
