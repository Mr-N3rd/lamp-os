import { describe, it, expect, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useLampInventoryStore } from '../lampInventory'

describe('useLampInventoryStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('starts empty', () => {
    const store = useLampInventoryStore()
    expect(store.lamps).toEqual([])
  })

  it('adds a lamp and dedupes by id', () => {
    const store = useLampInventoryStore()
    store.add({ id: 'AA:BB:CC:DD:EE:FF', name: 'kitchen', lastIp: '192.168.1.10' })
    store.add({ id: 'AA:BB:CC:DD:EE:FF', name: 'kitchen-renamed', lastIp: '192.168.1.11' })
    expect(store.lamps).toHaveLength(1)
    expect(store.lamps[0].name).toBe('kitchen-renamed')
    expect(store.lamps[0].lastIp).toBe('192.168.1.11')
  })

  it('removes a lamp by id', () => {
    const store = useLampInventoryStore()
    store.add({ id: 'X', name: 'a', lastIp: '1.1.1.1' })
    store.add({ id: 'Y', name: 'b', lastIp: '2.2.2.2' })
    store.remove('X')
    expect(store.lamps).toHaveLength(1)
    expect(store.lamps[0].id).toBe('Y')
  })

  it('finds a lamp by id', () => {
    const store = useLampInventoryStore()
    store.add({ id: 'Z', name: 'desk', lastIp: '3.3.3.3' })
    expect(store.findById('Z')?.name).toBe('desk')
    expect(store.findById('nope')).toBeUndefined()
  })
})
