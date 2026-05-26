import { ref } from 'vue'
import type { Ref } from 'vue'
import { initBle, scanForLamps } from '@/services/ble'
import type { BleScanDebug } from '@/services/ble'
import { scanMdnsLamps } from '@/services/mdns'
import { useLampInventoryStore } from '@/stores/lampInventory'

export interface NearbyLamp {
  id: string
  name: string
  ip?: string
  viaBle: boolean
  viaMdns: boolean
  /** Lamp is advertising the setup GATT service (brand-new, no WiFi credentials yet) */
  isUnconfigured: boolean
}

export interface ScanDebug {
  rawBleResults: Ref<BleScanDebug[]>
  rawMdnsResults: Ref<{ name: string; ip: string; port: number }[]>
  bleError: Ref<string | null>
  mdnsError: Ref<string | null>
}

export async function startScan(): Promise<{
  lamps: Ref<NearbyLamp[]>
  stop: () => void
  debug: ScanDebug
}> {
  const lamps = ref<NearbyLamp[]>([])
  const inventory = useLampInventoryStore()

  const rawBleResults = ref<BleScanDebug[]>([])
  const rawMdnsResults = ref<{ name: string; ip: string; port: number }[]>([])
  const bleError = ref<string | null>(null)
  const mdnsError = ref<string | null>(null)

  let bleStopped = false
  let mdnsStopped = false

  function isKnown(name: string): boolean {
    return inventory.lamps.some((l) => l.name === name)
  }

  function upsert(partial: Partial<NearbyLamp> & { name: string }) {
    const key = partial.name.toLowerCase()
    const idx = lamps.value.findIndex((l) => l.name.toLowerCase() === key)
    if (isKnown(partial.name)) return

    if (idx === -1) {
      lamps.value.push({
        id: partial.id ?? partial.name,
        name: partial.name,
        ip: partial.ip,
        viaBle: partial.viaBle ?? false,
        viaMdns: partial.viaMdns ?? false,
        isUnconfigured: partial.isUnconfigured ?? false,
      })
    } else {
      const existing = lamps.value[idx]
      lamps.value[idx] = {
        id: existing.id !== existing.name ? existing.id : (partial.id ?? existing.id),
        name: existing.name,
        ip: partial.ip ?? existing.ip,
        viaBle: existing.viaBle || (partial.viaBle ?? false),
        viaMdns: existing.viaMdns || (partial.viaMdns ?? false),
        isUnconfigured: existing.isUnconfigured || (partial.isUnconfigured ?? false),
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
            viaMdns: false,
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

  // Start mDNS scan
  try {
    if (!mdnsStopped) {
      await scanMdnsLamps((lamp) => {
        if (mdnsStopped) return
        rawMdnsResults.value = [...rawMdnsResults.value, lamp].slice(-30)
        upsert({ name: lamp.name, ip: lamp.ip, viaBle: false, viaMdns: true })
      })
    }
  } catch (err) {
    mdnsError.value = err instanceof Error ? err.message : String(err)
    console.warn('[scan] mDNS scan failed:', err)
  }

  function stop() {
    bleStopped = true
    mdnsStopped = true
  }

  return {
    lamps,
    stop,
    debug: { rawBleResults, rawMdnsResults, bleError, mdnsError },
  }
}
