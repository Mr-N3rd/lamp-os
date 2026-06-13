import './assets/main.css'

import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import router from './plugins/router'
import GlobalComponentsPlugin from './plugins/globalComponents'

const app = createApp(App)
app.use(createPinia())
app.use(router)
app.use(GlobalComponentsPlugin)
app.mount('#app')
