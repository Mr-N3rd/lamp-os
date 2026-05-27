<script setup lang="ts">
import { computed, ref } from 'vue'
import ComponentForm from '@/components/Form.vue'
import BrightnessSlider from '@/components/BrightnessSlider.vue'
import HomeNetworkSheet from '@/components/HomeNetworkSheet.vue'
import type { FieldDefinition, FormValues } from '@/types'
import { useLampStore, MAX_LEDS_BASE } from '@/stores/lamp'

const lampStore = useLampStore()

const homeSheetOpen = ref(false)

const homeMode = computed(() => lampStore.state.homeMode ?? {})
const homeSsid = computed(() => homeMode.value.ssid ?? '')
const homeBrightness = computed(() => homeMode.value.brightness ?? 60)

const wifiStatusLabel = computed(() => {
  const ws = lampStore.wifiState
  if (!ws) return 'Unknown'
  switch (ws.state) {
    case 'connected':  return `Connected${ws.ip ? ' · ' + ws.ip : ''}`
    case 'connecting': return 'Connecting…'
    case 'scanning':   return 'Scanning…'
    case 'failed':
      if (ws.lastError === 'auth') return 'Wrong password'
      if (ws.lastError === 'noap') return 'Network not found'
      if (ws.lastError === 'timeout') return 'Connection timed out'
      return 'Connection failed'
    case 'idle':
    default:           return 'Not connected'
  }
})

const onForget = async () => {
  if (!confirm(`Forget ${homeSsid.value}?`)) return
  try { await lampStore.wifiForget() } catch (err) { console.warn('[setup] wifiForget failed:', err) }
}

const onHomeBrightnessInput = (value: number) => {
  if (!lampStore.state.homeMode) lampStore.state.homeMode = {}
  lampStore.state.homeMode.brightness = value
  lampStore.previewHomeBrightness(value)
}

const onHomeBrightnessChange = () => {
  lampStore.restoreBrightness()
}

// Field definitions for the setup page form
const fields = computed<FieldDefinition[]>(() => [
  {
    name: 'lampNameHeading',
    type: 'group-heading',
    label: 'Lamp Name',
  },
  {
    name: 'name',
    type: 'text',
    help: 'Names must be all lowercase letters and between 3-12 characters.',
    default: '',
    optional: true,
    props: {
      placeholder: 'Enter a name for your lamp',
      maxLength: 12,
      minLength: 3,
      pattern: '[a-z]+',
      transform: 'lowercase',
    },
  },
  {
    name: 'passwordHeading',
    type: 'group-heading',
    label: 'Lamp Password',
  },
  {
    name: 'password',
    type: 'password',
    help: 'Optional password to protect your lamp from changes. Between 8-16 characters. Leave empty for no password.',
    default: '',
    optional: true,
    props: {
      placeholder: 'Optional password',
      maxLength: 16,
      minLength: 8,
    },
  },
  {
    name: 'ledProfileHeading',
    type: 'group-heading',
    label: 'Lamp Base LED Profile',
  },
  ...(lampStore.state.lamp?.advancedEnabled
    ? [
        {
          name: 'baseBpp',
          type: 'select' as const,
          label: 'Base LED Type',
          default: 4,
          optional: true,
          options: [
            { value: 4, label: 'RGBWW' },
            { value: 3, label: 'RGB' },
          ],
        },
      ]
    : []),
  {
    name: 'basePx',
    type: 'number',
    label: 'Base LED Count',
    default: 36,
    optional: true,
    props: {
      min: 5,
      max: MAX_LEDS_BASE,
      placeholder: 'Number of LEDs',
    },
  },
  {
    name: 'knockoutPixels',
    type: 'slot',
    label: 'Per-Pixel Brightness Adjustment',
  },
  ...(lampStore.state.lamp?.advancedEnabled
    ? [
        {
          name: 'shadeProfileHeading',
          type: 'group-heading' as const,
          label: 'Lamp Shade LED Profile',
        },
        {
          name: 'shadeBpp',
          type: 'select' as const,
          label: 'Shade LED Type',
          default: 4,
          optional: true,
          options: [
            { value: 4, label: 'RGBWW' },
            { value: 3, label: 'RGB' },
          ],
        },
      ]
    : []),
])

// Map store state to form values
const formValues = computed({
  get: () => ({
    name: lampStore.state.lamp?.name ?? '',
    password: lampStore.state.lamp?.password ?? '',
    basePx: lampStore.state.base?.px ?? 36,
    baseBpp: lampStore.state.base?.bpp ?? 4,
    shadeBpp: lampStore.state.shade?.bpp ?? 4,
  }),
  set: () => {
    // Values are updated via individual handlers
  },
})

// Handle form value changes
const handleFormUpdate = (values: FormValues) => {
  if (values.name !== undefined && values.name !== formValues.value.name) {
    lampStore.updateLampName(values.name as string)
  }
  if (values.password !== undefined && values.password !== formValues.value.password) {
    lampStore.updateLampPassword(values.password as string)
  }
  if (values.basePx !== undefined && values.basePx !== formValues.value.basePx) {
    lampStore.updateBasePxCount(values.basePx as number)
  }
  if (values.baseBpp !== undefined && values.baseBpp !== formValues.value.baseBpp) {
    lampStore.updateBaseBpp(values.baseBpp as number)
  }
  if (values.shadeBpp !== undefined && values.shadeBpp !== formValues.value.shadeBpp) {
    lampStore.updateShadeBpp(values.shadeBpp as number)
  }
}

// LED pixel count for knockout grid
const ledCount = computed(() => lampStore.state.base?.px ?? 36)
</script>

<template>
  <section class="tab-panel" aria-label="Setup settings">
    <ComponentForm
      :fields="fields"
      :model-value="formValues"
      @update:model-value="handleFormUpdate"
      :show-button="false"
      :disabled="lampStore.disabled"
    >
      <!-- Per-Pixel Brightness Adjustment slot -->
      <template #knockoutPixels>
        <CollapsiblePanel label="Per-Pixel Brightness Adjustment">
          <div class="pixel-grid">
            <div
              v-for="ledIndex in Array.from(
                { length: ledCount },
                (_, i) => ledCount - i,
              )"
              :key="ledIndex - 1"
              class="pixel-row"
            >
              <label class="pixel-label">LED {{ ledIndex }}</label>
              <BrightnessSlider
                :model-value="lampStore.getKnockoutBrightness(ledIndex - 1)"
                @update:model-value="(value) => lampStore.updateKnockoutPixel(ledIndex - 1, value)"
                :id="`knockout-pixel-${ledIndex - 1}`"
                :min="0"
                :max="100"
                append="%"
                :disabled="lampStore.disabled"
              />
            </div>
          </div>
        </CollapsiblePanel>
      </template>
    </ComponentForm>

    <section class="home-mode">
      <h3 class="home-mode-heading">Home Mode</h3>
      <p class="home-mode-blurb">
        When the lamp joins your home WiFi, it switches to a separate brightness level — useful for ambient
        background brightness at home. WiFi is paused while you're using the app over Bluetooth.
      </p>

      <div class="home-mode-row">
        <div class="home-mode-network">
          <span class="home-mode-label">Network</span>
          <span class="home-mode-value">
            {{ homeSsid || 'Not configured' }}
            <span v-if="homeSsid" class="home-mode-status" :class="`home-mode-status--${lampStore.wifiState?.state ?? 'idle'}`">
              · {{ wifiStatusLabel }}
            </span>
          </span>
        </div>
        <div class="home-mode-actions">
          <button class="home-mode-btn home-mode-btn--primary" :disabled="lampStore.disabled" @click="homeSheetOpen = true">
            {{ homeSsid ? 'Change' : 'Choose network' }}
          </button>
          <button v-if="homeSsid" class="home-mode-btn home-mode-btn--danger" :disabled="lampStore.disabled" @click="onForget">
            Forget
          </button>
        </div>
      </div>

      <div class="home-mode-brightness">
        <span class="home-mode-label">Home Mode Brightness</span>
        <BrightnessSlider
          :model-value="homeBrightness"
          @update:model-value="onHomeBrightnessInput"
          @change="onHomeBrightnessChange"
          id="home-mode-brightness"
          :min="0"
          :max="100"
          append="%"
          :disabled="lampStore.disabled"
        />
      </div>
    </section>

    <HomeNetworkSheet :open="homeSheetOpen" @close="homeSheetOpen = false" />
  </section>
</template>

<style scoped>
.tab-panel {
  animation: fadeIn 0.3s ease-in-out;
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

/* Knockout Pixels Styles */
.pixel-grid {
  display: flex;
  flex-direction: column;
  gap: 10px;
  max-height: 400px;
  overflow-y: auto;
  padding: 8px;
  background: rgba(253, 253, 253, 0.02);
  border-radius: 8px;
  border: 1px solid var(--color-border);
}

.pixel-row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 8px;
  background: rgba(253, 253, 253, 0.02);
}

.pixel-label {
  min-width: 80px;
  font-size: 0.9rem;
  color: var(--brand-fog-grey);
  font-weight: 500;
}

.pixel-row .number-slider {
  flex: 1;
}

/* Home Mode */
.home-mode {
  margin-top: 20px;
  padding: 16px;
  background: var(--color-background-mute);
  border: 1px solid var(--color-border);
  border-radius: 12px;
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.home-mode-heading {
  margin: 0;
  font-size: 1rem;
  font-weight: 700;
  color: var(--brand-lamp-white);
}

.home-mode-blurb {
  margin: 0;
  font-size: 0.85rem;
  color: var(--brand-fog-grey);
  line-height: 1.4;
}

.home-mode-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
}

.home-mode-network {
  display: flex;
  flex-direction: column;
  gap: 2px;
  flex: 1;
  min-width: 160px;
}

.home-mode-label {
  font-size: 0.75rem;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  color: var(--brand-fog-grey);
  font-weight: 600;
}

.home-mode-value {
  color: var(--brand-lamp-white);
  font-weight: 600;
  font-size: 0.95rem;
}

.home-mode-status {
  font-weight: 500;
  color: var(--brand-fog-grey);
  font-size: 0.85rem;
}

.home-mode-status--connected { color: var(--color-success); }
.home-mode-status--connecting,
.home-mode-status--scanning { color: var(--brand-aurora-blue); }
.home-mode-status--failed { color: #ff6b6b; }

.home-mode-actions {
  display: flex;
  gap: 8px;
}

.home-mode-btn {
  padding: 8px 14px;
  border-radius: 8px;
  border: 1px solid var(--color-border);
  background: var(--color-background-soft);
  color: var(--brand-fog-grey);
  font-weight: 600;
  font-size: 0.85rem;
  cursor: pointer;
}

.home-mode-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.home-mode-btn--primary {
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-glow-pink));
  color: var(--brand-lamp-white);
  border-color: transparent;
}

.home-mode-btn--danger {
  border-color: rgba(255, 107, 107, 0.5);
  color: #ff6b6b;
}

.home-mode-brightness {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
</style>
