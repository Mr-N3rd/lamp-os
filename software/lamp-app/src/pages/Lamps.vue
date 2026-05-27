<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import { useLampInventoryStore } from '@/stores/lampInventory'
import { initBle, scanForLamps } from '@/services/ble'
import { BleClient } from '@capacitor-community/bluetooth-le'

const router = useRouter()
const inventory = useLampInventoryStore()

async function pollOnce() {
  const knownIds = new Set(inventory.lamps.map((l) => l.id))
  try {
    await initBle()
    void scanForLamps((discovered) => {
      if (!knownIds.has(discovered.id)) return
      inventory.updateSeen(discovered.id, {
        base: discovered.baseColor,
        shade: discovered.shadeColor,
      })
    }, 1_500)
  } catch (err) {
    console.warn('BLE scan failed:', err)
  }
}

const onVisibilityChange = () => {
  if (document.visibilityState === 'visible') void pollOnce()
}

onMounted(async () => {
  await inventory.load()
  void pollOnce()
  document.addEventListener('visibilitychange', onVisibilityChange)
})

onUnmounted(() => {
  document.removeEventListener('visibilitychange', onVisibilityChange)
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

const swatchStyle = (color?: [number, number, number]) => {
  if (!color) return { background: 'var(--brand-slate-grey)' }
  return { background: `rgb(${color[0]}, ${color[1]}, ${color[2]})` }
}
</script>

<template>
  <div class="lamps-page">
    <div class="container">
      <div class="main-content">
        <header class="page-header">
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
            <div class="lamp-info">
              <span class="lamp-name">{{ lamp.name }}</span>
            </div>
            <div class="lamp-colors" :class="{ offline: !isOnline(lamp) }">
              <span class="swatch" :style="swatchStyle(lamp.lastShadeColor)"></span>
              <span class="swatch" :style="swatchStyle(lamp.lastBaseColor)"></span>
            </div>
            <span class="lamp-chevron">›</span>
          </li>
        </ul>

        <p v-else class="empty">No lamps yet. Tap "Add a lamp" to get started.</p>
      </div>
    </div>
  </div>
</template>

<style scoped>
.lamps-page {
  min-height: 100vh;
  background: var(--brand-midnight-black);
  padding: 0;
  width: 100%;
}

.container {
  width: 100%;
  max-width: 100%;
  margin: 0 auto;
}

.main-content {
  background: var(--color-background-soft);
  padding: 16px;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
  backdrop-filter: blur(10px);
}

/* Header */
.page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 24px;
}

.page-header h1 {
  color: var(--brand-lamp-white);
  font-size: 1rem;
  font-weight: 800;
}

/* Add button — gradient style matching floating save button */
.add-btn {
  padding: 12px 24px;
  border: none;
  border-radius: 50px;
  font-size: 0.9rem;
  font-weight: 600;
  cursor: pointer;
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-glow-pink));
  color: var(--brand-lamp-white);
  transition: all 0.3s ease;
  box-shadow:
    0 4px 16px rgba(0, 0, 0, 0.3),
    0 0 0 1px rgba(255, 255, 255, 0.1);
  font-family: inherit;
}

.add-btn:hover {
  transform: translateY(-2px);
  box-shadow:
    0 8px 24px rgba(0, 0, 0, 0.4),
    0 0 20px rgba(68, 108, 156, 0.4),
    0 0 0 1px rgba(253, 253, 253, 0.2);
}

.add-btn:active {
  transform: translateY(0);
}

/* Lamp list */
.lamp-list {
  list-style: none;
  padding: 0;
  margin: 0;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.lamp-row {
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 20px;
  background: var(--color-background-mute);
  border-radius: 16px;
  cursor: pointer;
  transition: background 0.2s ease;
  border: 1px solid var(--color-border);
}

.lamp-row:active {
  background: rgba(253, 253, 253, 0.06);
}

/* Status dot */
.status-dot {
  flex-shrink: 0;
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: var(--brand-slate-grey);
  transition: all 0.3s ease;
}

.status-dot.online {
  background: var(--color-success);
  box-shadow: 0 0 8px rgba(141, 205, 166, 0.6);
  animation: pulse 2s ease-in-out infinite;
}

@keyframes pulse {
  0%, 100% { box-shadow: 0 0 6px rgba(141, 205, 166, 0.5); }
  50% { box-shadow: 0 0 12px rgba(141, 205, 166, 0.8); }
}

/* Lamp info */
.lamp-info {
  display: flex;
  flex-direction: column;
  gap: 3px;
  flex: 1;
}

.lamp-name {
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-size: 1rem;
}

.lamp-colors {
  display: flex;
  gap: 4px;
  flex-shrink: 0;
  opacity: 1;
  transition: opacity 0.3s ease;
}

.lamp-colors.offline {
  opacity: 0.35;
}

.swatch {
  width: 18px;
  height: 18px;
  border-radius: 3px;
  border: 1px solid rgba(253, 253, 253, 0.15);
  box-shadow: inset 0 0 4px rgba(0, 0, 0, 0.3);
  transition: background 0.3s ease;
}

.lamp-chevron {
  color: var(--brand-slate-grey);
  font-size: 1.3rem;
  flex-shrink: 0;
}

/* Empty state */
.empty {
  color: var(--color-text-secondary);
  text-align: center;
  padding: 48px 0;
  font-size: 0.95rem;
}

</style>
