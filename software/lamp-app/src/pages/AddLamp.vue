<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import type { Ref } from 'vue'
import { useRouter } from 'vue-router'
import { startScan } from '@/services/scan'
import type { NearbyLamp, ScanDebug } from '@/services/scan'
import { useLampInventoryStore } from '@/stores/lampInventory'

const router = useRouter()
const inventory = useLampInventoryStore()

// ── Scan state ────────────────────────────────────────────────────────────────
const lamps: Ref<NearbyLamp[]> = ref<NearbyLamp[]>([])
const scanning = ref(true)
const debug = ref<ScanDebug | null>(null)
let stopScan: (() => void) | null = null

onMounted(async () => {
  const result = await startScan()
  lamps.value = result.lamps.value
  stopScan = result.stop
  debug.value = result.debug
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

// ── Selection state ───────────────────────────────────────────────────────────
const selectedLamp = ref<NearbyLamp | null>(null)
const lampPassword = ref('')
const connecting = ref(false)

function selectLamp(lamp: NearbyLamp) {
  errorMessage.value = null
  selectedLamp.value = lamp
  lampPassword.value = ''
}

function cancelSelection() {
  selectedLamp.value = null
  lampPassword.value = ''
  errorMessage.value = null
}

async function connectLamp() {
  const lamp = selectedLamp.value!
  connecting.value = true
  errorMessage.value = null

  try {
    inventory.add({
      id: lamp.id,
      name: lamp.name,
      password: lampPassword.value || undefined,
      lastSeen: Date.now(),
    })
    await router.push({ name: 'lamp-home', params: { id: lamp.id } })
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    connecting.value = false
    errorMessage.value = `Failed to add lamp: ${message}`
  }
}
</script>

<template>
  <div class="add-lamp-page">
    <div class="container">
      <div class="main-content">

        <!-- ── Connect Panel ─────────────────────────────────────────────── -->
        <template v-if="selectedLamp">
          <header class="page-header">
            <button class="back-btn" :disabled="connecting" @click="cancelSelection">
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
            <div class="connect-lamp-name">{{ selectedLamp.name }}</div>
          </div>

          <div class="connect-form-wrap">
            <div class="field-group">
              <label class="field-label">Password <span class="optional">(optional)</span></label>
              <input
                v-model="lampPassword"
                type="password"
                class="field-input"
                placeholder="Leave blank if none"
                :disabled="connecting"
                autocomplete="off"
              />
              <p class="field-hint">Enter the lamp's password if one has been set.</p>
            </div>

            <div class="form-actions">
              <button
                class="gradient-btn"
                :disabled="connecting"
                @click="connectLamp"
              >
                <span v-if="connecting">Connecting…</span>
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
                  <span class="lamp-hint">Nearby</span>
                </div>
                <span class="lamp-chevron">›</span>
              </li>
            </ul>
          </template>

          <!-- Debug — raw BLE scan results -->
          <details v-if="debug" class="scan-debug-panel" open>
            <summary>Debug: BLE scan ({{ debug.rawBleResults.length }} ads seen)</summary>
            <div v-if="debug.bleError" class="scan-debug-error">
              <strong>BLE error:</strong> {{ debug.bleError }}
            </div>
            <p v-if="!debug.rawBleResults.length" class="scan-debug-empty">
              No advertisements received. Check Bluetooth is on and the app has Nearby Devices permission.
            </p>
            <ul v-else class="scan-debug-list">
              <li v-for="r in debug.rawBleResults" :key="r.deviceId">
                <div><strong>{{ r.name }}</strong> ({{ r.deviceId }}) rssi={{ r.rssi }}</div>
                <div>mfgKeys: <code>[{{ r.mfgKeys.join(', ') || '(none)' }}]</code></div>
                <div v-if="r.uuids.length">uuids: <code>{{ r.uuids.join(', ') }}</code></div>
              </li>
            </ul>
          </details>
        </template>

      </div>
    </div>
  </div>
</template>

<style scoped>
/* ── Page shell ──────────────────────────────────────────────────────────── */
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

/* ── Connect panel ───────────────────────────────────────────────────────── */
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

/* ── Form ────────────────────────────────────────────────────────────────── */
.connect-form-wrap {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.field-group {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.field-label {
  color: var(--brand-lamp-white);
  font-size: 0.9rem;
  font-weight: 600;
}

.optional {
  color: var(--color-text-secondary);
  font-weight: 400;
  font-size: 0.85rem;
}

.field-input {
  background: var(--color-background-mute);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  color: var(--brand-lamp-white);
  font-size: 0.95rem;
  padding: 12px 14px;
  font-family: inherit;
  outline: none;
  transition: border-color 0.2s ease;
  width: 100%;
  box-sizing: border-box;
}

.field-input:focus {
  border-color: var(--brand-aurora-blue);
}

.field-input:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.field-hint {
  color: var(--color-text-secondary);
  font-size: 0.8rem;
  margin: 0;
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
/* Scan debug panel */
.scan-debug-panel {
  margin-top: 16px;
  padding: 10px 14px;
  background: var(--color-background-mute);
  border-radius: 10px;
  border: 1px solid var(--color-border);
  color: var(--color-text-secondary);
  font-size: 0.75rem;
  font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
}
.scan-debug-panel summary {
  cursor: pointer;
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-family: inherit;
}
.scan-debug-error {
  margin: 8px 0;
  padding: 8px 10px;
  background: rgba(248, 113, 113, 0.12);
  border: 1px solid var(--color-error);
  border-radius: 6px;
  color: var(--color-error);
  word-break: break-word;
}
.scan-debug-empty {
  color: var(--brand-slate-grey);
  font-style: italic;
  margin: 8px 0;
}
.scan-debug-list {
  list-style: none;
  padding: 0;
  margin: 8px 0 0;
}
.scan-debug-list li {
  padding: 6px 0;
  border-bottom: 1px solid var(--color-border);
  word-break: break-word;
}
.scan-debug-list li:last-child { border-bottom: none; }
.scan-debug-list code {
  background: rgba(255, 255, 255, 0.05);
  padding: 1px 4px;
  border-radius: 3px;
}
</style>
