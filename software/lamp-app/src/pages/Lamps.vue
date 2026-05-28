<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import { useLampInventoryStore } from '@/stores/lampInventory'
import { useLampStore } from '@/stores/lamp'
import { initBle, scanForLamps } from '@/services/ble'
import { BleClient } from '@capacitor-community/bluetooth-le'
import LampIcon from '@/components/LampIcon.vue'

const router = useRouter()
const inventory = useLampInventoryStore()
// nearbyLamps is populated whenever the user is (or recently was) BLE-paired
// to a lamp. Pinia keeps it across navigation, so after one connect the
// gallery can flag which inventory lamps are reachable + surface lamps not
// yet in inventory.
const lampStore = useLampStore()

// Per-name lookup so we can badge inventory rows with via* flags.
const nearbyByName = computed(() => {
  const m = new Map<string, { viaBle: boolean; viaEspNow: boolean }>()
  for (const l of lampStore.nearbyLamps) {
    m.set(l.name, { viaBle: l.viaBle, viaEspNow: l.viaEspNow })
  }
  return m
})

const isInGrid = (name: string) => !!nearbyByName.value.get(name)?.viaEspNow

// Lamps reachable nearby that aren't in inventory yet — surfaces undiscovered
// peers with the same badges as inventory rows.
const unknownNearby = computed(() => {
  const inv = new Set(inventory.lamps.map((l) => l.name))
  return lampStore.nearbyLamps.filter((l) => !inv.has(l.name))
})

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

const statusDotClass = (lamp: { name: string; lastSeen?: number }) => {
  if (!isOnline(lamp)) return ''       // grey (default)
  if (isInGrid(lamp.name)) return 'status-dot--mesh'  // bright green
  return 'status-dot--bt'                                // dim green
}

const statusTitle = (lamp: { name: string; lastSeen?: number }) => {
  if (!isOnline(lamp)) return 'Offline'
  if (isInGrid(lamp.name)) return 'Online · on the mesh'
  return 'Online · legacy BLE only'
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
            <span
              class="status-dot"
              :class="statusDotClass(lamp)"
              :title="statusTitle(lamp)"
            ></span>
            <div class="lamp-info">
              <span class="lamp-name">{{ lamp.name }}</span>
            </div>
            <LampIcon
              :shade="lamp.lastShadeColor ?? [60, 60, 60]"
              :base="lamp.lastBaseColor ?? [60, 60, 60]"
              :size="26"
              :class="{ 'lamp-icon--offline': !isOnline(lamp) }"
            />
            <span class="lamp-chevron">›</span>
          </li>
        </ul>

        <p v-else class="empty">No lamps yet. Tap "Add a lamp" to get started.</p>

        <!-- Nearby lamps not in inventory — reachable via BLE adv or grid,
             but the user hasn't added them. Read-only here; full add flow
             is the existing Add Lamp page. -->
        <section v-if="unknownNearby.length" class="unknown-grid">
          <h2 class="unknown-grid-title">Other nearby lamps</h2>
          <ul class="lamp-list">
            <li
              v-for="lamp in unknownNearby"
              :key="lamp.mac || lamp.name"
              class="lamp-row lamp-row--unknown"
            >
              <span
                class="status-dot"
                :class="lamp.viaEspNow ? 'status-dot--mesh' : 'status-dot--bt'"
                :title="lamp.viaEspNow ? 'Online · on the mesh' : 'Online · legacy BLE only'"
              ></span>
              <div class="lamp-info">
                <span class="lamp-name">{{ lamp.name || '(unnamed)' }}</span>
              </div>
              <LampIcon :shade="lamp.shade" :base="lamp.base" :size="26" />
            </li>
          </ul>
        </section>
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

/* Status dot — three states:
   - default: grey (offline)
   - .status-dot--bt: dim green (online, legacy BLE only)
   - .status-dot--mesh: bright glowing green (online + reachable via mesh) */
.status-dot {
  flex-shrink: 0;
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: var(--brand-slate-grey);
  transition: all 0.3s ease;
}
.status-dot--bt {
  background: #4d6f53;
  box-shadow: 0 0 4px rgba(77, 111, 83, 0.4);
}
.status-dot--mesh {
  background: var(--color-success);
  box-shadow: 0 0 8px rgba(141, 205, 166, 0.6);
  animation: pulse 2s ease-in-out infinite;
}
@keyframes pulse {
  0%, 100% { box-shadow: 0 0 6px rgba(141, 205, 166, 0.5); }
  50%      { box-shadow: 0 0 12px rgba(141, 205, 166, 0.85); }
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

.lamp-icon--offline { opacity: 0.35; }

/* Unknown grid peers section */
.unknown-grid {
  margin-top: 24px;
}
.unknown-grid-title {
  color: var(--brand-fog-grey);
  font-size: 0.85rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  margin: 0 0 8px 4px;
}
.lamp-row--unknown {
  opacity: 0.9;
  cursor: default;
}
.lamp-row--unknown:active {
  background: var(--color-background-mute);
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
