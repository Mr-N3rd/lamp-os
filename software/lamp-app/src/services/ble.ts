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
