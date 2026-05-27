<script setup lang="ts">
import { onMounted, onUnmounted, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import TopNavigation from '@/components/TopNavigation.vue'
import { useLampStore, tabs } from '@/stores/lamp'
import { useLampInventoryStore } from '@/stores/lampInventory'

const route = useRoute()
const router = useRouter()
const lampStore = useLampStore()
const inventory = useLampInventoryStore()

const targetForLamp = (id: string) => {
  const lamp = inventory.findById(id)
  if (!lamp) return null
  return {
    deviceId: lamp.id,
    password: lamp.password,
  }
}

onMounted(async () => {
  if (!inventory.loaded) await inventory.load()
  const id = String(route.params.id)
  const target = targetForLamp(id)
  if (!target) {
    router.replace({ name: 'lamps' })
    return
  }
  await lampStore.initialize(target)
})

watch(
  () => route.params.id,
  async (newId, oldId) => {
    if (newId === oldId) return
    const target = targetForLamp(String(newId))
    if (!target) {
      router.replace({ name: 'lamps' })
      return
    }
    await lampStore.setTarget(target)
  },
)

onUnmounted(() => {
  lampStore.cleanup()
})

const handleTabChange = (tabId: string) => {
  router.push({ name: `lamp-${tabId}`, params: { id: route.params.id } })
}

async function goBackToLamps() {
  await lampStore.cleanup()
  await router.push({ name: 'lamps' })
}

async function retryConnect() {
  const id = String(route.params.id)
  const target = targetForLamp(id)
  if (!target) return
  await lampStore.setTarget(target)
}

// Map route name to tab ID
const getTabIdFromRoute = (routeName: string | undefined): string => {
  if (!routeName) return 'home'
  if (routeName.startsWith('lamp-')) return routeName.slice('lamp-'.length)
  return 'home'
}

// Sync route changes to active tab
watch(
  () => route.name,
  (newName) => {
    lampStore.setActiveTab(getTabIdFromRoute(newName as string | undefined))
  },
  { immediate: true },
)
</script>

<template>
  <div class="lamp-layout">
    <!-- WebSocket Status Indicator -->
    <div
      class="ws-status-indicator"
      :class="{ connected: lampStore.wsConnected }"
      :title="lampStore.wsConnected ? 'WebSocket Connected' : 'WebSocket Disconnected'"
    >
      <div class="ws-status-dot"></div>
    </div>

    <div v-if="lampStore.loaded" class="container">
      <!-- Top bar: back to lamps + lamp name -->
      <div class="lamp-topbar">
        <button class="lamp-back-btn" @click="goBackToLamps">
          <span class="lamp-back-arrow">←</span>
          <span>Lamps</span>
        </button>
        <span class="lamp-topbar-name">{{ lampStore.state.lamp?.name || 'Lamp' }}</span>
        <span class="lamp-topbar-spacer"></span>
      </div>

      <!-- Connection error banner -->
      <div v-if="lampStore.connectionError" class="connection-error-banner">
        <span class="connection-error-text">{{ lampStore.connectionError }}</span>
        <button class="connection-error-retry" @click="retryConnect">Retry</button>
      </div>

      <main class="main-content">
        <!-- Tab Navigation -->
        <TopNavigation :tabs="tabs" :active-tab="lampStore.activeTab" @update:active-tab="handleTabChange" />

        <!-- Page Content (Router View) -->
        <div class="tab-content">
          <router-view />
        </div>
      </main>
    </div>

    <!-- Loading State -->
    <div v-else class="loading-container">
      <div class="loading-spinner"></div>
      <p>Connecting…</p>
    </div>

    <!-- Debug console — surfaces the BLE init flow step by step -->
    <details class="lamp-debug-panel">
      <summary>Debug ({{ lampStore.debugLog.length }} steps, wsConnected={{ lampStore.wsConnected }}, loaded={{ lampStore.loaded }})</summary>
      <div v-if="lampStore.connectionError" class="lamp-debug-error">
        <strong>error:</strong> {{ lampStore.connectionError }}
      </div>
      <ul class="lamp-debug-list">
        <li v-for="(entry, i) in lampStore.debugLog" :key="i">
          <span class="lamp-debug-time">+{{ ((entry.t - lampStore.debugLog[0].t) / 1000).toFixed(2) }}s</span>
          <span class="lamp-debug-msg">{{ entry.msg }}</span>
        </li>
      </ul>
    </details>

    <!-- Floating Save Button -->
    <div v-if="lampStore.loaded" class="floating-save-container">
      <button
        class="floating-save-button"
        :class="{
          'has-changes': lampStore.hasChanges,
          saving: lampStore.saving,
          'no-changes': !lampStore.hasChanges || lampStore.disabled,
        }"
        @click="lampStore.saveSettings"
        :disabled="!lampStore.hasChanges || lampStore.saving || lampStore.disabled"
      >
        <span v-if="lampStore.disabled">Connecting...</span>
        <span v-else-if="lampStore.saving">Saving...</span>
        <span v-else-if="lampStore.hasChanges">Save Changes</span>
        <span v-else>No Changes</span>
      </button>
    </div>
  </div>
</template>

<style scoped>
.lamp-layout {
  min-height: 100vh;
  background: var(--brand-midnight-black);
  padding: 16px;
  padding-bottom: 10px !important;
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
  padding-bottom: 40px !important;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
  backdrop-filter: blur(10px);
}

/* Loading State */
.loading-container {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  min-height: 50vh;
  color: var(--brand-fog-grey);
}

.loading-spinner {
  width: 40px;
  height: 40px;
  border: 3px solid var(--color-border);
  border-top-color: var(--brand-aurora-blue);
  border-radius: 50%;
  animation: spin 1s linear infinite;
  margin-bottom: 16px;
}

@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}

/* Tab Content Styles */
.tab-content {
  min-height: 200px;
}

/* Top bar with back button + lamp name */
.lamp-topbar {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 12px;
  padding: 0 4px;
}

.lamp-back-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  background: var(--color-background-mute);
  border: 1px solid var(--color-border);
  border-radius: 20px;
  color: var(--brand-lamp-white);
  font-size: 0.85rem;
  font-weight: 500;
  cursor: pointer;
  padding: 6px 12px;
  font-family: inherit;
  transition: all 0.2s ease;
}

.lamp-back-btn:hover {
  background: var(--color-border-hover);
}

.lamp-back-arrow {
  font-size: 1rem;
  line-height: 1;
}

.lamp-topbar-name {
  flex: 1;
  text-align: center;
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-size: 0.95rem;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.lamp-topbar-spacer {
  /* Mirrors back button width so the name truly centers */
  width: 72px;
}

/* Connection error banner */
.connection-error-banner {
  display: flex;
  align-items: center;
  gap: 12px;
  background: rgba(248, 113, 113, 0.12);
  border: 1px solid rgba(248, 113, 113, 0.3);
  border-radius: 12px;
  padding: 12px 14px;
  margin-bottom: 12px;
}

.connection-error-text {
  flex: 1;
  color: var(--color-error);
  font-size: 0.85rem;
  line-height: 1.35;
  word-break: break-word;
}

.connection-error-retry {
  background: transparent;
  border: 1px solid var(--color-error);
  border-radius: 8px;
  color: var(--color-error);
  font-size: 0.8rem;
  font-weight: 600;
  padding: 6px 12px;
  cursor: pointer;
  font-family: inherit;
}

/* Debug console */
.lamp-debug-panel {
  margin: 16px 4px 80px;
  padding: 10px 14px;
  background: var(--color-background-mute);
  border-radius: 10px;
  border: 1px solid var(--color-border);
  color: var(--color-text-secondary);
  font-size: 0.75rem;
  font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
}
.lamp-debug-panel summary {
  cursor: pointer;
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-family: inherit;
  word-break: break-all;
}
.lamp-debug-error {
  margin: 8px 0;
  padding: 8px 10px;
  background: rgba(248, 113, 113, 0.12);
  border: 1px solid var(--color-error);
  border-radius: 6px;
  color: var(--color-error);
  word-break: break-word;
}
.lamp-debug-list {
  list-style: none;
  padding: 0;
  margin: 8px 0 0;
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.lamp-debug-list li {
  display: flex;
  gap: 8px;
  border-bottom: 1px solid var(--color-border);
  padding-bottom: 4px;
}
.lamp-debug-list li:last-child { border-bottom: none; }
.lamp-debug-time {
  color: var(--brand-slate-grey);
  flex-shrink: 0;
  min-width: 50px;
}
.lamp-debug-msg {
  flex: 1;
  word-break: break-word;
}

/* Floating Save Button Styles */
.floating-save-container {
  position: fixed;
  bottom: 15px;
  z-index: 1000;
  pointer-events: none;
  display: flex;
  justify-content: center;
  width: 100%;
}

.floating-save-button {
  pointer-events: auto;
  padding: 16px 32px;
  border: none;
  border-radius: 50px;
  font-size: 1rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.3s ease;
  box-shadow:
    0 20px 60px rgba(0, 0, 0, 0.4),
    0 8px 32px rgba(0, 0, 0, 0.3),
    0 0 0 1px rgba(255, 255, 255, 0.1);
  backdrop-filter: blur(10px);
  font-family: inherit;
  min-width: 160px;
  text-align: center;
}

.floating-save-button.no-changes {
  background: var(--color-background-mute) !important;
  color: var(--brand-slate-grey);
  cursor: not-allowed;
}

.floating-save-button.has-changes {
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-glow-pink));
  color: var(--brand-lamp-white);
  cursor: pointer;
}

.floating-save-button.has-changes:hover {
  transform: translateY(-4px);
  box-shadow:
    0 25px 80px rgba(0, 0, 0, 0.5),
    0 15px 50px rgba(68, 108, 156, 0.4),
    0 0 0 1px rgba(253, 253, 253, 0.2),
    0 0 30px rgba(68, 108, 156, 0.4);
}

.floating-save-button.saving {
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-lumen-green));
  color: var(--brand-lamp-white);
  cursor: not-allowed;
  opacity: 0.8;
}

.floating-save-button:disabled {
  cursor: not-allowed;
}

/* WebSocket Status Indicator */
.ws-status-indicator {
  position: fixed;
  bottom: 16px;
  right: 16px;
  z-index: 1001;
  background: rgba(0, 0, 0, 0.6);
  border-radius: 50%;
  backdrop-filter: blur(10px);
  transition: all 0.3s ease;
}

.ws-status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--color-error);
  transition: all 0.3s ease;
  box-shadow: 0 0 8px rgba(248, 113, 113, 0.5);
}

.ws-status-indicator.connected .ws-status-dot {
  background: var(--color-success);
  box-shadow: 0 0 8px rgba(141, 205, 166, 0.5);
}

/* Mobile-first design */
@media (min-width: 480px) {
  .container {
    max-width: 400px;
  }

  .lamp-layout {
    padding: 20px;
  }

  .main-content {
    padding: 20px;
    padding-bottom: 40px !important;
  }
}

@media (min-width: 1024px) {
  .container {
    max-width: 450px;
  }
}

/* Mobile adjustments */
@media (max-width: 479px) {
  .floating-save-container {
    bottom: 16px;
    left: 16px;
    right: 16px;
    transform: none;
    padding: 0 16px;
  }

  .floating-save-button {
    width: 100%;
    max-width: 300px;
    min-width: auto;
  }

  .ws-status-indicator {
    bottom: 12px;
    right: 12px;
  }
}
</style>
