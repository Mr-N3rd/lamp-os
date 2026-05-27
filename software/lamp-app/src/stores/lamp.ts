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
  writeExpressionUpsert,
  writeExpressionRemove,
  writeWifiScan,
  writeWifiConnect,
  writeWifiForget,
  readWifiState,
  subscribeWifiState,
  unsubscribeWifiState,
  writeSettingsBlob,
  readLampSection,
  readBaseSection,
  readShadeSection,
  readExprSection,
  readHomeSection,
  subscribeSectionNotifies,
  unsubscribeSectionNotifies,
  type WifiState,
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

interface HomeModeSettings {
  ssid?: string
  password?: string
  brightness?: number
}

export interface LampState {
  lamp?: LampSettings
  shade?: ShadeSettings
  base?: BaseSettings
  expressions?: ExpressionSettings[]
  homeMode?: HomeModeSettings
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

  // Apply a section update from a notify (or background lazy-load). Only refreshes
  // originalState when the user hasn't made unsaved local changes, so an
  // incoming push doesn't silently discard their pending edits.
  function applySection<K extends keyof LampState>(key: K, data: LampState[K]) {
    state.value = { ...state.value, [key]: data }
    if (!hasChanges.value) {
      originalState.value = JSON.stringify(state.value)
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

  const cleanExpressionForWire = (entry: ExpressionSettings): Record<string, unknown> => {
    const out: Record<string, unknown> = { ...entry }
    if (Array.isArray(entry.colors)) {
      out.colors = entry.colors.filter((c): c is string => typeof c === 'string')
    }
    return out
  }

  const testExpression = async (type: string, target: number) => {
    if (!wsConnected.value) return
    try {
      await writeExpressionTest(deviceId(), type, target)
    } catch (err) {
      console.warn('[lamp] writeExpressionTest failed:', err)
    }
  }

  const addExpression = async (entry: ExpressionSettings) => {
    if (!wsConnected.value) throw new Error('Not connected')
    await writeExpressionUpsert(deviceId(), cleanExpressionForWire(entry))
    state.value.expressions = [...(state.value.expressions ?? []), entry]
  }

  const updateExpression = async (
    prevKey: { type: string; target: number },
    entry: ExpressionSettings,
  ) => {
    if (!wsConnected.value) throw new Error('Not connected')
    if (prevKey.type !== entry.type || prevKey.target !== entry.target) {
      await writeExpressionRemove(deviceId(), prevKey.type, prevKey.target)
    }
    await writeExpressionUpsert(deviceId(), cleanExpressionForWire(entry))
    const next = (state.value.expressions ?? []).filter(
      (e) => !(e.type === prevKey.type && e.target === prevKey.target),
    )
    next.push(entry)
    state.value.expressions = next
  }

  const removeExpression = async (type: string, target: number) => {
    if (!wsConnected.value) throw new Error('Not connected')
    await writeExpressionRemove(deviceId(), type, target)
    state.value.expressions = (state.value.expressions ?? []).filter(
      (e) => !(e.type === type && e.target === target),
    )
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

  const previewExpressionColor = (color: string, previewTarget: number) => {
    if (!wsConnected.value) return
    if (previewTarget === 1 || previewTarget === 3) {
      scheduleWrite('shadeColors', [color], (v) => writeShadeColors(deviceId(), v as string[]))
    }
    if (previewTarget === 2 || previewTarget === 3) {
      scheduleWrite('baseColors', [color], (v) => writeBaseColors(deviceId(), v as string[]))
    }
  }

  const wifiState = ref<WifiState | null>(null)

  const wifiScan = async () => {
    if (!wsConnected.value) return
    try { await writeWifiScan(deviceId()) } catch (err) {
      console.warn('[lamp] writeWifiScan failed:', err)
    }
  }

  const wifiConnect = async (ssid: string, password: string) => {
    if (!wsConnected.value) throw new Error('Not connected')
    if (!state.value.homeMode) state.value.homeMode = {}
    state.value.homeMode.ssid = ssid
    state.value.homeMode.password = password
    await writeWifiConnect(deviceId(), ssid, password)
  }

  const wifiForget = async () => {
    if (!wsConnected.value) throw new Error('Not connected')
    if (!state.value.homeMode) state.value.homeMode = {}
    state.value.homeMode.ssid = ''
    state.value.homeMode.password = ''
    await writeWifiForget(deviceId())
  }

  // Preview the home-mode brightness while the user is dragging the slider.
  // The lamp is in regular brightness (because BLE is connected = WiFi paused),
  // so this just writes the previewed level via the existing throttle. When
  // the user releases the slider, restoreBrightness writes the saved
  // lamp.brightness back.
  const previewHomeBrightness = (level: number) => {
    if (!wsConnected.value) return
    scheduleWrite('brightness', level, (v) => writeBrightness(deviceId(), v as number))
  }

  const restoreBrightness = () => {
    if (!wsConnected.value) return
    const level = state.value.lamp?.brightness ?? 100
    scheduleWrite('brightness', level, (v) => writeBrightness(deviceId(), v as number))
  }

  const updateHomeModeBrightness = (level: number) => {
    if (!state.value.homeMode) state.value.homeMode = {}
    state.value.homeMode.brightness = level
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

  const reconnectTimer = ref<ReturnType<typeof setTimeout> | null>(null)
  const reconnectAttempts = ref(0)
  const reconnecting = ref(false)
  const RECONNECT_DELAYS_MS = [500, 1000, 2000, 4000, 8000]

  // Post-connect bootstrap: priority bump, eager-load home-tab sections
  // (lamp/base/shade), flip the UI to interactive, then lazy-load
  // expressions + home-mode in the background. Per-section notifies replace
  // the legacy single state-notify (clients no longer re-read a monolithic
  // settings blob).
  const bootstrap = async (t: LampTarget) => {
    BleClient.requestConnectionPriority(t.deviceId, ConnectionPriority.CONNECTION_PRIORITY_HIGH)
      .then(() => dlog('requestConnectionPriority HIGH OK'))
      .catch((e) => dlog(`requestConnectionPriority skipped: ${e instanceof Error ? e.message : String(e)}`))

    const next: LampState = {}
    try {
      dlog('reading lamp/base/shade sections…')
      const [lampSec, baseSec, shadeSec] = await Promise.all([
        readLampSection(t.deviceId),
        readBaseSection(t.deviceId),
        readShadeSection(t.deviceId),
      ])
      next.lamp = lampSec as LampSettings
      next.base = baseSec as BaseSettings
      next.shade = shadeSec as ShadeSettings
      dlog('home-tab sections OK')
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      connectionError.value = `Couldn't read lamp settings: ${msg}`
      dlog(`home-tab sections failed: ${msg}`)
    }

    state.value = next
    originalState.value = JSON.stringify(next)
    wsConnected.value = true
    loaded.value = true
    saving.value = false
    dlog('bootstrap home-tab ready; wsConnected=true loaded=true')

    if (t.password) {
      authConnection(t.deviceId, t.password)
        .then(() => dlog('authConnection OK'))
        .catch((err) => dlog(`authConnection failed: ${err instanceof Error ? err.message : String(err)}`))
    }

    // Lazy-load the tabs the user hasn't opened yet. By the time they tap
    // Expressions / Setup, the data is already in state.
    Promise.all([
      readExprSection(t.deviceId).then((d) => applySection('expressions', d as ExpressionSettings[])),
      readHomeSection(t.deviceId).then((d) => applySection('homeMode', d as HomeModeSettings)),
    ])
      .then(() => dlog('lazy sections OK'))
      .catch((err) => dlog(`lazy sections failed: ${err instanceof Error ? err.message : String(err)}`))

    subscribeSectionNotifies(t.deviceId, {
      onLamp:        (d) => applySection('lamp',        d as LampSettings),
      onBase:        (d) => applySection('base',        d as BaseSettings),
      onShade:       (d) => applySection('shade',       d as ShadeSettings),
      onExpressions: (d) => applySection('expressions', d as ExpressionSettings[]),
      onHomeMode:    (d) => applySection('homeMode',    d as HomeModeSettings),
    })
      .then(() => dlog('subscribeSectionNotifies OK'))
      .catch((err) => dlog(`subscribeSectionNotifies failed: ${err instanceof Error ? err.message : String(err)}`))

    readWifiState(t.deviceId)
      .then((ws) => { wifiState.value = ws })
      .catch((err) => dlog(`readWifiState failed: ${err instanceof Error ? err.message : String(err)}`))
    subscribeWifiState(t.deviceId, (ws) => { wifiState.value = ws })
      .then(() => dlog('subscribeWifiState OK'))
      .catch((err) => dlog(`subscribeWifiState failed: ${err instanceof Error ? err.message : String(err)}`))
  }

  const onLinkDisconnect = () => {
    wsConnected.value = false
    dlog('BLE disconnected (callback fired) — scheduling reconnect')
    scheduleReconnect()
  }

  const scheduleReconnect = () => {
    if (!target.value) return
    if (reconnectTimer.value) return
    reconnecting.value = true
    const delay = RECONNECT_DELAYS_MS[Math.min(reconnectAttempts.value, RECONNECT_DELAYS_MS.length - 1)]
    reconnectTimer.value = setTimeout(async () => {
      reconnectTimer.value = null
      if (!target.value) {
        reconnecting.value = false
        return
      }
      reconnectAttempts.value++
      const t = target.value
      try {
        dlog(`reconnect attempt ${reconnectAttempts.value}…`)
        await BleClient.connect(t.deviceId, onLinkDisconnect, { skipDescriptorDiscovery: true })
        dlog('reconnect connect OK')
        await bootstrap(t)
        reconnectAttempts.value = 0
        reconnecting.value = false
      } catch (err) {
        dlog(`reconnect failed: ${err instanceof Error ? err.message : String(err)}`)
        scheduleReconnect()
      }
    }, delay)
  }

  const initialize = async (newTarget: LampTarget) => {
    target.value = newTarget
    connectionError.value = null
    reconnectAttempts.value = 0
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
        await BleClient.connect(newTarget.deviceId, onLinkDisconnect, { skipDescriptorDiscovery: true })
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

    await bootstrap(newTarget)
  }

  // ── Cleanup ───────────────────────────────────────────────────────────────────

  const cleanup = async () => {
    if (reconnectTimer.value) {
      clearTimeout(reconnectTimer.value)
      reconnectTimer.value = null
    }
    reconnecting.value = false
    reconnectAttempts.value = 0
    if (!target.value) return
    const id = target.value.deviceId
    try {
      await unsubscribeSectionNotifies(id)
    } catch {
      // ignore
    }
    try {
      await unsubscribeWifiState(id)
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
    reconnecting,
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
    addExpression,
    updateExpression,
    removeExpression,

    // Home mode (WiFi)
    wifiState,
    wifiScan,
    wifiConnect,
    wifiForget,
    previewHomeBrightness,
    restoreBrightness,
    updateHomeModeBrightness,

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
