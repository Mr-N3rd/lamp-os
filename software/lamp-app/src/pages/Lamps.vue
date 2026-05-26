<script setup lang="ts">
import { onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useLampInventoryStore } from '@/stores/lampInventory'

const router = useRouter()
const inventory = useLampInventoryStore()

onMounted(() => {
  void inventory.load()
})

const openLamp = (id: string) => {
  router.push({ name: 'lamp-home', params: { id } })
}

const addLamp = () => {
  router.push({ name: 'add-lamp' })
}

const isOnline = (lamp: { lastSeen?: number }) => {
  if (!lamp.lastSeen) return false
  return Date.now() - lamp.lastSeen < 30_000
}
</script>

<template>
  <main class="lamps-page">
    <header>
      <h1>Your Lamps</h1>
      <button class="add-btn" @click="addLamp">+ Add a lamp</button>
    </header>

    <ul v-if="inventory.lamps.length" class="lamp-list">
      <li
        v-for="lamp in inventory.lamps"
        :key="lamp.id"
        class="lamp-row"
        @click="openLamp(lamp.id)"
      >
        <span class="status-dot" :class="{ online: isOnline(lamp) }"></span>
        <span class="lamp-name">{{ lamp.name }}</span>
        <span class="lamp-ip">{{ lamp.lastIp }}</span>
      </li>
    </ul>

    <p v-else class="empty">No lamps yet. Tap "Add a lamp" to get started.</p>
  </main>
</template>

<style scoped>
.lamps-page { padding: 24px; }
header { display: flex; justify-content: space-between; align-items: center; }
.add-btn { padding: 8px 16px; border-radius: 8px; }
.lamp-list { list-style: none; padding: 0; }
.lamp-row { display: flex; align-items: center; gap: 12px; padding: 16px; border-radius: 8px; }
.lamp-row:active { background: rgba(255,255,255,0.05); }
.status-dot { width: 10px; height: 10px; border-radius: 50%; background: #555; }
.status-dot.online { background: #4ade80; }
.lamp-name { flex: 1; font-weight: 600; }
.lamp-ip { color: #999; font-size: 0.85rem; }
.empty { color: #999; }
</style>
