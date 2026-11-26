import './assets/main.css'

import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import router from './router'
import { primeConfigs } from './helpers/bridge'

// Start the single root-shell read of all configs NOW — before Vue even
// builds the component tree — so it overlaps with startup instead of
// following it. Every store read waits on this one promise.
primeConfigs()

const app = createApp(App)
app.use(createPinia())
app.use(router)

router.isReady().then(() => {
  app.mount('#app')
})
