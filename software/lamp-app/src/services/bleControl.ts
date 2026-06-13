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
export const CHAR_MQTT_SECTION         = '5f64f4e1-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_MQTT_OP              = '5f64f4e2-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_NEARBY_LAMPS         = '5f64f4e3-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const CHAR_REMOTE_OP            = '5f64f4e4-d6d9-4a44-9b3f-3a8d6f7e6b40'

export interface WifiScanResult { ssid: string; rssi: number; encrypted: boolean }
export interface WifiState {
  state: 'idle' | 'scanning' | 'connecting' | 'connected' | 'failed'
  ssid?: string
  ip?: string
  lastError?: string  // 'auth' | 'noap' | 'scan' | 'timeout' | other
  scanResults?: WifiScanResult[]
}

/**
 * Smart-home / Home Assistant MQTT broker config. `password` is `'********'`
 * when the firmware has a value stored — UI should treat that as "unchanged"
 * and only send back a real string when the user types a new password.
 */
export interface MqttConfig {
  enabled: boolean
  brokerHost: string
  brokerPort: number
  username: string
  password: string  // may be '********' from firmware (= stored, unchanged)
  topicPrefix: string
}

/**
 * One lamp the locally-paired lamp can hear, via either transport.
 *
 * - `viaBle`     — heard via legacy BLE manufacturer-data scan within
 *                  LAMP_TRANSPORT_RECENCY_MS. Triggers SocialBehavior on
 *                  the lamp.
 * - `viaEspNow`  — heard via ESP-NOW HELLO within the same window. Means
 *                  it's a new-firmware lamp and is remote-configurable.
 * - `mac`        — present only when `viaEspNow` has been true at some
 *                  point. Legacy lamps never set this.
 * - `lastSeenMs` — lamp-side `millis()` at most recent sighting on either
 *                  transport. Use only for relative freshness comparisons
 *                  between entries from the same lamp, not against
 *                  Date.now().
 */
export interface NearbyLamp {
  name: string
  shade: [number, number, number, number]  // RGBW
  base:  [number, number, number, number]  // RGBW
  lastSeenMs: number
  viaBle: boolean
  viaEspNow: boolean
  mac?: string
}

/**
 * Payload shapes by `char` field, mirroring local BLE writes. The `targetMac`
 * is added by writeRemoteOp.
 */
export type RemoteOpPayload =
  | { char: 'brightness'; value: number }
  | { char: 'shadeColors'; colors: string[] }
  | { char: 'baseColors'; colors: string[] }
  | { char: 'knockout'; pixel: number; brightness: number }
  | { char: 'expressionOp'; op: 'upsert' | 'remove'; entry?: unknown; type?: string; target?: number }
  | { char: 'mqtt'; op: 'update'; enabled?: boolean; brokerHost?: string; brokerPort?: number; username?: string; password?: string; topicPrefix?: string }

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

// ── Encrypted-write retry helper ──────────────────────────────────────────────

/**
 * Writes to characteristics marked WRITE_ENC (CHAR_AUTH, CHAR_WIFI_OP,
 * CHAR_SETTINGS_BLOB) trigger an OS-mediated BLE pair on first use. On
 * Android the OS dialog can race the write — the first attempt fails with
 * `GATT_INSUFFICIENT_AUTHENTICATION` while the user is still tapping
 * "Pair". One retry after a short delay covers it; iOS typically pairs
 * silently and the first attempt succeeds.
 */
async function writeEncryptedWithRetry(fn: () => Promise<void>): Promise<void> {
  try {
    await fn()
    return
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    if (!/auth/i.test(msg) && !/insufficient/i.test(msg) && !/encrypt/i.test(msg)) {
      throw err
    }
    await new Promise((r) => setTimeout(r, 800))
    await fn()
  }
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
 *
 * AUTH is marked WRITE_ENC on the lamp — the first call also triggers BLE
 * pairing so the password is never sent in cleartext.
 */
export async function authConnection(deviceId: string, password: string): Promise<void> {
  await writeEncryptedWithRetry(() =>
    BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_AUTH, textToDataView(password)),
  )
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
  // WRITE_ENC on the lamp side — first call triggers BLE pairing so the
  // home WiFi password is sent over an encrypted link.
  await writeEncryptedWithRetry(() =>
    BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_WIFI_OP,
      textToDataView(JSON.stringify({ op: 'connect', ssid, password }))),
  )
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
  // WRITE_ENC on the lamp side — full config save includes the lamp
  // password, so this write rides the encrypted/bonded link.
  await writeEncryptedWithRetry(() =>
    BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_SETTINGS_BLOB, textToDataView(json)),
  )
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
export async function readMqttSection(deviceId: string): Promise<MqttConfig> {
  const raw = await readSection<Partial<MqttConfig>>(deviceId, CHAR_MQTT_SECTION, 'mqtt section', {})
  return {
    enabled: raw.enabled ?? false,
    brokerHost: raw.brokerHost ?? '',
    brokerPort: raw.brokerPort ?? 1883,
    username: raw.username ?? '',
    password: raw.password ?? '',
    topicPrefix: raw.topicPrefix ?? '',
  }
}

/**
 * Write a full MQTT config update. WRITE_ENC on the lamp side — first
 * call after pairing triggers the OS pair flow; the retry wrapper handles
 * the brief INSUFFICIENT_AUTHENTICATION race.
 *
 * If `cfg.password` is the literal `'********'`, the firmware preserves the
 * stored password rather than overwriting. Pass a real string only when the
 * user has actually typed a new password.
 */
export async function writeMqttConfig(deviceId: string, cfg: MqttConfig): Promise<void> {
  const payload = JSON.stringify({ op: 'update', ...cfg })
  await writeEncryptedWithRetry(() =>
    BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_MQTT_OP, textToDataView(payload)),
  )
}

// ── Grid (ESP-NOW mesh) ──────────────────────────────────────────────────────

export async function readNearbyLamps(deviceId: string): Promise<NearbyLamp[]> {
  const dv = await BleClient.read(deviceId, CONTROL_SERVICE_UUID, CHAR_NEARBY_LAMPS)
  const text = new TextDecoder().decode(dv).trim()
  if (!text) return []
  try {
    return JSON.parse(text) as NearbyLamp[]
  } catch (err) {
    console.warn('[ble] nearby lamps parse failed:', err, 'payload:', text)
    return []
  }
}

/**
 * Forward a control write to a peer lamp via the local BLE-paired lamp.
 * `targetMac` is the destination ("AA:BB:..."), or the string "broadcast"
 * to fan out to every reachable grid peer. WRITE_ENC — uses the same
 * retry wrapper as other sensitive writes.
 */
export async function writeRemoteOp(
  deviceId: string,
  targetMac: string,
  payload: RemoteOpPayload,
): Promise<void> {
  const body = JSON.stringify({ targetMac, ...payload })
  await writeEncryptedWithRetry(() =>
    BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_REMOTE_OP, textToDataView(body)),
  )
}

// ── Per-section notifications ─────────────────────────────────────────────────

export interface SectionCallbacks {
  onLamp?: (data: Record<string, unknown>) => void
  onBase?: (data: Record<string, unknown>) => void
  onShade?: (data: Record<string, unknown>) => void
  onExpressions?: (data: unknown[]) => void
  onHomeMode?: (data: Record<string, unknown>) => void
  onMqtt?: (data: MqttConfig) => void
  onNearbyLamps?: (lamps: NearbyLamp[]) => void
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
    subscribe(deviceId, CHAR_MQTT_SECTION,  'mqtt section',
      { enabled: false, brokerHost: '', brokerPort: 1883, username: '', password: '', topicPrefix: '' } as MqttConfig,
      callbacks.onMqtt),
    subscribe(deviceId, CHAR_NEARBY_LAMPS,  'nearby lamps',        [] as NearbyLamp[], callbacks.onNearbyLamps),
  ])
}

export async function unsubscribeSectionNotifies(deviceId: string): Promise<void> {
  const stops = [
    CHAR_LAMP_SECTION,
    CHAR_BASE_SECTION,
    CHAR_SHADE_SECTION,
    CHAR_EXPR_SECTION,
    CHAR_HOME_SECTION,
    CHAR_MQTT_SECTION,
    CHAR_NEARBY_LAMPS,
  ].map(async (uuid) => {
    try { await BleClient.stopNotifications(deviceId, CONTROL_SERVICE_UUID, uuid) }
    catch { /* not subscribed or disconnected */ }
  })
  await Promise.all(stops)
}
