/**
 * Lamp Store
 *
 * Centralized state management for lamp settings, BLE GATT connection,
 * and control actions using Pinia.
 */

import { defineStore } from 'pinia'
import { ref, computed, readonly } from 'vue'
import { BleClient, ConnectionPriority } from '@capacitor-community/bluetooth-le'
import {
  authConnection,
  writeBrightness,
  writeShadeColors,
  writeBaseColors,
  writeBaseKnockout,
  writeExpressionTest,
  writeExpressionComplete,
  readSettingsBlob,
  writeSettingsBlob,
  subscribeStateNotify,
  unsubscribeStateNotify,
} from '@/services/bleControl'

export const MAX_LEDS_BASE = 50

// Types
export interface LampTarget {
  deviceId: string
  password?: string
}

interface KnockoutPixel {
  p: number
  b: number
}

interface LampSettings {
  name?: string
  brightness?: number
  homeMode?: boolean
  homeModeSSID?: string
  homeModeBrightness?: number
  password?: string
  advancedEnabled?: boolean
}

interface ShadeSettings {
  px?: number
  colors?: string[]
  bpp?: number
}

interface BaseSettings {
  px?: number
  colors?: string[]
  ac?: number
  knockout?: KnockoutPixel[]
  bpp?: number
}

interface ExpressionSettings {
  type: string
  enabled: boolean
  target: number
  colors: (string | null)[]
  intervalMin?: number
  intervalMax?: number
  durationMin?: number
  durationMax?: number
  shiftDurationMin?: number
  shiftDurationMax?: number
  fadeDuration?: number
  pulseSpeed?: number
}

export interface LampState {
  lamp?: LampSettings
  shade?: ShadeSettings
  base?: BaseSettings
  expressions?: ExpressionSettings[]
}

// Tab configuration
export const tabs = [
  { id: 'home', label: 'Home' },
  { id: 'expressions', label: 'Expressions' },
  { id: 'lamp-setup', label: 'Setup' },
  { id: 'info', label: 'Info' },
]

export const useLampStore = defineStore('lamp', () => {
  // State
  const state = ref<LampState>({})
  const originalState = ref<string>('')
  const loaded = ref(false)
  const saving = ref(false)
  const activeTab = ref('home')
  const target = ref<LampTarget | null>(null)

  // BLE connection state (exported as wsConnected for backward compat with pages)
  const wsConnected = ref(false)

  // Computed
  const hasChanges = computed(() => {
    return JSON.stringify(state.value) !== originalState.value
  })

  const disabled = computed(() => !wsConnected.value)

  // ── Helpers ───────────────────────────────────────────────────────────────────

  function deviceId(): string {
    if (!target.value) throw new Error('No target set')
    return target.value.deviceId
  }

  async function refreshState() {
    try {
      const json = await readSettingsBlob(deviceId())
      const data = JSON.parse(json) as LampState
      state.value = data
      // Only update originalState if we haven't made local changes since last save
      if (!hasChanges.value) {
        originalState.value = JSON.stringify(data)
      }
    } catch (err) {
      console.warn('[lamp] refreshState failed:', err)
    }
  }

  // ── State Update Methods ──────────────────────────────────────────────────────

  const updateLampName = (name: string) => {
    if (!state.value.lamp) state.value.lamp = {}
    state.value.lamp.name = name
  }

  const updateLampPassword = (password: string) => {
    if (!state.value.lamp) state.value.lamp = {}
    state.value.lamp.password = password
  }

  const updateBrightness = async (brightness: number) => {
    if (!state.value.lamp) state.value.lamp = {}
    state.value.lamp.brightness = brightness
    if (!state.value.lamp.homeMode && wsConnected.value) {
      try {
        await writeBrightness(deviceId(), brightness)
      } catch (err) {
        console.warn('[lamp] writeBrightness failed:', err)
      }
    }
  }

  const updateHomeMode = async (enabled: boolean) => {
    if (!state.value.lamp) state.value.lamp = {}
    state.value.lamp.homeMode = enabled
    if (wsConnected.value) {
      try {
        const v = enabled
          ? (state.value.lamp.homeModeBrightness ?? 80)
          : (state.value.lamp.brightness ?? 100)
        await writeBrightness(deviceId(), v)
      } catch (err) {
        console.warn('[lamp] writeBrightness (homeMode) failed:', err)
      }
    }
  }

  const updateHomeModeBrightness = async (brightness: number) => {
    if (!state.value.lamp) state.value.lamp = {}
    state.value.lamp.homeModeBrightness = brightness
    if (state.value.lamp.homeMode && wsConnected.value) {
      try {
        await writeBrightness(deviceId(), brightness)
      } catch (err) {
        console.warn('[lamp] writeBrightness (homeModeBrightness) failed:', err)
      }
    }
  }

  const updateHomeModeSSID = (ssid: string) => {
    if (!state.value.lamp) state.value.lamp = {}
    state.value.lamp.homeModeSSID = ssid
  }

  const updateShadeColors = async (colors: string[]) => {
    if (!state.value.shade) state.value.shade = {}
    state.value.shade.colors = colors
    if (wsConnected.value) {
      try {
        await writeShadeColors(deviceId(), colors)
      } catch (err) {
        console.warn('[lamp] writeShadeColors failed:', err)
      }
    }
  }

  const updateBaseColors = async (colors: string[]) => {
    if (!state.value.base) state.value.base = {}
    state.value.base.colors = colors
    if (wsConnected.value) {
      try {
        await writeBaseColors(deviceId(), colors)
      } catch (err) {
        console.warn('[lamp] writeBaseColors failed:', err)
      }
    }
  }

  const updateBaseActiveColor = (index: number) => {
    if (!state.value.base) state.value.base = {}
    state.value.base.ac = index
  }

  const updateBasePxCount = (count: number) => {
    if (!state.value.base) state.value.base = {}
    state.value.base.px = count
  }

  const updateAdvancedEnabled = (enabled: boolean) => {
    if (!state.value.lamp) state.value.lamp = {}
    state.value.lamp.advancedEnabled = enabled
  }

  const updateShadeBpp = (bpp: number) => {
    if (!state.value.shade) state.value.shade = {}
    state.value.shade.bpp = bpp
  }

  const updateBaseBpp = (bpp: number) => {
    if (!state.value.base) state.value.base = {}
    state.value.base.bpp = bpp
  }

  const updateKnockoutPixel = async (ledIndex: number, brightness: number) => {
    if (!state.value.base) state.value.base = {}
    if (!state.value.base.knockout) state.value.base.knockout = []

    const existingIndex = state.value.base.knockout.findIndex((kp) => kp.p === ledIndex)

    if (brightness === 100) {
      if (existingIndex !== -1) {
        state.value.base.knockout.splice(existingIndex, 1)
      }
    } else {
      if (existingIndex !== -1) {
        state.value.base.knockout[existingIndex].b = brightness
      } else {
        state.value.base.knockout.push({ p: ledIndex, b: brightness })
      }
    }

    if (wsConnected.value) {
      try {
        await writeBaseKnockout(deviceId(), ledIndex, brightness)
      } catch (err) {
        console.warn('[lamp] writeBaseKnockout failed:', err)
      }
    }
  }

  const getKnockoutBrightness = (ledIndex: number): number => {
    if (!state.value.base?.knockout) return 100
    const knockout = state.value.base.knockout.find((kp) => kp.p === ledIndex)
    return knockout ? knockout.b : 100
  }

  const updateExpressions = (expressions: ExpressionSettings[]) => {
    state.value.expressions = expressions
  }

  // Expression handlers
  const testExpression = async (type: string) => {
    if (!wsConnected.value) return
    try {
      await writeExpressionTest(deviceId(), type)
    } catch (err) {
      console.warn('[lamp] writeExpressionTest failed:', err)
    }
  }

  const testExpressionComplete = async () => {
    if (!wsConnected.value) return
    try {
      await writeExpressionComplete(deviceId())
    } catch (err) {
      console.warn('[lamp] writeExpressionComplete failed:', err)
    }
  }

  const previewExpressionColor = async (color: string, previewTarget: number) => {
    if (!wsConnected.value) return
    try {
      if (previewTarget === 1 || previewTarget === 3) {
        await writeShadeColors(deviceId(), [color])
      }
      if (previewTarget === 2 || previewTarget === 3) {
        await writeBaseColors(deviceId(), [color])
      }
    } catch (err) {
      console.warn('[lamp] previewExpressionColor failed:', err)
    }
  }

  const restoreColorsAfterPreview = async () => {
    if (!wsConnected.value) return
    try {
      if (state.value.shade?.colors) {
        await writeShadeColors(deviceId(), state.value.shade.colors)
      }
      if (state.value.base?.colors) {
        await writeBaseColors(deviceId(), state.value.base.colors)
      }
    } catch (err) {
      console.warn('[lamp] restoreColorsAfterPreview failed:', err)
    }
  }

  // Tab management
  const setActiveTab = (tabId: string) => {
    activeTab.value = tabId
  }

  // ── websocketSend: kept for API compat; no-op in BLE mode ────────────────────
  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  const websocketSend = (_action: Record<string, unknown>) => {
    // No-op: BLE mode sends directly via GATT helpers. This stub keeps any
    // residual callers from throwing at runtime.
  }

  // ── Save settings ─────────────────────────────────────────────────────────────

  const saveSettings = async () => {
    if (!hasChanges.value || saving.value) return

    saving.value = true
    wsConnected.value = false  // firmware reboots on write

    // Clean up knockout pixels
    if (!state.value.base) state.value.base = {}
    state.value.base.knockout =
      state.value.base?.knockout?.filter(
        ({ p, b }) => p !== undefined && p !== null && b < 100
      ) ?? []

    try {
      await writeSettingsBlob(deviceId(), state.value)
      originalState.value = JSON.stringify(state.value)
    } catch (err) {
      // Lamp reboots → GATT connection drops mid-write → throws. Expected.
      console.log('[lamp] writeSettingsBlob threw (likely reboot-induced disconnect):', err)
      originalState.value = JSON.stringify(state.value)
    }

    // Wait for firmware to reboot then reconnect
    if (target.value) {
      const savedTarget = { ...target.value }
      setTimeout(async () => {
        try {
          await initialize(savedTarget)
        } catch (err) {
          console.warn('[lamp] post-save reconnect failed:', err)
          saving.value = false
        }
      }, 5_000)
    } else {
      saving.value = false
    }
  }

  // Reset to original state
  const resetState = () => {
    if (originalState.value) {
      state.value = JSON.parse(originalState.value)
    }
  }

  // ── Initialize (connect + load) ───────────────────────────────────────────────

  const initialize = async (newTarget: LampTarget) => {
    target.value = newTarget

    // Make sure no stray LE scan is fighting with our GATT connect
    try { await BleClient.stopLEScan() } catch { /* ignore */ }

    // Connect with one automatic retry on transient failure (Android in
    // particular often needs a second attempt right after a scan).
    let connected = false
    let lastErr: unknown = null
    for (let attempt = 1; attempt <= 2 && !connected; attempt++) {
      try {
        await BleClient.connect(newTarget.deviceId, () => {
          wsConnected.value = false
          console.log('[lamp] BLE disconnected')
        })
        connected = true
      } catch (err) {
        lastErr = err
        console.warn(`[lamp] BleClient.connect attempt ${attempt} failed:`, err)
        if (attempt < 2) await new Promise((r) => setTimeout(r, 1000))
      }
    }
    if (!connected) {
      console.warn('[lamp] BleClient.connect gave up:', lastErr)
      loaded.value = true
      return
    }

    // Best-effort high-priority connection (Android only — reduces GATT latency)
    try {
      await BleClient.requestConnectionPriority(newTarget.deviceId, ConnectionPriority.CONNECTION_PRIORITY_HIGH)
    } catch {
      // Not available on all platforms/plugin versions — ignore
    }

    // Read current settings
    try {
      const json = await readSettingsBlob(newTarget.deviceId)
      const data = JSON.parse(json) as LampState
      state.value = data
      originalState.value = JSON.stringify(data)
    } catch (err) {
      console.warn('[lamp] readSettingsBlob failed:', err)
    }

    // Auth if password provided and lamp has password set
    if (newTarget.password && state.value.lamp?.password) {
      try {
        await authConnection(newTarget.deviceId, newTarget.password)
      } catch (err) {
        console.warn('[lamp] authConnection failed:', err)
      }
    }

    // Subscribe to state notifications
    try {
      await subscribeStateNotify(newTarget.deviceId, () => {
        void refreshState()
      })
    } catch (err) {
      console.warn('[lamp] subscribeStateNotify failed:', err)
    }

    wsConnected.value = true
    loaded.value = true
    saving.value = false
  }

  // ── Cleanup ───────────────────────────────────────────────────────────────────

  const cleanup = async () => {
    if (!target.value) return
    const id = target.value.deviceId
    try {
      await unsubscribeStateNotify(id)
    } catch {
      // ignore
    }
    try {
      await BleClient.disconnect(id)
    } catch {
      // ignore — may already be disconnected
    }
    wsConnected.value = false
  }

  const setTarget = async (newTarget: LampTarget) => {
    await cleanup()
    state.value = {}
    originalState.value = JSON.stringify({})
    loaded.value = false
    await initialize(newTarget)
  }

  return {
    // State
    state,
    loaded,
    saving,
    wsConnected,
    disabled,
    hasChanges,
    activeTab,

    // Lamp update methods
    updateLampName,
    updateLampPassword,
    updateBrightness,
    updateHomeMode,
    updateHomeModeBrightness,
    updateHomeModeSSID,

    // Color update methods
    updateShadeColors,
    updateBaseColors,
    updateBaseActiveColor,

    // Base/LED methods
    updateBasePxCount,
    updateKnockoutPixel,
    getKnockoutBrightness,
    updateAdvancedEnabled,
    updateShadeBpp,
    updateBaseBpp,

    // Expression methods
    updateExpressions,
    testExpression,
    testExpressionComplete,
    previewExpressionColor,
    restoreColorsAfterPreview,

    // Navigation
    setActiveTab,

    // Actions
    saveSettings,
    resetState,
    initialize,
    cleanup,
    setTarget,
    websocketSend,

    // Target (read-only ref)
    target: readonly(target),
  }
})
