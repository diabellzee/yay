import { readFileSync } from 'node:fs'
import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import tailwindcss from '@tailwindcss/vite'
import { ViteMinifyPlugin } from 'vite-plugin-minify'

// Single source of truth for the displayed version: the module's own
// module.prop (repo root = two levels above this project).
function moduleVersion() {
  try {
    const prop = readFileSync(fileURLToPath(new URL('../../module.prop', import.meta.url)), 'utf8')
    return /^version=v?(.+)$/m.exec(prop)?.[1].trim() ?? 'dev'
  } catch {
    return 'dev'
  }
}

export default defineConfig({
  plugins: [vue(), tailwindcss(), ViteMinifyPlugin({})],
  define: {
    __YAY_VERSION__: JSON.stringify(moduleVersion()),
  },
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  build: {
    outDir: 'dist',
    assetsInlineLimit: 4096,
    // Tailwind v4 already requires Chrome 111+ WebViews (color-mix, @property,
    // nesting). Targeting it stops esbuild from down-levelling modern JS/CSS
    // for browsers that cannot run this UI anyway.
    target: 'chrome111',
    // Few files > many: on KernelSU each asset is a root file read.
    cssCodeSplit: false,
    modulePreload: { polyfill: false },
    rollupOptions: { output: { inlineDynamicImports: true } },
  },
})
