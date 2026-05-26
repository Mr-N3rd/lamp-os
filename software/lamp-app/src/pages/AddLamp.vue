<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import type { Ref } from 'vue'
import { useRouter } from 'vue-router'
import { startScan } from '@/services/scan'
import type { NearbyLamp } from '@/services/scan'
import { writeLampSetup } from '@/services/ble'
import { scanMdnsLamps } from '@/services/mdns'
import { useLampInventoryStore } from '@/stores/lampInventory'

const router = useRouter()
const inventory = useLampInventoryStore()

// ── Scan state ────────────────────────────────────────────────────────────────
const lamps: Ref<NearbyLamp[]> = ref<NearbyLamp[]>([])
const scanning = ref(true)
let stopScan: (() => void) | null = null

onMounted(async () => {
  const result = await startScan()
  lamps.value = result.lamps.value
  stopScan = result.stop
  scanning.value = false
})

onUnmounted(() => {
  stopScan?.()
})

// ── Case A wizard state ───────────────────────────────────────────────────────
const wizardLamp = ref<NearbyLamp | null>(null)
const wizardName = ref('')
const wizardSsid = ref('')
const wizardPassword = ref('')
const wizardStatus = ref<string | null>(null)
const wizardBusy = ref(false)

function openSetupWizard(lamp: NearbyLamp) {
  wizardLamp.value = lamp
  wizardName.value = lamp.name
  wizardSsid.value = ''
  wizardPassword.value = ''
  wizardStatus.value = null
  wizardBusy.value = false
}

function cancelWizard() {
  wizardLamp.value = null
  wizardStatus.value = null
  wizardBusy.value = false
}

async function waitForLampOnNetwork(name: string, timeoutMs = 60_000): Promise<string | null> {
  const lowered = name.toLowerCase()
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    let foundIp: string | null = null
    await new Promise<void>((resolve) => {
      let resolved = false
      void scanMdnsLamps((lamp) => {
        if (lamp.name.toLowerCase() === lowered && !foundIp) {
          foundIp = lamp.ip
        }
      }, 5_000)
      setTimeout(() => { if (!resolved) { resolved = true; resolve() } }, 5_500)
    })
    if (foundIp) return foundIp
  }
  return null
}

async function submitSetup() {
  const lamp = wizardLamp.value!
  const name = wizardName.value.trim()
  const ssid = wizardSsid.value.trim()

  if (!name || !ssid) {
    alert('Lamp name and WiFi network name are required.')
    return
  }

  wizardBusy.value = true

  try {
    wizardStatus.value = 'Connecting…'
    await writeLampSetup(lamp.id, {
      ssid,
      password: wizardPassword.value,
      name,
    })

    wizardStatus.value = 'Waiting for lamp to appear on home WiFi (up to 60s)…'
    const ip = await waitForLampOnNetwork(name)

    if (!ip) {
      wizardStatus.value = null
      wizardBusy.value = false
      alert("Lamp didn't appear on the network. Check your WiFi credentials and try again.")
      return
    }

    inventory.add({
      id: lamp.id,
      name,
      lastIp: ip,
      password: wizardPassword.value || undefined,
      lastSeen: Date.now(),
    })

    await router.push({ name: 'lamp-home', params: { id: lamp.id } })
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    wizardStatus.value = null
    wizardBusy.value = false
    alert(`Setup failed: ${message}`)
  }
}

// ── Case B (lamp already on WiFi) ─────────────────────────────────────────────
async function selectLamp(lamp: NearbyLamp) {
  if (!lamp.ip) {
    // Case A — BLE-only, not yet on home WiFi
    openSetupWizard(lamp)
    return
  }

  // Case B — already on home WiFi, just verify and add
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
    <!-- ── Case A Wizard overlay ──────────────────────────────────────────── -->
    <div v-if="wizardLamp" class="wizard-overlay">
      <header class="add-lamp-header">
        <button class="back-btn" :disabled="wizardBusy" @click="cancelWizard">← Back</button>
        <h1>Set Up Lamp</h1>
      </header>

      <div v-if="wizardStatus" class="wizard-status">{{ wizardStatus }}</div>

      <form v-else class="wizard-form" @submit.prevent="submitSetup">
        <label class="field-label">
          Lamp name
          <input
            v-model="wizardName"
            class="field-input"
            type="text"
            placeholder="My Lamp"
            autocomplete="off"
            :disabled="wizardBusy"
          />
        </label>

        <label class="field-label">
          Home WiFi network (SSID)
          <input
            v-model="wizardSsid"
            class="field-input"
            type="text"
            placeholder="Network name"
            autocomplete="off"
            :disabled="wizardBusy"
          />
        </label>

        <label class="field-label">
          WiFi password
          <input
            v-model="wizardPassword"
            class="field-input"
            type="password"
            placeholder="Leave blank if open network"
            autocomplete="current-password"
            :disabled="wizardBusy"
          />
        </label>

        <button type="submit" class="primary-btn" :disabled="wizardBusy">
          Set up
        </button>
      </form>
    </div>

    <!-- ── Scan list ──────────────────────────────────────────────────────── -->
    <template v-else>
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
              <template v-else>Nearby · tap to set up</template>
            </span>
          </div>
          <span class="lamp-chevron">›</span>
        </li>
      </ul>
    </template>
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

.back-btn:disabled {
  opacity: 0.4;
  cursor: default;
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

/* ── Wizard ────────────────────────────────────────────────────────────────── */

.wizard-overlay {
  display: flex;
  flex-direction: column;
}

.wizard-form {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.field-label {
  display: flex;
  flex-direction: column;
  gap: 8px;
  font-size: 0.85rem;
  color: #aaa;
}

.field-input {
  background: rgba(255, 255, 255, 0.06);
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 8px;
  color: inherit;
  font-size: 1rem;
  padding: 12px 14px;
  outline: none;
}

.field-input:focus {
  border-color: rgba(255, 255, 255, 0.3);
}

.field-input:disabled {
  opacity: 0.5;
}

.primary-btn {
  margin-top: 8px;
  background: #fff;
  color: #111;
  border: none;
  border-radius: 10px;
  font-size: 1rem;
  font-weight: 600;
  padding: 14px;
  cursor: pointer;
}

.primary-btn:active {
  opacity: 0.85;
}

.primary-btn:disabled {
  opacity: 0.4;
  cursor: default;
}

.wizard-status {
  color: #aaa;
  font-size: 0.95rem;
  text-align: center;
  padding: 48px 0;
}
</style>
