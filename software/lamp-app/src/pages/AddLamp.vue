<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import type { Ref } from 'vue'
import { useRouter } from 'vue-router'
import { startScan } from '@/services/scan'
import type { NearbyLamp } from '@/services/scan'
import { useLampInventoryStore } from '@/stores/lampInventory'

const router = useRouter()
const inventory = useLampInventoryStore()

// Will be swapped to the ref returned by startScan so mutations in scan.ts are reflected.
const lamps: Ref<NearbyLamp[]> = ref<NearbyLamp[]>([])
const scanning = ref(true)
let stopScan: (() => void) | null = null

onMounted(async () => {
  const result = await startScan()
  // Adopt the scan service's reactive ref so in-place pushes stay visible to the template.
  lamps.value = result.lamps.value
  stopScan = result.stop
  scanning.value = false
})

onUnmounted(() => {
  stopScan?.()
})

async function selectLamp(lamp: NearbyLamp) {
  if (!lamp.ip) {
    alert('Setting up new lamps will be available in a future update.')
    return
  }

  const password = window.prompt(`Password for "${lamp.name}" (leave blank if none):`, '') ?? ''
  const baseUrl = `http://${lamp.ip}`

  try {
    const headers: Record<string, string> = {}
    if (password) headers['X-Password'] = password

    const res = await fetch(`${baseUrl}/settings`, { headers })
    if (!res.ok) throw new Error(`HTTP ${res.status}`)

    inventory.add({
      id: lamp.id,
      name: lamp.name,
      lastIp: lamp.ip,
      password: password || undefined,
      lastSeen: Date.now(),
    })

    await router.push({ name: 'lamp-home', params: { id: lamp.id } })
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    alert(`Couldn't verify lamp: ${message}`)
  }
}
</script>

<template>
  <main class="add-lamp">
    <header class="add-lamp-header">
      <button class="back-btn" @click="router.push({ name: 'lamps' })">← Back</button>
      <h1>Add a Lamp</h1>
    </header>

    <p v-if="scanning || !lamps.length" class="scanning-hint">Scanning for lamps…</p>

    <ul v-if="lamps.length" class="lamp-list">
      <li
        v-for="lamp in lamps"
        :key="lamp.id"
        class="lamp-row"
        @click="selectLamp(lamp)"
      >
        <div class="lamp-info">
          <span class="lamp-name">{{ lamp.name }}</span>
          <span class="lamp-hint">
            <template v-if="lamp.viaMdns">On your WiFi · {{ lamp.ip }}</template>
            <template v-else>Nearby</template>
          </span>
        </div>
        <span class="lamp-chevron">›</span>
      </li>
    </ul>
  </main>
</template>

<style scoped>
.add-lamp {
  padding: 24px;
}

.add-lamp-header {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-bottom: 24px;
}

.add-lamp-header h1 {
  margin: 0;
  font-size: 1.4rem;
}

.back-btn {
  background: none;
  border: none;
  color: inherit;
  font-size: 1rem;
  cursor: pointer;
  padding: 4px 8px;
  border-radius: 6px;
}

.back-btn:active {
  background: rgba(255, 255, 255, 0.1);
}

.scanning-hint {
  color: #999;
  text-align: center;
  padding: 48px 0;
}

.lamp-list {
  list-style: none;
  padding: 0;
  margin: 0;
}

.lamp-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 16px;
  border-radius: 10px;
  cursor: pointer;
  border-bottom: 1px solid rgba(255, 255, 255, 0.07);
}

.lamp-row:active {
  background: rgba(255, 255, 255, 0.06);
}

.lamp-info {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.lamp-name {
  font-weight: 600;
  font-size: 1rem;
}

.lamp-hint {
  color: #999;
  font-size: 0.82rem;
}

.lamp-chevron {
  color: #666;
  font-size: 1.3rem;
}
</style>
