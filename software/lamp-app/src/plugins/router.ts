import { createRouter, createWebHistory } from 'vue-router'

const LampsPage = () => import('@/pages/Lamps.vue')

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', name: 'lamps', component: LampsPage },
  ],
})

export default router
