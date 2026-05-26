<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import { useLampInventoryStore } from '@/stores/lampInventory'
import { initBle, scanForLamps } from '@/services/ble'
import { scanMdnsLamps } from '@/services/mdns'
import { BleClient } from '@capacitor-community/bluetooth-le'

const router = useRouter()
const inventory = useLampInventoryStore()

let pollTimer: number | undefined

async function pollOnce() {
  const knownNamesLowered = new Map(
    inventory.lamps.map((l) => [l.name.toLowerCase(), l.id]),
  )
  const knownIds = new Set(inventory.lamps.map((l) => l.id))

  try {
    await initBle()
    void scanForLamps((discovered) => {
      if (knownIds.has(discovered.id)) {
        inventory.updateSeen(discovered.id)
      }
    }, 8_000)
  } catch (err) {
    console.warn('BLE scan failed:', err)
  }

  try {
    void scanMdnsLamps((discovered) => {
      const id = knownNamesLowered.get(discovered.name.toLowerCase())
      if (id) inventory.updateSeen(id, discovered.ip)
    }, 5_000)
  } catch (err) {
    console.warn('mDNS scan failed:', err)
  }
}

onMounted(async () => {
  await inventory.load()
  await pollOnce()
  pollTimer = window.setInterval(() => { void pollOnce() }, 30_000)
})

onUnmounted(() => {
  if (pollTimer) clearInterval(pollTimer)
  try { void BleClient.stopLEScan() } catch { /* not running or unavailable */ }
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
