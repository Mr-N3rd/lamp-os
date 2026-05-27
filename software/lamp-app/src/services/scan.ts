import { ref } from 'vue'
import type { Ref } from 'vue'
import { BleClient } from '@capacitor-community/bluetooth-le'
import { initBle, scanForLamps } from '@/services/ble'
import type { BleScanDebug } from '@/services/ble'
import { useLampInventoryStore } from '@/stores/lampInventory'

export interface NearbyLamp {
  id: string
  name: string
  viaBle: boolean
  /** Lamp is advertising the control GATT service (lamp-os firmware) */
  isUnconfigured: boolean
  rssi?: number
}

export interface ScanDebug {
  rawBleResults: Ref<BleScanDebug[]>
  bleError: Ref<string | null>
}

export async function startScan(): Promise<{
  lamps: Ref<NearbyLamp[]>
  stop: () => void
  debug: ScanDebug
}> {
  const lamps = ref<NearbyLamp[]>([])
  const inventory = useLampInventoryStore()

  const rawBleResults = ref<BleScanDebug[]>([])
  const bleError = ref<string | null>(null)

  let bleStopped = false

  function isKnown(id: string): boolean {
    return inventory.lamps.some((l) => l.id === id)
  }

  function upsert(partial: Partial<NearbyLamp> & { id: string; name: string }) {
    if (isKnown(partial.id)) return
    const idx = lamps.value.findIndex((l) => l.id === partial.id)
    if (idx === -1) {
      lamps.value.push({
        id: partial.id,
        name: partial.name,
        viaBle: partial.viaBle ?? false,
        isUnconfigured: partial.isUnconfigured ?? false,
        rssi: partial.rssi,
      })
    } else {
      const existing = lamps.value[idx]
      lamps.value[idx] = {
        ...existing,
        name: partial.name,
        rssi: partial.rssi ?? existing.rssi,
      }
    }
  }

  // Start BLE scan
  try {
    await initBle()
    if (!bleStopped) {
      await scanForLamps(
        (lamp) => {
          if (bleStopped) return
          upsert({
            id: lamp.id,
            name: lamp.name,
            viaBle: true,
            isUnconfigured: lamp.isUnconfigured ?? false,
          })
        },
        10_000,
        (raw) => {
          if (bleStopped) return
          // dedupe by deviceId, keep latest 30
          const without = rawBleResults.value.filter((r) => r.deviceId !== raw.deviceId)
          rawBleResults.value = [raw, ...without].slice(0, 30)
        },
      )
    }
  } catch (err) {
    bleError.value = err instanceof Error ? err.message : String(err)
    console.warn('[scan] BLE scan failed:', err)
  }

  function stop() {
    bleStopped = true
    // Actually halt the LE scan in the BLE stack so subsequent GATT connects
    // don't fight with an active scan (Android in particular dislikes this).
    void BleClient.stopLEScan().catch(() => {
      // already stopped or never started — ignore
    })
  }

  return {
    lamps,
    stop,
    debug: { rawBleResults, bleError },
  }
}
