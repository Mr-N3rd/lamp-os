<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useLampInventoryStore } from '@/stores/lampInventory'
import { initBle, scanForLamps } from '@/services/ble'
import type { DiscoveredLamp } from '@/services/ble'

const router = useRouter()
const inventory = useLampInventoryStore()
const connecting = ref(true)

onMounted(async () => {
  await inventory.load()

  if (!inventory.lamps.length) {
    // No lamps in inventory — go straight to the list so user can add one
    connecting.value = false
    await router.replace({ name: 'lamps' })
    return
  }

  // BLE-scan for ~3 s to find the strongest-RSSI known lamp
  let bestLamp: DiscoveredLamp | null = null
  let bestRssi = -Infinity
  const knownIds = new Set(inventory.lamps.map((l) => l.id))

  try {
    await initBle()
    await new Promise<void>((resolve) => {
      const scanDurationMs = 3_000
      void scanForLamps(
        (lamp) => {
          if (!knownIds.has(lamp.id)) return
          inventory.updateSeen(lamp.id)
          const rssi = lamp.rssi ?? -100
          if (rssi > bestRssi) {
            bestRssi = rssi
            bestLamp = lamp
          }
        },
        scanDurationMs,
      )
      setTimeout(resolve, scanDurationMs + 200)
    })
  } catch (err) {
    console.warn('[App] bootstrap BLE scan failed:', err)
  }

  connecting.value = false

  if (bestLamp) {
    await router.replace({ name: 'lamp-home', params: { id: (bestLamp as DiscoveredLamp).id } })
  } else {
    await router.replace({ name: 'lamps' })
  }
})
</script>

<template>
  <div v-if="connecting" class="bootstrap-overlay">
    <div class="bootstrap-spinner"></div>
    <p class="bootstrap-text">Connecting…</p>
  </div>
  <router-view v-else />
</template>

<style scoped>
.bootstrap-overlay {
  min-height: 100vh;
  background: var(--brand-midnight-black);
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 16px;
}

.bootstrap-spinner {
  width: 40px;
  height: 40px;
  border: 3px solid rgba(255, 255, 255, 0.1);
  border-top-color: var(--brand-aurora-blue);
  border-radius: 50%;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.bootstrap-text {
  color: var(--brand-fog-grey);
  font-size: 0.95rem;
  margin: 0;
}
</style>
