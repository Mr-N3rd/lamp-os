import { vi } from 'vitest'

vi.mock('@capacitor/preferences', () => {
  const memory = new Map<string, string>()
  return {
    Preferences: {
      get: vi.fn(async ({ key }: { key: string }) => ({ value: memory.get(key) ?? null })),
      set: vi.fn(async ({ key, value }: { key: string; value: string }) => {
        memory.set(key, value)
      }),
      remove: vi.fn(async ({ key }: { key: string }) => { memory.delete(key) }),
    },
  }
})
