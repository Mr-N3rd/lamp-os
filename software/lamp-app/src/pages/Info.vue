<!-- eslint-disable vue/multi-word-component-names -->
<script setup lang="ts">
import { useRouter } from 'vue-router'
import LamplitLogo from '@/components/LamplitLogo.vue'
import LampIcon from '@/components/LampIcon.vue'
import { useTapCounter } from '@/composables/useTapCounter'
import { useLampStore } from '@/stores/lamp'

const router = useRouter()
const lampStore = useLampStore()

const { recordTap } = useTapCounter(5, 3000, () => {
  if (lampStore.state.lamp?.advancedEnabled) return
  lampStore.updateAdvancedEnabled(true)
  router.push('/setup')
})
</script>

<template>
  <section class="tab-panel" aria-label="Information">
    <div class="info-content">
      <div class="logo-container" @click="recordTap">
        <LamplitLogo style="width: 50%; max-width: 200px;" />
      </div>

      <div class="tagline">
        <span class="tagline-icon">✦</span>
        Sparking inspiration through shared creative experiences
      </div>

      <div class="info-section">
        <p>
          Lamplit Art Society is a non-profit collective sparking
          connection and creativity through shared lamp art. More at
          <a href="https://lamplit.ca" target="_blank" rel="noopener">lamplit.ca</a>.
        </p>
      </div>

      <!-- Nearby Lamps (BLE + ESP-NOW unified) -->
      <div class="info-section grid-section">
        <h3 class="section-title">
          <span class="title-accent">Nearby</span> Lamps
        </h3>
        <p v-if="!lampStore.nearbyLamps.length" class="grid-empty">
          No other lamps heard yet. Other lamps within Bluetooth range
          (or further via the grid) will appear here.
        </p>
        <ul v-else class="grid-list">
          <li
            v-for="lamp in lampStore.nearbyLamps"
            :key="lamp.mac || lamp.name"
            class="grid-row"
            :class="{ 'grid-row--active': lamp.mac && lampStore.targetMac === lamp.mac }"
          >
            <span
              class="status-dot"
              :class="lamp.viaEspNow ? 'status-dot--mesh' : 'status-dot--bt'"
              :title="lamp.viaEspNow ? 'Online · on the mesh' : 'Online · legacy BLE only'"
            ></span>
            <div class="grid-info">
              <span class="grid-name">{{ lamp.name || '(unnamed)' }}</span>
              <span v-if="lamp.mac" class="grid-mac">{{ lamp.mac }}</span>
            </div>
            <LampIcon :shade="lamp.shade" :base="lamp.base" :size="26" />
            <button
              v-if="lamp.mac && lamp.viaEspNow"
              class="grid-target-btn"
              :disabled="!lampStore.wsConnected"
              @click="lampStore.setTargetMac(lamp.mac)"
            >
              {{ lampStore.targetMac === lamp.mac ? 'Targeting' : 'Configure' }}
            </button>
          </li>
        </ul>
        <button
          v-if="lampStore.targetMac !== 'self'"
          class="grid-back-btn"
          @click="lampStore.setTargetMac('self')"
        >
          ← Back to local lamp
        </button>
      </div>

      <div class="footer-link">
        <span class="link-decoration"></span>
        <a href="https://lamplit.ca" target="_blank" rel="noopener">lamplit.ca</a>
        <span class="link-decoration"></span>
      </div>
    </div>
  </section>
</template>

<style scoped>
.tab-panel {
  animation: fadeIn 0.3s ease-in-out;
}

/* Grid (ESP-NOW peer list) */
.grid-empty {
  color: var(--color-text-secondary);
  font-size: 0.9rem;
  font-style: italic;
}
.grid-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.grid-row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px;
  background: var(--color-background-mute);
  border: 1px solid var(--color-border);
  border-radius: 12px;
}
.grid-row--active {
  border-color: var(--brand-glow-pink);
  background: rgba(225, 164, 74, 0.08);
}
.grid-info {
  display: flex;
  flex-direction: column;
  flex: 1;
  min-width: 0;
}
.grid-name {
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-size: 0.95rem;
}
.status-dot {
  flex-shrink: 0;
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: var(--brand-slate-grey);
}
.status-dot--mesh {
  background: var(--color-success);
  box-shadow: 0 0 8px rgba(141, 205, 166, 0.6);
  animation: pulse 2s ease-in-out infinite;
}
.status-dot--bt {
  background: #4d6f53;
  box-shadow: 0 0 4px rgba(77, 111, 83, 0.4);
}
@keyframes pulse {
  0%, 100% { box-shadow: 0 0 6px rgba(141, 205, 166, 0.5); }
  50%      { box-shadow: 0 0 12px rgba(141, 205, 166, 0.85); }
}

/* Inline links inside info sections (e.g. the About paragraph's
   lamplit.ca link). Browser default blue is super gross on the dark
   background; match the footer link's brand colors. */
.info-section a {
  color: var(--brand-aurora-blue);
  text-decoration: none;
  font-weight: 600;
  border-bottom: 1px solid rgba(68, 108, 156, 0.4);
  transition: color 0.2s ease, border-color 0.2s ease;
}
.info-section a:hover {
  color: var(--brand-glow-pink);
  border-bottom-color: rgba(225, 164, 74, 0.6);
}
.grid-mac {
  color: var(--color-text-secondary);
  font-size: 0.75rem;
  font-family: ui-monospace, monospace;
}
.grid-target-btn {
  flex-shrink: 0;
  padding: 8px 14px;
  border: none;
  border-radius: 8px;
  background: var(--brand-aurora-blue);
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-size: 0.85rem;
  cursor: pointer;
}
.grid-target-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}
.grid-back-btn {
  margin-top: 12px;
  padding: 10px 16px;
  border: 1px solid var(--color-border);
  background: transparent;
  border-radius: 8px;
  color: var(--brand-lamp-white);
  font-weight: 500;
  cursor: pointer;
  width: 100%;
}

@keyframes fadeIn {
  from {
    opacity: 0;
    transform: translateY(10px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}

.info-content {
  padding: 16px 20px;
}

.logo-container {
  display: flex;
  justify-content: center;
  margin-bottom: 24px;
}

.tagline {
  text-align: center;
  font-size: 0.95rem;
  font-style: italic;
  color: var(--brand-glow-pink);
  margin-bottom: 32px;
  line-height: 1.5;
}

.tagline-icon {
  display: inline-block;
  margin-right: 8px;
  color: var(--brand-amber-gold);
  animation: sparkle 2s ease-in-out infinite;
}

@keyframes sparkle {
  0%, 100% {
    opacity: 1;
    transform: scale(1);
  }
  50% {
    opacity: 0.6;
    transform: scale(0.9);
  }
}

.info-section {
  margin-bottom: 24px;
}

.section-title {
  font-size: 1.1rem;
  font-weight: 600;
  color: var(--brand-lamp-white);
  margin: 0 0 8px 0;
}

.title-accent {
  color: var(--brand-lumen-green);
  font-weight: 700;
}

.info-section p {
  margin: 0;
  font-size: 0.9rem;
  line-height: 1.7;
  color: var(--brand-fog-grey);
}

.info-section em {
  font-style: normal;
  color: var(--brand-amber-gold);
}

.footer-link {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 16px;
  margin-top: 32px;
  padding-top: 24px;
  border-top: 1px solid rgba(141, 205, 166, 0.2);
}

.link-decoration {
  width: 40px;
  height: 2px;
  background: linear-gradient(
    90deg,
    transparent,
    var(--brand-glow-pink),
    transparent
  );
  border-radius: 1px;
}

.footer-link a {
  font-size: 1.1rem;
  font-weight: 600;
  color: var(--brand-aurora-blue);
  text-decoration: none;
  transition: all 0.2s ease;
}

.footer-link a:hover {
  color: var(--brand-glow-pink);
}

/* Mobile optimizations */
@media (max-width: 480px) {
  .info-content {
    padding: 12px 16px;
  }

  .tagline {
    font-size: 0.9rem;
  }
}
</style>
