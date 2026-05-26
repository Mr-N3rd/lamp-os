import { createRouter, createWebHistory } from 'vue-router'

const LampsPage = () => import('@/pages/Lamps.vue')
const AddLampPage = () => import('@/pages/AddLamp.vue')
const LampLayout = () => import('@/layout/Lamp.vue')
const IndexPage = () => import('@/pages/Index.vue')
const ExpressionsPage = () => import('@/pages/Expressions.vue')
const SetupPage = () => import('@/pages/Setup.vue')
const InfoPage = () => import('@/pages/Info.vue')

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', name: 'lamps', component: LampsPage },
    { path: '/add', name: 'add-lamp', component: AddLampPage },
    {
      path: '/lamp/:id',
      component: LampLayout,
      children: [
        { path: '', name: 'lamp-home', component: IndexPage },
        { path: 'expressions', name: 'lamp-expressions', component: ExpressionsPage },
        { path: 'setup', name: 'lamp-lamp-setup', component: SetupPage },
        { path: 'info', name: 'lamp-info', component: InfoPage },
      ],
    },
  ],
})

export default router
