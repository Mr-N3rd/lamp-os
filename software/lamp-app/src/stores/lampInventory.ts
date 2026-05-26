import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { Preferences } from '@capacitor/preferences'

const STORAGE_KEY = 'lamp-inventory'

export interface InventoryLamp {
  id: string           // BLE MAC, stable across reboots
  name: string         // user-facing
  lastIp: string       // last-known IP on the home network
  lastSeen?: number    // epoch ms
  addedAt?: number     // epoch ms
  password?: string    // lamp's password, if set (stored so user doesn't re-enter)
}

export const useLampInventoryStore = defineStore('lampInventory', () => {
  const lamps = ref<InventoryLamp[]>([])
  const loaded = ref(false)

  const count = computed(() => lamps.value.length)

  const load = async () => {
    const { value } = await Preferences.get({ key: STORAGE_KEY })
    if (value) {
      try {
        lamps.value = JSON.parse(value)
      } catch {
        lamps.value = []
      }
    }
    loaded.value = true
  }

  const persist = async () => {
    await Preferences.set({ key: STORAGE_KEY, value: JSON.stringify(lamps.value) })
  }

  const add = (lamp: InventoryLamp) => {
    const existing = lamps.value.findIndex((l) => l.id === lamp.id)
    if (existing !== -1) {
      lamps.value[existing] = { ...lamps.value[existing], ...lamp }
    } else {
      lamps.value.push({ ...lamp, addedAt: Date.now() })
    }
    void persist()
  }

  const remove = (id: string) => {
    lamps.value = lamps.value.filter((l) => l.id !== id)
    void persist()
  }

  const updateSeen = (id: string, ip?: string) => {
    const lamp = lamps.value.find((l) => l.id === id)
    if (!lamp) return
    lamp.lastSeen = Date.now()
    if (ip) lamp.lastIp = ip
    void persist()
  }

  const findById = (id: string) => lamps.value.find((l) => l.id === id)

  return { lamps, count, loaded, load, add, remove, updateSeen, findById }
})
