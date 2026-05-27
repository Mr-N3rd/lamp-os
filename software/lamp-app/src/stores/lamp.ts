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
  const connectionError = ref<string | null>(null)

  // In-app debug log — rolling buffer of init/connect steps so user can see
  // what's happening without chrome://inspect. Keep last 30 entries.
  const debugLog = ref<Array<{ t: number; msg: string }>>([])
  function dlog(msg: string) {
    const entry = { t: Date.now(), msg }
    debugLog.value = [...debugLog.value, entry].slice(-30)
    console.log('[lamp]', msg)
  }

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

  // Coalescing write throttle. Slider drags fire updates at ~60 Hz; sending
  // every one as a BLE write floods the lamp's NimBLE host task and starves
  // its loop task, eventually crashing the lamp. We update state.value
  // immediately (UI stays snappy) but rate-limit the actual BLE write to
  // ~16 Hz, latest-value-wins.
  const WRITE_THROTTLE_MS = 60
  type Pending = { value: unknown; flushAt: number; timer: ReturnType<typeof setTimeout> | null }
  const pendingWrites = new Map<string, Pending>()

  const scheduleWrite = (key: string, value: unknown, doWrite: (v: unknown) => Promise<void>) => {
    const existing = pendingWrites.get(key)
    const now = Date.now()
    if (existing) {
      existing.value = value
      return
    }
    const delay = WRITE_THROTTLE_MS
    const entry: Pending = {
      value,
      flushAt: now + delay,
      timer: setTimeout(async () => {
        const e = pendingWrites.get(key)
        if (!e) return
        pendingWrites.delete(key)
        try { await doWrite(e.value) } catch (err) {
          console.warn(`[lamp] ${key} write failed:`, err)
        }
      }, delay),
    }
    pendingWrites.set(key, entry)
  }

  const updateBrightness = async (brightness: number) => {
    if (!state.value.lamp) state.value.lamp = {}
    state.value.lamp.brightness = brightness
    if (wsConnected.value) {
      scheduleWrite('brightness', brightness, (v) => writeBrightness(deviceId(), v as number))
    }
  }

  const updateShadeColors = async (colors: string[]) => {
    if (!state.value.shade) state.value.shade = {}
    state.value.shade.colors = colors
    if (wsConnected.value) {
      scheduleWrite('shadeColors', colors, (v) => writeShadeColors(deviceId(), v as string[]))
    }
  }

  const updateBaseColors = async (colors: string[]) => {
    if (!state.value.base) state.value.base = {}
    state.value.base.colors = colors
    if (wsConnected.value) {
      scheduleWrite('baseColors', colors, (v) => writeBaseColors(deviceId(), v as string[]))
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
    const expr = state.value.expressions?.find((e) => e.type === type)
    if (!expr) return
    const cleaned: Record<string, unknown> = { ...expr }
    if (Array.isArray(expr.colors)) {
      cleaned.colors = expr.colors.filter((c): c is string => typeof c === 'string')
    }
    try {
      await writeExpressionTest(deviceId(), cleaned)
    } catch (err) {
      console.warn('[lamp] writeExpressionTest failed:', err)
    }
  }

  const testExpressionComplete = async () => {
    if (!wsConnected.value) return
    try {
      const shadeColors = state.value.shade?.colors ?? []
      const baseColors = state.value.base?.colors ?? []
      await writeExpressionComplete(deviceId(), shadeColors, baseColors)
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

    dlog(`writeSettingsBlob: ${JSON.stringify(state.value).length} bytes`)
    try {
      await writeSettingsBlob(deviceId(), state.value)
      originalState.value = JSON.stringify(state.value)
      dlog('writeSettingsBlob OK')
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      dlog(`writeSettingsBlob threw: ${msg}`)
      // "Not connected" / "Device disconnected" → likely the lamp rebooted
      // (expected on settings_blob writes which trigger ESP.restart()).
      // Other errors (timeout, ATT errors) mean the write didn't reach.
      const looksLikeReboot = msg.toLowerCase().includes('not connected') ||
                              msg.toLowerCase().includes('disconnect')
      if (looksLikeReboot) {
        dlog('treating as reboot-induced disconnect (expected)')
        originalState.value = JSON.stringify(state.value)
      } else {
        connectionError.value = `Save failed: ${msg}`
        saving.value = false
        wsConnected.value = false
        return  // Don't try to reconnect — let user retry manually
      }
    }

    // Wait for firmware to reboot then reconnect
    if (target.value) {
      const savedTarget = { ...target.value }
      setTimeout(async () => {
        try {
          // Tear down the stale GATT handle on Android before reconnecting.
          // Without this the OS keeps the old (now-broken) BluetoothGatt
          // object and the subsequent connect fails or hits a stale cache.
          await cleanup()
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
    connectionError.value = null
    dlog(`initialize: target=${newTarget.deviceId} password=${newTarget.password ? '<set>' : '<none>'}`)

    // Make sure no stray LE scan is fighting with our GATT connect
    try {
      await BleClient.stopLEScan()
      dlog('stopLEScan OK')
    } catch (e) {
      dlog(`stopLEScan threw: ${e instanceof Error ? e.message : String(e)}`)
    }

    // Connect with one automatic retry on transient failure (Android in
    // particular often needs a second attempt right after a scan).
    let connected = false
    let lastErr: unknown = null
    for (let attempt = 1; attempt <= 2 && !connected; attempt++) {
      try {
        dlog(`BleClient.connect attempt ${attempt}…`)
        await BleClient.connect(newTarget.deviceId, () => {
          wsConnected.value = false
          dlog('BLE disconnected (callback fired)')
        })
        connected = true
        dlog(`BleClient.connect attempt ${attempt} OK`)
      } catch (err) {
        lastErr = err
        dlog(`BleClient.connect attempt ${attempt} failed: ${err instanceof Error ? err.message : String(err)}`)
        if (attempt < 2) await new Promise((r) => setTimeout(r, 1000))
      }
    }
    if (!connected) {
      const msg = lastErr instanceof Error ? lastErr.message : String(lastErr)
      connectionError.value = `Couldn't connect to lamp: ${msg}`
      dlog(`giving up: ${msg}`)
      loaded.value = true
      return
    }

    // Best-effort high-priority connection (Android only — reduces GATT latency)
    try {
      await BleClient.requestConnectionPriority(newTarget.deviceId, ConnectionPriority.CONNECTION_PRIORITY_HIGH)
      dlog('requestConnectionPriority HIGH OK')
    } catch (e) {
      dlog(`requestConnectionPriority skipped: ${e instanceof Error ? e.message : String(e)}`)
    }

    // Force fresh service discovery (Android caches services across sessions;
    // if firmware changed, the cache may be stale).
    try {
      dlog('discoverServices…')
      await BleClient.discoverServices(newTarget.deviceId)
      dlog('discoverServices OK')
    } catch (e) {
      dlog(`discoverServices threw: ${e instanceof Error ? e.message : String(e)}`)
    }

    // Log discovered services so we can see what the app actually knows about
    try {
      const services = await BleClient.getServices(newTarget.deviceId)
      const uuids = services.map((s) => s.uuid)
      dlog(`getServices: ${uuids.length} services [${uuids.join(', ')}]`)
    } catch (e) {
      dlog(`getServices threw: ${e instanceof Error ? e.message : String(e)}`)
    }

    // Read current settings
    try {
      dlog('readSettingsBlob…')
      const json = await readSettingsBlob(newTarget.deviceId)
      dlog(`readSettingsBlob OK (${json.length} bytes)`)
      const data = JSON.parse(json) as LampState
      state.value = data
      originalState.value = JSON.stringify(data)
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      connectionError.value = `Couldn't read lamp settings: ${msg}`
      dlog(`readSettingsBlob failed: ${msg}`)
    }

    // Auth if the user gave us a password. We trust the inventory entry — if
    // the lamp redacts the password in its settings JSON for security (future
    // change), we shouldn't gate auth on what's in state.value.lamp.password.
    if (newTarget.password) {
      try {
        dlog('authConnection…')
        await authConnection(newTarget.deviceId, newTarget.password)
        dlog('authConnection OK')
      } catch (err) {
        dlog(`authConnection failed: ${err instanceof Error ? err.message : String(err)}`)
      }
    }

    // Subscribe to state notifications
    try {
      dlog('subscribeStateNotify…')
      await subscribeStateNotify(newTarget.deviceId, () => {
        void refreshState()
      })
      dlog('subscribeStateNotify OK')
    } catch (err) {
      dlog(`subscribeStateNotify failed: ${err instanceof Error ? err.message : String(err)}`)
    }

    wsConnected.value = true
    loaded.value = true
    saving.value = false
    dlog('initialize complete; wsConnected=true loaded=true')
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
    connectionError,
    debugLog,
    disabled,
    hasChanges,
    activeTab,

    // Lamp update methods
    updateLampName,
    updateLampPassword,
    updateBrightness,

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
