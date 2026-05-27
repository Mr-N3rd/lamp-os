<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useLampStore } from '@/stores/lamp'

const props = defineProps<{ open: boolean }>()
const emit = defineEmits<{ close: [] }>()

const lampStore = useLampStore()

const selectedSsid = ref<string>('')
const password = ref<string>('')
const submitting = ref(false)
const errorMsg = ref<string | null>(null)

watch(
  () => props.open,
  (open) => {
    if (!open) return
    selectedSsid.value = ''
    password.value = ''
    errorMsg.value = null
    submitting.value = false
    void lampStore.wifiScan()
  },
  { immediate: true },
)

const scanResults = computed(() => lampStore.wifiState?.scanResults ?? [])
const wifiStatus = computed(() => lampStore.wifiState?.state ?? 'idle')

const selectedEntry = computed(() => scanResults.value.find((r) => r.ssid === selectedSsid.value))
const needsPassword = computed(() => selectedEntry.value?.encrypted ?? true)

const canSubmit = computed(() => {
  if (!lampStore.wsConnected) return false
  if (submitting.value) return false
  if (!selectedSsid.value) return false
  if (needsPassword.value && password.value.length < 8) return false
  return true
})

watch(wifiStatus, (s) => {
  if (!submitting.value) return
  if (s === 'connected') {
    submitting.value = false
    emit('close')
  } else if (s === 'failed') {
    submitting.value = false
    const err = lampStore.wifiState?.lastError
    if (err === 'auth') errorMsg.value = 'Wrong password'
    else if (err === 'noap') errorMsg.value = 'Network not found'
    else if (err === 'timeout') errorMsg.value = 'Connection timed out'
    else errorMsg.value = 'Connection failed'
  }
})

const rssiBars = (rssi: number): number => {
  if (rssi >= -55) return 4
  if (rssi >= -65) return 3
  if (rssi >= -75) return 2
  return 1
}

const onSubmit = async () => {
  if (!canSubmit.value) return
  submitting.value = true
  errorMsg.value = null
  try {
    await lampStore.wifiConnect(selectedSsid.value, password.value)
  } catch (err) {
    submitting.value = false
    errorMsg.value = err instanceof Error ? err.message : String(err)
  }
}

const onRescan = () => {
  void lampStore.wifiScan()
}

const onCancel = () => emit('close')
</script>

<template>
  <Teleport to="body">
    <div v-if="open" class="sheet-overlay" @click.self="onCancel">
      <div class="sheet">
        <header class="sheet-header">
          <h3>Choose home network</h3>
          <button class="sheet-close" @click="onCancel" aria-label="Close">×</button>
        </header>

        <div class="sheet-body">
          <div class="net-toolbar">
            <span class="net-status">
              <template v-if="wifiStatus === 'scanning'">Scanning…</template>
              <template v-else-if="wifiStatus === 'connecting'">Connecting to {{ selectedSsid }}…</template>
              <template v-else-if="scanResults.length">{{ scanResults.length }} networks found</template>
              <template v-else>Tap rescan</template>
            </span>
            <button class="net-rescan" :disabled="wifiStatus === 'scanning'" @click="onRescan">Rescan</button>
          </div>

          <ul v-if="scanResults.length" class="net-list">
            <li
              v-for="r in scanResults"
              :key="r.ssid"
              class="net-row"
              :class="{ 'net-row--selected': selectedSsid === r.ssid }"
              @click="selectedSsid = r.ssid; password = ''; errorMsg = null"
            >
              <span class="net-ssid">{{ r.ssid }}</span>
              <span class="net-meta">
                <span class="net-lock" v-if="r.encrypted">🔒</span>
                <span class="net-rssi">{{ '·'.repeat(4 - rssiBars(r.rssi)) }}{{ '|'.repeat(rssiBars(r.rssi)) }}</span>
              </span>
            </li>
          </ul>

          <div v-if="selectedSsid && needsPassword" class="net-password">
            <label class="net-label">Password for {{ selectedSsid }}</label>
            <input
              v-model="password"
              type="password"
              class="net-input"
              placeholder="At least 8 characters"
              autocomplete="current-password"
            />
          </div>

          <p v-if="errorMsg" class="sheet-error">{{ errorMsg }}</p>
        </div>

        <footer class="sheet-footer">
          <button class="sheet-btn sheet-btn--cancel" @click="onCancel">Cancel</button>
          <button class="sheet-btn sheet-btn--primary" :disabled="!canSubmit" @click="onSubmit">
            {{ submitting ? 'Connecting…' : 'Connect' }}
          </button>
        </footer>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.sheet-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.8);
  display: flex;
  align-items: flex-end;
  justify-content: center;
  z-index: 900;
  backdrop-filter: blur(4px);
}

.sheet {
  background: var(--color-background-mute);
  border-top-left-radius: 20px;
  border-top-right-radius: 20px;
  width: 100%;
  max-width: 540px;
  max-height: 90vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 -8px 32px rgba(0, 0, 0, 0.5);
  animation: slide-up 0.22s ease-out;
}

@keyframes slide-up {
  from { transform: translateY(100%); }
  to   { transform: translateY(0); }
}

.sheet-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 20px 20px 14px;
  border-bottom: 1px solid var(--color-border);
}

.sheet-header h3 { margin: 0; font-size: 1.1rem; font-weight: 700; color: var(--brand-lamp-white); }

.sheet-close {
  width: 36px; height: 36px;
  border-radius: 10px;
  border: 1px solid var(--color-border);
  background: var(--color-background-soft);
  color: var(--brand-fog-grey);
  font-size: 20px;
  cursor: pointer;
}

.sheet-body { padding: 20px; overflow-y: auto; flex: 1; display: flex; flex-direction: column; gap: 16px; }

.net-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.net-status { color: var(--brand-fog-grey); font-size: 0.9rem; }
.net-rescan {
  padding: 6px 12px;
  border-radius: 8px;
  background: var(--color-background-soft);
  color: var(--brand-fog-grey);
  border: 1px solid var(--color-border);
  cursor: pointer;
  font-size: 0.85rem;
}
.net-rescan:disabled { opacity: 0.5; cursor: not-allowed; }

.net-list {
  list-style: none;
  padding: 0;
  margin: 0;
  display: flex;
  flex-direction: column;
  gap: 6px;
  max-height: 320px;
  overflow-y: auto;
}
.net-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px 14px;
  background: var(--color-background-soft);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  cursor: pointer;
}
.net-row--selected {
  background: rgba(68, 108, 156, 0.15);
  border-color: var(--brand-aurora-blue);
}
.net-ssid { color: var(--brand-lamp-white); font-weight: 600; }
.net-meta { display: flex; align-items: center; gap: 8px; color: var(--brand-fog-grey); }
.net-rssi { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }

.net-password { display: flex; flex-direction: column; gap: 6px; }
.net-label { font-size: 0.85rem; color: var(--brand-fog-grey); font-weight: 600; }
.net-input {
  padding: 12px 14px;
  border-radius: 10px;
  border: 1px solid var(--color-border);
  background: var(--color-background-soft);
  color: var(--brand-lamp-white);
  font-size: 0.95rem;
  font-family: inherit;
}

.sheet-error { color: #ff6b6b; font-size: 0.9rem; margin: 0; }

.sheet-footer {
  display: flex; gap: 10px;
  padding: 14px 20px 20px;
  border-top: 1px solid var(--color-border);
}
.sheet-btn {
  flex: 1;
  padding: 12px 18px;
  border-radius: 10px;
  font-weight: 600;
  font-size: 0.95rem;
  cursor: pointer;
  border: none;
}
.sheet-btn--cancel {
  background: var(--color-background-soft);
  color: var(--brand-fog-grey);
  border: 1px solid var(--color-border);
}
.sheet-btn--primary {
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-glow-pink));
  color: var(--brand-lamp-white);
}
.sheet-btn:disabled { opacity: 0.5; cursor: not-allowed; }
</style>
