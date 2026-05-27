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

function goBackToLamps() {
  void lampStore.cleanup()
  void router.push({ name: 'lamps' })
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
    <!-- Connection status: dot + optional reconnecting label -->
    <div
      class="ws-status-indicator"
      :class="{ connected: lampStore.wsConnected, reconnecting: lampStore.reconnecting }"
      :title="lampStore.reconnecting ? 'Reconnecting…' : lampStore.wsConnected ? 'Connected' : 'Disconnected'"
    >
      <div class="ws-status-dot"></div>
      <span v-if="lampStore.reconnecting" class="ws-status-label">Reconnecting…</span>
    </div>

    <div v-if="lampStore.loaded" class="container">
      <!-- Top bar: back to lamps + lamp name + save -->
      <div class="lamp-topbar">
        <button class="lamp-back-btn" @click="goBackToLamps">
          <span class="lamp-back-arrow">←</span>
          <span>Lamps</span>
        </button>
        <span class="lamp-topbar-name">{{ lampStore.state.lamp?.name || 'Lamp' }}</span>
        <button
          v-if="lampStore.hasChanges"
          class="lamp-topbar-save"
          :disabled="lampStore.saving || lampStore.disabled"
          @click="lampStore.saveSettings"
        >
          {{ lampStore.saving ? 'Saving…' : 'Save' }}
        </button>
        <span v-else class="lamp-topbar-spacer"></span>
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

  </div>
</template>

<style scoped>
.lamp-layout {
  min-height: 100vh;
  background: var(--brand-midnight-black);
  padding: 0 0 10px 0;
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

/* Top bar with back button + lamp name + save */
.lamp-topbar {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 16px;
  background: var(--brand-midnight-black);
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
  /* Mirrors back button width so the name truly centers when no Save shown */
  width: 72px;
}

.lamp-topbar-save {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  padding: 6px 14px;
  border: none;
  border-radius: 20px;
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-glow-pink));
  color: var(--brand-lamp-white);
  font-family: inherit;
  font-size: 0.85rem;
  font-weight: 600;
  cursor: pointer;
  min-width: 72px;
  transition: opacity 0.2s ease, transform 0.2s ease;
}

.lamp-topbar-save:hover:not(:disabled) {
  transform: translateY(-1px);
}

.lamp-topbar-save:disabled {
  opacity: 0.55;
  cursor: not-allowed;
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


/* Connection status indicator */
.ws-status-indicator {
  position: fixed;
  bottom: 16px;
  right: 16px;
  z-index: 1001;
  background: rgba(0, 0, 0, 0.6);
  border-radius: 16px;
  padding: 6px;
  display: inline-flex;
  align-items: center;
  gap: 6px;
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

.ws-status-indicator.reconnecting .ws-status-dot {
  background: var(--brand-aurora-blue);
  box-shadow: 0 0 8px rgba(68, 108, 156, 0.6);
  animation: ws-status-pulse 1s ease-in-out infinite;
}

.ws-status-label {
  color: var(--brand-lamp-white);
  font-size: 0.75rem;
  font-weight: 600;
  padding-right: 4px;
}

@keyframes ws-status-pulse {
  0%, 100% { opacity: 1; }
  50%      { opacity: 0.4; }
}

@media (max-width: 479px) {
  .ws-status-indicator {
    bottom: 12px;
    right: 12px;
  }
}
</style>
