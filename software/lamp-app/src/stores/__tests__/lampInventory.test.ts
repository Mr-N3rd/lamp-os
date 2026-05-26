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
    store.add({ id: 'AA:BB:CC:DD:EE:FF', name: 'kitchen' })
    store.add({ id: 'AA:BB:CC:DD:EE:FF', name: 'kitchen-renamed' })
    expect(store.lamps).toHaveLength(1)
    expect(store.lamps[0].name).toBe('kitchen-renamed')
  })

  it('adds a lamp with password', () => {
    const store = useLampInventoryStore()
    store.add({ id: 'AA:BB:CC:DD:EE:FF', name: 'kitchen', password: 'secret' })
    expect(store.lamps[0].password).toBe('secret')
  })

  it('removes a lamp by id', () => {
    const store = useLampInventoryStore()
    store.add({ id: 'X', name: 'a' })
    store.add({ id: 'Y', name: 'b' })
    store.remove('X')
    expect(store.lamps).toHaveLength(1)
    expect(store.lamps[0].id).toBe('Y')
  })

  it('finds a lamp by id', () => {
    const store = useLampInventoryStore()
    store.add({ id: 'Z', name: 'desk' })
    expect(store.findById('Z')?.name).toBe('desk')
    expect(store.findById('nope')).toBeUndefined()
  })

  it('updateSeen sets lastSeen timestamp', () => {
    const store = useLampInventoryStore()
    store.add({ id: 'A', name: 'test' })
    const before = Date.now()
    store.updateSeen('A')
    expect(store.lamps[0].lastSeen).toBeGreaterThanOrEqual(before)
  })
})
