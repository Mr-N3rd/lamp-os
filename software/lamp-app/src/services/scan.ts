import { ref } from 'vue'
import type { Ref } from 'vue'
import { initBle, scanForLamps } from '@/services/ble'
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

export async function startScan(): Promise<{ lamps: Ref<NearbyLamp[]>; stop: () => void }> {
  const lamps = ref<NearbyLamp[]>([])
  const inventory = useLampInventoryStore()

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
      await scanForLamps((lamp) => {
        if (bleStopped) return
        upsert({
          id: lamp.id,
          name: lamp.name,
          viaBle: true,
          viaMdns: false,
          isUnconfigured: lamp.isUnconfigured ?? false,
        })
      })
    }
  } catch (err) {
    console.warn('[scan] BLE scan failed:', err)
  }

  // Start mDNS scan
  try {
    if (!mdnsStopped) {
      await scanMdnsLamps((lamp) => {
        if (mdnsStopped) return
        upsert({ name: lamp.name, ip: lamp.ip, viaBle: false, viaMdns: true })
      })
    }
  } catch (err) {
    console.warn('[scan] mDNS scan failed:', err)
  }

  function stop() {
    bleStopped = true
    mdnsStopped = true
  }

  return { lamps, stop }
}
