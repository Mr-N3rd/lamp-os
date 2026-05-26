import { BleClient } from '@capacitor-community/bluetooth-le'

// The firmware sets manufacturer id = 42069 (0xA455). The BLE stack uses bytes 0-1
// of manufacturer-specific data as the Company ID — they appear as the KEY of
// result.manufacturerData[], not in the value. So the value is just the 6 color
// bytes that follow: R G B (base) R G B (shade).
const LAMP_MANUFACTURER_ID = '42069'

export interface DiscoveredLamp {
  id: string         // BLE MAC / identifier
  name: string       // BLE advertisement name
  baseColor?: [number, number, number]
  shadeColor?: [number, number, number]
  /** true when the lamp is advertising its unconfigured-setup GATT service */
  isUnconfigured?: boolean
}

export async function initBle() {
  await BleClient.initialize({ androidNeverForLocation: true })
}

export async function scanForLamps(
  onLamp: (lamp: DiscoveredLamp) => void,
  durationMs = 10_000,
) {
  await BleClient.requestLEScan({ allowDuplicates: true }, (result) => {
    const name = result.localName ?? result.device.name ?? 'unknown'

    // Configured lamp: color-sync beacon (manufacturer data keyed by 42069).
    const colorData = result.manufacturerData?.[LAMP_MANUFACTURER_ID]
    if (colorData && colorData.byteLength >= 6) {
      onLamp({
        id: result.device.deviceId,
        name,
        baseColor: [colorData.getUint8(0), colorData.getUint8(1), colorData.getUint8(2)],
        shadeColor: [colorData.getUint8(3), colorData.getUint8(4), colorData.getUint8(5)],
      })
      return
    }

    // Unconfigured lamp: advertises the setup GATT service UUID.
    const uuids = result.uuids?.map((u) => u.toLowerCase()) ?? []
    if (uuids.includes(SETUP_SERVICE_UUID.toLowerCase())) {
      onLamp({
        id: result.device.deviceId,
        name,
        isUnconfigured: true,
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
