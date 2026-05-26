<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import type { Ref } from 'vue'
import { useRouter } from 'vue-router'
import { startScan } from '@/services/scan'
import type { NearbyLamp } from '@/services/scan'
import { writeLampSetup } from '@/services/ble'
import { scanMdnsLamps } from '@/services/mdns'
import { useLampInventoryStore } from '@/stores/lampInventory'
import ComponentForm from '@/components/Form.vue'
import type { FieldDefinition, FormValues } from '@/types'

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

// ── Error state ───────────────────────────────────────────────────────────────
const errorMessage = ref<string | null>(null)

function clearError() {
  errorMessage.value = null
}

// ── Case A wizard state ───────────────────────────────────────────────────────
const wizardLamp = ref<NearbyLamp | null>(null)
const wizardStatus = ref<string | null>(null)
const wizardBusy = ref(false)

// Form values for Case A wizard
const wizardFormValues = ref<FormValues>({
  ssid: '',
  password: '',
  name: '',
})

const setupFields = computed<FieldDefinition[]>(() => [
  { name: 'wifiHeading', type: 'group-heading', label: 'Home WiFi' },
  {
    name: 'ssid',
    type: 'text',
    label: 'Network name',
    default: '',
    props: { placeholder: 'WiFi SSID', maxLength: 32 },
  },
  {
    name: 'password',
    type: 'password',
    label: 'Network password',
    default: '',
    optional: true,
    props: { placeholder: 'WiFi password' },
  },
  { name: 'lampHeading', type: 'group-heading', label: 'Lamp Name' },
  {
    name: 'name',
    type: 'text',
    label: 'Lamp name',
    default: wizardLamp.value?.name ?? '',
    props: {
      placeholder: 'Name for this lamp',
      maxLength: 12,
      minLength: 3,
      pattern: '[a-z]+',
      transform: 'lowercase',
    },
  },
])

function handleWizardFormUpdate(values: FormValues) {
  wizardFormValues.value = { ...wizardFormValues.value, ...values }
}

function openSetupWizard(lamp: NearbyLamp) {
  wizardLamp.value = lamp
  wizardFormValues.value = {
    ssid: '',
    password: '',
    name: lamp.name,
  }
  wizardStatus.value = null
  wizardBusy.value = false
  errorMessage.value = null
}

function cancelWizard() {
  wizardLamp.value = null
  wizardStatus.value = null
  wizardBusy.value = false
  errorMessage.value = null
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
  const name = String(wizardFormValues.value.name ?? '').trim()
  const ssid = String(wizardFormValues.value.ssid ?? '').trim()

  if (!name || !ssid) {
    errorMessage.value = 'Lamp name and WiFi network name are required.'
    return
  }

  errorMessage.value = null
  wizardBusy.value = true

  try {
    wizardStatus.value = 'Connecting…'
    await writeLampSetup(lamp.id, {
      ssid,
      password: String(wizardFormValues.value.password ?? ''),
      name,
    })

    wizardStatus.value = 'Waiting for lamp to appear on home WiFi (up to 60s)…'
    const ip = await waitForLampOnNetwork(name)

    if (!ip) {
      wizardStatus.value = null
      wizardBusy.value = false
      errorMessage.value = "Lamp didn't appear on the network. Check your WiFi credentials and try again."
      return
    }

    inventory.add({
      id: lamp.id,
      name,
      lastIp: ip,
      password: String(wizardFormValues.value.password ?? '') || undefined,
      lastSeen: Date.now(),
    })

    await router.push({ name: 'lamp-home', params: { id: lamp.id } })
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    wizardStatus.value = null
    wizardBusy.value = false
    errorMessage.value = `Setup failed: ${message}`
  }
}

// ── Case B (lamp already on WiFi) ─────────────────────────────────────────────
const caseBLamp = ref<NearbyLamp | null>(null)
const caseBBusy = ref(false)

// Form values for Case B password panel
const caseBFormValues = ref<FormValues>({ caseBPassword: '' })

const caseBFields: FieldDefinition[] = [
  {
    name: 'caseBPassword',
    type: 'password',
    label: 'Lamp password',
    default: '',
    optional: true,
    props: { placeholder: 'Leave blank if none' },
  },
]

function handleCaseBFormUpdate(values: FormValues) {
  caseBFormValues.value = { ...caseBFormValues.value, ...values }
}

function cancelCaseB() {
  caseBLamp.value = null
  caseBBusy.value = false
  errorMessage.value = null
}

async function connectCaseB() {
  const lamp = caseBLamp.value!
  const password = String(caseBFormValues.value.caseBPassword ?? '')
  const baseUrl = `http://${lamp.ip}`

  errorMessage.value = null
  caseBBusy.value = true

  try {
    const headers: Record<string, string> = {}
    if (password) headers['X-Password'] = password

    const res = await fetch(`${baseUrl}/settings`, { headers })
    if (!res.ok) throw new Error(`HTTP ${res.status}`)

    inventory.add({
      id: lamp.id,
      name: lamp.name,
      lastIp: lamp.ip!,
      password: password || undefined,
      lastSeen: Date.now(),
    })

    await router.push({ name: 'lamp-home', params: { id: lamp.id } })
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    caseBBusy.value = false
    errorMessage.value = `Couldn't verify lamp: ${message}`
  }
}

async function selectLamp(lamp: NearbyLamp) {
  errorMessage.value = null

  // Case A — unconfigured lamp advertising setup GATT (regardless of whether
  // we coincidentally have a stale mDNS hit for it)
  if (lamp.isUnconfigured) {
    openSetupWizard(lamp)
    return
  }

  // Case B — known/configured lamp. Needs an IP to reach via HTTP+WS.
  if (!lamp.ip) {
    errorMessage.value = `${lamp.name} is reachable over Bluetooth but not on this WiFi — try again after the lamp joins your network.`
    return
  }

  caseBLamp.value = lamp
  caseBFormValues.value = { caseBPassword: '' }
  caseBBusy.value = false
}
</script>

<template>
  <div class="add-lamp-page">
    <div class="container">
      <div class="main-content">

        <!-- ── Case A Wizard ──────────────────────────────────────────────── -->
        <template v-if="wizardLamp">
          <header class="page-header">
            <button class="back-btn" :disabled="wizardBusy" @click="cancelWizard">
              <span class="back-arrow">←</span>
              <span>Back</span>
            </button>
            <h1>Set Up Lamp</h1>
          </header>

          <!-- Error banner -->
          <div v-if="errorMessage" class="error-banner">
            <span class="error-text">{{ errorMessage }}</span>
            <button class="error-dismiss" @click="clearError">Try again</button>
          </div>

          <!-- Status / spinner -->
          <div v-if="wizardStatus" class="status-panel">
            <div class="loading-spinner"></div>
            <p class="status-text">{{ wizardStatus }}</p>
          </div>

          <!-- Setup form -->
          <div v-else class="wizard-form-wrap">
            <ComponentForm
              :fields="setupFields"
              :model-value="wizardFormValues"
              @update:model-value="handleWizardFormUpdate"
              :show-button="false"
              :disabled="wizardBusy"
            />

            <div class="form-actions">
              <button
                class="gradient-btn"
                :disabled="wizardBusy"
                @click="submitSetup"
              >
                <span v-if="wizardBusy">Setting up…</span>
                <span v-else>Set up</span>
              </button>
            </div>
          </div>
        </template>

        <!-- ── Case B Password Panel ──────────────────────────────────────── -->
        <template v-else-if="caseBLamp">
          <header class="page-header">
            <button class="back-btn" :disabled="caseBBusy" @click="cancelCaseB">
              <span class="back-arrow">←</span>
              <span>Back</span>
            </button>
            <h1>Connect to Lamp</h1>
          </header>

          <!-- Error banner -->
          <div v-if="errorMessage" class="error-banner">
            <span class="error-text">{{ errorMessage }}</span>
            <button class="error-dismiss" @click="clearError">Try again</button>
          </div>

          <div class="lamp-connect-info">
            <div class="connect-lamp-name">{{ caseBLamp.name }}</div>
            <div class="connect-lamp-ip">{{ caseBLamp.ip }}</div>
          </div>

          <div class="case-b-form-wrap">
            <ComponentForm
              :fields="caseBFields"
              :model-value="caseBFormValues"
              @update:model-value="handleCaseBFormUpdate"
              :show-button="false"
              :disabled="caseBBusy"
            />

            <div class="form-actions">
              <button
                class="gradient-btn"
                :disabled="caseBBusy"
                @click="connectCaseB"
              >
                <span v-if="caseBBusy">Connecting…</span>
                <span v-else>Connect</span>
              </button>
            </div>
          </div>
        </template>

        <!-- ── Scan list ──────────────────────────────────────────────────── -->
        <template v-else>
          <header class="page-header">
            <button class="back-btn" @click="router.push({ name: 'lamps' })">
              <span class="back-arrow">←</span>
              <span>Back</span>
            </button>
            <h1>Add a Lamp</h1>
          </header>

          <!-- Error banner -->
          <div v-if="errorMessage" class="error-banner">
            <span class="error-text">{{ errorMessage }}</span>
            <button class="error-dismiss" @click="clearError">Try again</button>
          </div>

          <!-- Scanning / loading state -->
          <div v-if="scanning" class="status-panel">
            <div class="loading-spinner"></div>
            <p class="status-text">Scanning for lamps…</p>
          </div>

          <template v-else>
            <p v-if="!lamps.length" class="empty">No lamps found nearby. Make sure your lamp is powered on.</p>

            <ul v-else class="lamp-list">
              <li
                v-for="lamp in lamps"
                :key="lamp.id"
                class="lamp-row"
                @click="selectLamp(lamp)"
              >
                <div class="lamp-info">
                  <span class="lamp-name">{{ lamp.name }}</span>
                  <span class="lamp-hint">
                    <template v-if="lamp.isUnconfigured">Nearby — not set up yet</template>
                    <template v-else-if="lamp.viaMdns">On your WiFi · {{ lamp.ip }}</template>
                    <template v-else>Nearby</template>
                  </span>
                </div>
                <span class="lamp-chevron">›</span>
              </li>
            </ul>
          </template>
        </template>

      </div>
    </div>
  </div>
</template>

<style scoped>
/* ── Page shell (matches Lamp.vue layout) ──────────────────────────────────── */
.add-lamp-page {
  min-height: 100vh;
  background: var(--brand-midnight-black);
  padding: 16px;
  width: 100%;
}

.container {
  width: 100%;
  max-width: 100%;
  margin: 0 auto;
}

.main-content {
  background: var(--color-background-soft);
  border-radius: 16px;
  padding: 20px;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
  backdrop-filter: blur(10px);
}

/* ── Header ──────────────────────────────────────────────────────────────── */
.page-header {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 24px;
}

.page-header h1 {
  color: var(--brand-lamp-white);
  font-size: 1rem;
  font-weight: 800;
  flex: 1;
}

/* ── Back button ─────────────────────────────────────────────────────────── */
.back-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  background: var(--color-background-mute);
  border: 1px solid var(--color-border);
  border-radius: 20px;
  color: var(--brand-lamp-white);
  font-size: 0.875rem;
  font-weight: 500;
  cursor: pointer;
  padding: 8px 14px;
  transition: all 0.2s ease;
  font-family: inherit;
}

.back-btn:hover:not(:disabled) {
  background: var(--color-border-hover);
  border-color: var(--color-border-hover);
}

.back-btn:active:not(:disabled) {
  background: rgba(253, 253, 253, 0.08);
}

.back-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.back-arrow {
  font-size: 1rem;
  line-height: 1;
}

/* ── Error banner ────────────────────────────────────────────────────────── */
.error-banner {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  background: rgba(248, 113, 113, 0.12);
  border: 1px solid rgba(248, 113, 113, 0.3);
  border-radius: 12px;
  padding: 14px 16px;
  margin-bottom: 20px;
}

.error-text {
  color: var(--color-error);
  font-size: 0.9rem;
  line-height: 1.4;
  flex: 1;
}

.error-dismiss {
  background: none;
  border: 1px solid rgba(248, 113, 113, 0.5);
  border-radius: 8px;
  color: var(--color-error);
  font-size: 0.8rem;
  font-weight: 600;
  cursor: pointer;
  padding: 6px 12px;
  transition: all 0.2s ease;
  white-space: nowrap;
  font-family: inherit;
}

.error-dismiss:hover {
  background: rgba(248, 113, 113, 0.1);
}

/* ── Loading / status panel ──────────────────────────────────────────────── */
.status-panel {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 48px 16px;
  gap: 16px;
}

.loading-spinner {
  width: 40px;
  height: 40px;
  border: 3px solid var(--color-border);
  border-top-color: var(--brand-aurora-blue);
  border-radius: 50%;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.status-text {
  color: var(--color-text-secondary);
  font-size: 0.95rem;
  text-align: center;
}

/* ── Lamp list (scan results) ────────────────────────────────────────────── */
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
  justify-content: space-between;
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

.lamp-info {
  display: flex;
  flex-direction: column;
  gap: 4px;
  flex: 1;
}

.lamp-name {
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-size: 1rem;
}

.lamp-hint {
  color: var(--color-text-secondary);
  font-size: 0.82rem;
}

.lamp-chevron {
  color: var(--brand-slate-grey);
  font-size: 1.3rem;
  flex-shrink: 0;
}

/* ── Empty state ─────────────────────────────────────────────────────────── */
.empty {
  color: var(--color-text-secondary);
  text-align: center;
  padding: 48px 0;
  font-size: 0.95rem;
}

/* ── Case B connect info ─────────────────────────────────────────────────── */
.lamp-connect-info {
  background: var(--color-background-mute);
  border-radius: 12px;
  padding: 16px 20px;
  margin-bottom: 20px;
  border: 1px solid var(--color-border);
}

.connect-lamp-name {
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-size: 1rem;
}

.connect-lamp-ip {
  color: var(--color-text-secondary);
  font-size: 0.85rem;
  margin-top: 3px;
}

/* ── Form wrappers ───────────────────────────────────────────────────────── */
.wizard-form-wrap,
.case-b-form-wrap {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

/* ── Form actions / gradient button ─────────────────────────────────────── */
.form-actions {
  margin-top: 24px;
  display: flex;
  justify-content: center;
}

.gradient-btn {
  padding: 16px 32px;
  border: none;
  border-radius: 50px;
  font-size: 1rem;
  font-weight: 600;
  cursor: pointer;
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-glow-pink));
  color: var(--brand-lamp-white);
  transition: all 0.3s ease;
  box-shadow:
    0 4px 16px rgba(0, 0, 0, 0.3),
    0 0 0 1px rgba(255, 255, 255, 0.1);
  min-width: 160px;
  font-family: inherit;
}

.gradient-btn:hover:not(:disabled) {
  transform: translateY(-2px);
  box-shadow:
    0 8px 24px rgba(0, 0, 0, 0.4),
    0 0 20px rgba(68, 108, 156, 0.4),
    0 0 0 1px rgba(253, 253, 253, 0.2);
}

.gradient-btn:active:not(:disabled) {
  transform: translateY(0);
}

.gradient-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
  transform: none;
}

/* ── Responsive ──────────────────────────────────────────────────────────── */
@media (min-width: 480px) {
  .container {
    max-width: 400px;
  }

  .add-lamp-page {
    padding: 20px;
  }
}

@media (min-width: 1024px) {
  .container {
    max-width: 450px;
  }
}

@media (max-width: 480px) {
  .gradient-btn {
    width: 100%;
  }
}
</style>
