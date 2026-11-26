import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vitest/config'
import vue from '@vitejs/plugin-vue'

// Separate from vite.config.js on purpose: tests don't need Tailwind or the
// HTML minifier, and the build-time __YAY_VERSION__ is pinned for assertions.
export default defineConfig({
  plugins: [vue()],
  define: { __YAY_VERSION__: JSON.stringify('9.9.9') },
  resolve: { alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) } },
  test: { environment: 'node', include: ['tests/**/*.test.js'], testTimeout: 20000 },
})
