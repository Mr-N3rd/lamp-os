/**
 * BLE GATT control service helpers.
 *
 * UUIDs and payload formats mirror ble_control.hpp / ble_control.cpp exactly.
 */

import { BleClient } from '@capacitor-community/bluetooth-le'

// ── Service & characteristic UUIDs ───────────────────────────────────────────

export const CONTROL_SERVICE_UUID      = '5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'

export const CHAR_AUTH                 = '5f64f4d1-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_BRIGHTNESS           = '5f64f4d2-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_SHADE_COLORS         = '5f64f4d3-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_BASE_COLORS          = '5f64f4d4-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_BASE_KNOCKOUT        = '5f64f4d5-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_EXPRESSION_TEST      = '5f64f4d6-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_SETTINGS_BLOB        = '5f64f4d7-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_STATE_NOTIFY         = '5f64f4d8-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_EXPRESSION_OP        = '5f64f4d9-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_WIFI_OP              = '5f64f4da-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_WIFI_STATE           = '5f64f4db-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_LAMP_SECTION         = '5f64f4dc-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_BASE_SECTION         = '5f64f4dd-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_SHADE_SECTION        = '5f64f4de-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_EXPR_SECTION         = '5f64f4df-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_HOME_SECTION         = '5f64f4e0-d6d9-4a44-9b3f-3a8d6f7e6b40'

export interface WifiScanResult { ssid: string; rssi: number; encrypted: boolean }
export interface WifiState {
  state: 'idle' | 'scanning' | 'connecting' | 'connected' | 'failed'
  ssid?: string
  ip?: string
  lastError?: string  // 'auth' | 'noap' | 'scan' | 'timeout' | other
  scanResults?: WifiScanResult[]
}

// Firmware negotiates up to 512 — request the same on the app side.
export const TARGET_MTU = 512

// ── Payload helpers ───────────────────────────────────────────────────────────

function textToDataView(s: string): DataView {
  const bytes = new TextEncoder().encode(s)
  return new DataView(bytes.buffer)
}

function u8ToDataView(value: number): DataView {
  const buf = new Uint8Array([value & 0xff])
  return new DataView(buf.buffer)
}

function twoU8ToDataView(a: number, b: number): DataView {
  const buf = new Uint8Array([a & 0xff, b & 0xff])
  return new DataView(buf.buffer)
}

// ── Auth ──────────────────────────────────────────────────────────────────────

/**
 * Write the lamp password to the auth characteristic.
 * Must be called (and awaited) before any other control writes when the lamp
 * has a password set.
 *
 * write-with-response so we get a GATT ack (not a GATT error — the firmware
 * does not return an ATT error on wrong password; it just ignores subsequent
 * writes). The caller can tell auth failed only by trying a subsequent write
 * and seeing no effect, or by reading settings_blob first to know whether a
 * password field is set.
 */
export async function authConnection(deviceId: string, password: string): Promise<void> {
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_AUTH, textToDataView(password))
}

// ── Control writes ────────────────────────────────────────────────────────────

export async function writeBrightness(deviceId: string, value: number): Promise<void> {
  const clamped = Math.max(0, Math.min(100, Math.round(value)))
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_BRIGHTNESS, u8ToDataView(clamped))
}

export async function writeShadeColors(deviceId: string, colors: string[]): Promise<void> {
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_SHADE_COLORS, textToDataView(JSON.stringify(colors)))
}

export async function writeBaseColors(deviceId: string, colors: string[]): Promise<void> {
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_BASE_COLORS, textToDataView(JSON.stringify(colors)))
}

export async function writeBaseKnockout(deviceId: string, pixelIndex: number, brightness: number): Promise<void> {
  const clampedB = Math.max(0, Math.min(100, Math.round(brightness)))
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_BASE_KNOCKOUT, twoU8ToDataView(pixelIndex, clampedB))
}

export async function writeExpressionTest(deviceId: string, type: string, target: number): Promise<void> {
  const payload = JSON.stringify({ a: 'test_expression', type, target })
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_EXPRESSION_TEST, textToDataView(payload))
}

export async function writeExpressionUpsert(deviceId: string, entry: Record<string, unknown>): Promise<void> {
  const payload = JSON.stringify({ op: 'upsert', entry })
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_EXPRESSION_OP, textToDataView(payload))
}

export async function writeExpressionRemove(deviceId: string, type: string, target: number): Promise<void> {
  const payload = JSON.stringify({ op: 'remove', type, target })
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_EXPRESSION_OP, textToDataView(payload))
}

export async function writeExpressionComplete(
  deviceId: string,
  shadeColors: string[],
  baseColors: string[],
): Promise<void> {
  const payload = JSON.stringify({
    a: 'test_expression_complete',
    shadeColors,
    baseColors,
  })
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_EXPRESSION_TEST, textToDataView(payload))
}

// ── WiFi (home mode) ──────────────────────────────────────────────────────────

export async function writeWifiScan(deviceId: string): Promise<void> {
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_WIFI_OP,
    textToDataView(JSON.stringify({ op: 'scan' })))
}

export async function writeWifiConnect(deviceId: string, ssid: string, password: string): Promise<void> {
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_WIFI_OP,
    textToDataView(JSON.stringify({ op: 'connect', ssid, password })))
}

export async function writeWifiForget(deviceId: string): Promise<void> {
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_WIFI_OP,
    textToDataView(JSON.stringify({ op: 'forget' })))
}

export async function readWifiState(deviceId: string): Promise<WifiState> {
  const dv = await BleClient.read(deviceId, CONTROL_SERVICE_UUID, CHAR_WIFI_STATE)
  const text = new TextDecoder().decode(dv).trim()
  if (!text) return { state: 'idle' }
  try {
    return JSON.parse(text) as WifiState
  } catch (err) {
    console.warn('[ble] wifi state JSON parse failed:', err, 'payload:', text)
    return { state: 'idle' }
  }
}

export async function subscribeWifiState(
  deviceId: string,
  onUpdate: (state: WifiState) => void,
): Promise<void> {
  await BleClient.startNotifications(deviceId, CONTROL_SERVICE_UUID, CHAR_WIFI_STATE, (dv) => {
    try {
      onUpdate(JSON.parse(new TextDecoder().decode(dv)) as WifiState)
    } catch (err) {
      console.warn('[ble] wifi state parse failed:', err)
    }
  })
}

export async function unsubscribeWifiState(deviceId: string): Promise<void> {
  try { await BleClient.stopNotifications(deviceId, CONTROL_SERVICE_UUID, CHAR_WIFI_STATE) } catch { /* ignore */ }
}

// ── Settings sections ─────────────────────────────────────────────────────────

/**
 * Write the full config JSON to the lamp.
 * ⚠️  Triggers an immediate lamp reboot — the GATT connection will drop.
 * Caller must handle the resulting disconnect gracefully.
 *
 * Reads happen via per-section characteristics (see readLampSection etc.);
 * the settings_blob path is write-only on the new firmware.
 */
export async function writeSettingsBlob(deviceId: string, config: unknown): Promise<void> {
  const json = JSON.stringify(config)
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_SETTINGS_BLOB, textToDataView(json))
}

function parseSection<T>(dv: DataView, label: string, fallback: T): T {
  const text = new TextDecoder().decode(dv).trim()
  if (!text) return fallback
  try {
    return JSON.parse(text) as T
  } catch (err) {
    console.warn(`[ble] ${label} parse failed:`, err, 'payload:', text)
    return fallback
  }
}

async function readSection<T>(
  deviceId: string,
  charUuid: string,
  label: string,
  fallback: T,
): Promise<T> {
  const dv = await BleClient.read(deviceId, CONTROL_SERVICE_UUID, charUuid)
  return parseSection(dv, label, fallback)
}

export async function readLampSection(deviceId: string): Promise<Record<string, unknown>> {
  return readSection(deviceId, CHAR_LAMP_SECTION, 'lamp section', {})
}
export async function readBaseSection(deviceId: string): Promise<Record<string, unknown>> {
  return readSection(deviceId, CHAR_BASE_SECTION, 'base section', {})
}
export async function readShadeSection(deviceId: string): Promise<Record<string, unknown>> {
  return readSection(deviceId, CHAR_SHADE_SECTION, 'shade section', {})
}
export async function readExprSection(deviceId: string): Promise<unknown[]> {
  return readSection<unknown[]>(deviceId, CHAR_EXPR_SECTION, 'expressions section', [])
}
export async function readHomeSection(deviceId: string): Promise<Record<string, unknown>> {
  return readSection(deviceId, CHAR_HOME_SECTION, 'home-mode section', {})
}

// ── Per-section notifications ─────────────────────────────────────────────────

export interface SectionCallbacks {
  onLamp?: (data: Record<string, unknown>) => void
  onBase?: (data: Record<string, unknown>) => void
  onShade?: (data: Record<string, unknown>) => void
  onExpressions?: (data: unknown[]) => void
  onHomeMode?: (data: Record<string, unknown>) => void
}

async function subscribe<T>(
  deviceId: string,
  charUuid: string,
  label: string,
  fallback: T,
  handler: ((data: T) => void) | undefined,
): Promise<void> {
  if (!handler) return
  await BleClient.startNotifications(deviceId, CONTROL_SERVICE_UUID, charUuid, (dv) => {
    handler(parseSection(dv, label, fallback))
  })
}

export async function subscribeSectionNotifies(
  deviceId: string,
  callbacks: SectionCallbacks,
): Promise<void> {
  await Promise.all([
    subscribe(deviceId, CHAR_LAMP_SECTION,  'lamp section',        {}, callbacks.onLamp),
    subscribe(deviceId, CHAR_BASE_SECTION,  'base section',        {}, callbacks.onBase),
    subscribe(deviceId, CHAR_SHADE_SECTION, 'shade section',       {}, callbacks.onShade),
    subscribe(deviceId, CHAR_EXPR_SECTION,  'expressions section', [] as unknown[], callbacks.onExpressions),
    subscribe(deviceId, CHAR_HOME_SECTION,  'home-mode section',   {}, callbacks.onHomeMode),
  ])
}

export async function unsubscribeSectionNotifies(deviceId: string): Promise<void> {
  const stops = [
    CHAR_LAMP_SECTION,
    CHAR_BASE_SECTION,
    CHAR_SHADE_SECTION,
    CHAR_EXPR_SECTION,
    CHAR_HOME_SECTION,
  ].map(async (uuid) => {
    try { await BleClient.stopNotifications(deviceId, CONTROL_SERVICE_UUID, uuid) }
    catch { /* not subscribed or disconnected */ }
  })
  await Promise.all(stops)
}
