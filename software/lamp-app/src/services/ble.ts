import { BleClient } from '@capacitor-community/bluetooth-le'

const LAMP_MANUFACTURER_MAGIC = 42069  // bytes 0-1 of manufacturer data

export interface DiscoveredLamp {
  id: string         // BLE MAC / identifier
  name: string       // BLE advertisement name
  baseColor?: [number, number, number]
  shadeColor?: [number, number, number]
}

export async function initBle() {
  await BleClient.initialize({ androidNeverForLocation: true })
}

export async function scanForLamps(
  onLamp: (lamp: DiscoveredLamp) => void,
  durationMs = 10_000,
) {
  await BleClient.requestLEScan({}, (result) => {
    const md = result.manufacturerData
    if (!md) return
    // Look for our magic number in any manufacturer-data slot
    for (const [, data] of Object.entries(md)) {
      const bytes = new Uint8Array((data as DataView).buffer)
      if (bytes.length < 8) continue
      const magic = (bytes[0] << 8) | bytes[1]
      if (magic !== LAMP_MANUFACTURER_MAGIC) continue
      onLamp({
        id: result.device.deviceId,
        name: result.device.name ?? 'unknown',
        baseColor: [bytes[2], bytes[3], bytes[4]],
        shadeColor: [bytes[5], bytes[6], bytes[7]],
      })
    }
  })
  setTimeout(() => { void BleClient.stopLEScan() }, durationMs)
}

// ── GATT Setup Service ────────────────────────────────────────────────────────

export const SETUP_SERVICE_UUID  = '5f64f4c1-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const SETUP_CHAR_SSID     = '5f64f4c2-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const SETUP_CHAR_PASSWORD = '5f64f4c3-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const SETUP_CHAR_NAME     = '5f64f4c4-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const SETUP_CHAR_APPLY    = '5f64f4c5-d6d9-4a44-9b3f-3a8d6f7e6b40'
export const SETUP_CHAR_INFO     = '5f64f4c6-d6d9-4a44-9b3f-3a8d6f7e6b40'

export interface LampSetupCredentials {
  ssid: string
  password: string
  name: string
}

function toDataView(s: string): DataView {
  const bytes = new TextEncoder().encode(s)
  return new DataView(bytes.buffer)
}

export async function writeLampSetup(deviceId: string, creds: LampSetupCredentials): Promise<void> {
  await BleClient.connect(deviceId)

  try {
    await BleClient.write(deviceId, SETUP_SERVICE_UUID, SETUP_CHAR_SSID,     toDataView(creds.ssid))
    await BleClient.write(deviceId, SETUP_SERVICE_UUID, SETUP_CHAR_PASSWORD, toDataView(creds.password))
    await BleClient.write(deviceId, SETUP_SERVICE_UUID, SETUP_CHAR_NAME,     toDataView(creds.name))

    // Trigger reboot — lamp will drop the connection, so this may throw; that's expected.
    try {
      await BleClient.write(deviceId, SETUP_SERVICE_UUID, SETUP_CHAR_APPLY, new DataView(new Uint8Array([0x01]).buffer))
    } catch {
      // Lamp rebooted mid-write — this is normal, continue.
    }
  } finally {
    try {
      await BleClient.disconnect(deviceId)
    } catch {
      // Already disconnected due to reboot — ignore.
    }
  }
}
