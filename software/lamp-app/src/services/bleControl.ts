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

/**
 * Write brightness (u8 0-100). write-without-response for minimum drag latency.
 */
export async function writeBrightness(deviceId: string, value: number): Promise<void> {
  const clamped = Math.max(0, Math.min(100, Math.round(value)))
  await BleClient.writeWithoutResponse(deviceId, CONTROL_SERVICE_UUID, CHAR_BRIGHTNESS, u8ToDataView(clamped))
}

/**
 * Write shade colors (JSON array of hex strings). write-without-response.
 */
export async function writeShadeColors(deviceId: string, colors: string[]): Promise<void> {
  await BleClient.writeWithoutResponse(deviceId, CONTROL_SERVICE_UUID, CHAR_SHADE_COLORS, textToDataView(JSON.stringify(colors)))
}

/**
 * Write base colors (JSON array of hex strings). write-without-response.
 */
export async function writeBaseColors(deviceId: string, colors: string[]): Promise<void> {
  await BleClient.writeWithoutResponse(deviceId, CONTROL_SERVICE_UUID, CHAR_BASE_COLORS, textToDataView(JSON.stringify(colors)))
}

/**
 * Write a base knockout pixel. 2 bytes: [pixelIndex u8, brightness u8 0-100].
 * write-without-response.
 */
export async function writeBaseKnockout(deviceId: string, pixelIndex: number, brightness: number): Promise<void> {
  const clampedB = Math.max(0, Math.min(100, Math.round(brightness)))
  await BleClient.writeWithoutResponse(deviceId, CONTROL_SERVICE_UUID, CHAR_BASE_KNOCKOUT, twoU8ToDataView(pixelIndex, clampedB))
}

/**
 * Start an expression preview. write-with-response (GATT ack).
 * @param type  Expression type name (non-empty string).
 */
export async function writeExpressionTest(deviceId: string, type: string): Promise<void> {
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_EXPRESSION_TEST, textToDataView(type))
}

/**
 * End expression preview (restore configurator colors). write-with-response.
 * Sends empty string — firmware treats empty or "complete" as test_expression_complete.
 */
export async function writeExpressionComplete(deviceId: string): Promise<void> {
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_EXPRESSION_TEST, textToDataView(''))
}

// ── Settings blob ─────────────────────────────────────────────────────────────

/**
 * Read the full config JSON from the lamp.
 * No auth required — app needs it to determine whether a password exists
 * before it can even attempt auth.
 */
export async function readSettingsBlob(deviceId: string): Promise<string> {
  const dv = await BleClient.read(deviceId, CONTROL_SERVICE_UUID, CHAR_SETTINGS_BLOB)
  return new TextDecoder().decode(dv)
}

/**
 * Write the full config JSON to the lamp.
 * ⚠️  Triggers an immediate lamp reboot — the GATT connection will drop.
 * Caller must handle the resulting disconnect gracefully.
 */
export async function writeSettingsBlob(deviceId: string, config: unknown): Promise<void> {
  const json = JSON.stringify(config)
  await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_SETTINGS_BLOB, textToDataView(json))
}

// ── State notifications ───────────────────────────────────────────────────────

/**
 * Subscribe to state-change notifications from the lamp.
 * The payload is always `{}` — clients should re-read settings_blob on each
 * notification to refresh state.
 */
export async function subscribeStateNotify(
  deviceId: string,
  onNotify: () => void,
): Promise<void> {
  await BleClient.startNotifications(
    deviceId,
    CONTROL_SERVICE_UUID,
    CHAR_STATE_NOTIFY,
    () => onNotify(),
  )
}

export async function unsubscribeStateNotify(deviceId: string): Promise<void> {
  try {
    await BleClient.stopNotifications(deviceId, CONTROL_SERVICE_UUID, CHAR_STATE_NOTIFY)
  } catch {
    // Not subscribed or already disconnected — safe to ignore.
  }
}
