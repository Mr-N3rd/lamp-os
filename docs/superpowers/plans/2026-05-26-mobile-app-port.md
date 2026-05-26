# Mobile App Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Working directory:** This plan executes in the git worktree at `/Users/jerrett/projects/lamp-os/.claude/worktrees/mobile-app-port/` on branch `worktree-mobile-app-port`. **Do not push** any branch — keep all commits local until the user explicitly says it's solid. See spec at `/Users/jerrett/.claude/plans/what-would-it-take-velvety-music.md`.

**Goal:** Replace the lamp-served Vue web UI with a real iOS + Android app (Capacitor + Vue 3) that maintains a curated list of *your lamps*, controls them over WiFi (HTTP REST + WebSocket), discovers via BLE + mDNS, and provisions new lamps over BLE GATT. Free up firmware flash by removing the on-lamp Vue bundle; replace the captive portal with a tiny "install the app" landing page.

**Architecture:** Capacitor wraps a Vue 3 SPA. The existing `software/lamp-ui` codebase (Pinia store, components, expression editor, form fields) is the source of truth for the control UI; the mobile app reuses it. Top-level adds a Lamps inventory + AddLamp flow. The Pinia lamp store is refactored from single-target (env vars) to multi-target (a passed-in `{ baseUrl, wsUrl }`). Firmware gains a BLE GATT setup service for adding lamps and an `_lamp._tcp` mDNS service record for discovery; it loses the gzipped Vue bundle from `data/`.

**Tech Stack:** Capacitor 6+ for native shell; Vue 3.5 + Pinia + Vue Router; TypeScript; Vite (without inline-everything plugins — that's a lamp-ui constraint, not a native app one); `@capacitor-community/bluetooth-le` for BLE; mDNS plugin TBD (spike in Phase 5); existing ESP32 PlatformIO firmware (`software/lamp-os`) on the device side.

---

## Phases (roadmap)

| # | Phase | Status in this plan |
|---|-------|---------------------|
| 0 | Resolve WIP-on-beta blocker | Detailed |
| 1 | Scaffold `software/lamp-app/` Capacitor + Vue project | Detailed |
| 2 | Refactor `lamp.ts` to multi-target | Detailed |
| 3 | Lamps inventory store + Lamps screen | Detailed |
| 4 | Per-lamp routing scope | Detailed |
| 5 | Native plugin spike: BLE + mDNS | Detailed (spike + decision) |
| 6 | AddLamp wizard (BLE GATT + WiFi add) | Outlined — fill in after Phase 5 |
| 7 | Firmware: `_lamp._tcp` mDNS service record | Detailed |
| 8 | Firmware: BLE GATT setup service | Detailed |
| 9 | Firmware: captive portal landing page + remove Vue bundle | Detailed |
| 10 | End-to-end verification | Detailed |

Phases 7, 8, 9 are independent of phases 1–6 and can be dispatched in parallel.

---

## Phase 0: Resolve WIP-on-beta blocker

The user has substantial uncommitted refactor work on `beta` (the modern `pages/`, `stores/`, `composables/`, `layout/`, `plugins/router.ts`, `components/fields/`, `components/Form.vue` directory structure). The committed `beta` HEAD is far older and does **not** contain this code. The worktree was created from `beta` HEAD, so the new Vue 3 codebase is missing here.

This must be resolved before any task that reads or modifies `software/lamp-ui/`.

### Task 0.1: Surface the situation to the user and pick a strategy

**Files:** none

- [ ] **Step 1: Pause and confirm strategy with the user**

Send a message to the user describing the situation:

> "Found that your `beta` working tree has lots of uncommitted Vue 3 refactor work (`pages/`, `stores/`, `layout/`, `plugins/router.ts`, `components/fields/`, `components/Form.vue`). My worktree branched from `beta` HEAD which doesn't have this yet. Three ways forward — which do you want?
>
> A) You commit the WIP on `beta` (or stash it), and I merge `beta` into the worktree. Cleanest.
>
> B) I commit the WIP for you on your behalf — message like `chore(ui): WIP Vue 3 refactor (committed by Claude during mobile-app port)`. Faster but I touch your in-progress work.
>
> C) Stay branched off the old commit. I work only on new files (`software/lamp-app/`, firmware) and you reconcile the lamp-ui refactor later. Defers conflict pain to merge time."

- [ ] **Step 2: Apply the user's choice**

For **A**: wait for confirmation, then run `git fetch && git merge beta --no-ff -m "merge beta into mobile-app-port"` in the worktree. Resolve conflicts (likely none — worktree is greenfield). Verify file layout matches what the spec assumes (`software/lamp-ui/src/stores/lamp.ts`, `pages/`, etc.) by running `ls software/lamp-ui/src/{stores,pages,layout,plugins}/`.

For **B**: in the parent working tree at `/Users/jerrett/projects/lamp-os/`, run `git add software/ && git commit -m "chore(ui): WIP Vue 3 refactor (committed by Claude during mobile-app port)"` (after showing the user the diff). Then merge into worktree as in **A**.

For **C**: copy the relevant `software/lamp-ui/src/` files from the parent working tree into the worktree as a single setup commit, with a note in the commit body explaining provenance.

- [ ] **Step 3: Verify the lamp-ui code is now present in the worktree**

Run: `ls software/lamp-ui/src/stores/lamp.ts software/lamp-ui/src/plugins/router.ts software/lamp-ui/src/layout/Lamp.vue software/lamp-ui/src/components/Form.vue`

Expected: All four files exist. If any are missing, the WIP wasn't fully merged — investigate before proceeding.

- [ ] **Step 4: Confirm `package.json` is consistent**

The WIP code imports `pinia` and `vue-router`, but those packages aren't in `software/lamp-ui/package.json` dependencies (the user hadn't gotten to `npm install` them yet). Add them:

```bash
cd software/lamp-ui
npm install pinia@3.0.4 vue-router@4.5.0
```

- [ ] **Step 5: Verify lamp-ui builds**

Run: `cd software/lamp-ui && npm run type-check && npm run build`

Expected: Both succeed. If type-check fails on missing imports, the WIP was in an even more incomplete state than expected — surface to user.

- [ ] **Step 6: Commit**

```bash
git add software/lamp-ui/package.json software/lamp-ui/package-lock.json
git commit -m "chore(ui): add pinia + vue-router to dependencies"
```

---

## Phase 1: Scaffold `software/lamp-app/` Capacitor + Vue project

Goal: A new Vue 3 + Capacitor app at `software/lamp-app/` that builds, opens in the iOS simulator, and shows a placeholder "Lamps" page.

### Task 1.1: Initialize the Vue 3 + Vite + TS project

**Files:**
- Create: `software/lamp-app/` (entire directory tree)

- [ ] **Step 1: Scaffold a new Vite + Vue 3 + TS project**

Run from the worktree root:

```bash
mkdir -p software/lamp-app
cd software/lamp-app
npm create vite@latest . -- --template vue-ts
```

When prompted, accept defaults (don't add anything besides Vue + TS).

- [ ] **Step 2: Install runtime deps to match the lamp-ui versions**

```bash
npm install vue@3.5.18 pinia@3.0.4 vue-router@4.5.0
```

- [ ] **Step 3: Install dev deps for linting and testing (mirror lamp-ui where applicable)**

```bash
npm install --save-dev typescript@5.9.3 vue-tsc@3.0.4 \
  @vitejs/plugin-vue@4.5.0 \
  vitest@3.2.4 jsdom@26.1.0 @vue/test-utils@2.4.6 \
  eslint@9.39.1 eslint-plugin-vue@10.6.0 @vue/eslint-config-typescript@14.6.0 \
  @vue/eslint-config-prettier@10.2.0 prettier@3.6.2
```

- [ ] **Step 4: Verify it builds**

```bash
npm run build
```

Expected: Build succeeds, `dist/` populated.

- [ ] **Step 5: Commit**

```bash
git add software/lamp-app
git commit -m "feat(app): scaffold Vue 3 + Vite + TS project for mobile app"
```

### Task 1.2: Add Capacitor and iOS + Android platforms

**Files:**
- Create: `software/lamp-app/capacitor.config.ts`
- Create: `software/lamp-app/ios/`, `software/lamp-app/android/`

- [ ] **Step 1: Install Capacitor**

```bash
cd software/lamp-app
npm install @capacitor/core @capacitor/cli @capacitor/ios @capacitor/android @capacitor/app @capacitor/preferences
```

- [ ] **Step 2: Initialize Capacitor**

```bash
npx cap init "Lamp" "com.lamplit.lamp" --web-dir dist
```

Expected: `capacitor.config.ts` created at project root.

- [ ] **Step 3: Edit `capacitor.config.ts` to set sane defaults**

Replace the generated file with:

```typescript
import type { CapacitorConfig } from '@capacitor/cli';

const config: CapacitorConfig = {
  appId: 'com.lamplit.lamp',
  appName: 'Lamp',
  webDir: 'dist',
  server: {
    androidScheme: 'https',
  },
  ios: {
    contentInset: 'always',
  },
};

export default config;
```

- [ ] **Step 4: Build the web app then add platforms**

```bash
npm run build
npx cap add ios
npx cap add android
npx cap sync
```

Expected: `ios/` and `android/` directories created with native projects.

- [ ] **Step 5: Verify iOS project opens (if on macOS)**

```bash
npx cap open ios
```

Expected: Xcode opens the project. (If Xcode isn't installed, skip — the project still exists on disk.)

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app/capacitor.config.ts software/lamp-app/package.json software/lamp-app/package-lock.json software/lamp-app/ios software/lamp-app/android
git commit -m "feat(app): add Capacitor + iOS + Android platforms"
```

### Task 1.3: Wire Pinia and Vue Router into the app shell

**Files:**
- Modify: `software/lamp-app/src/main.ts`
- Create: `software/lamp-app/src/plugins/router.ts`
- Modify: `software/lamp-app/src/App.vue`
- Create: `software/lamp-app/src/pages/Lamps.vue`

- [ ] **Step 1: Create a placeholder Lamps page**

`software/lamp-app/src/pages/Lamps.vue`:

```vue
<script setup lang="ts">
</script>

<template>
  <main class="lamps-page">
    <h1>Your Lamps</h1>
    <p>No lamps yet. Tap "Add a lamp" to get started.</p>
  </main>
</template>

<style scoped>
.lamps-page { padding: 24px; }
</style>
```

- [ ] **Step 2: Create the router with a single route to Lamps**

`software/lamp-app/src/plugins/router.ts`:

```typescript
import { createRouter, createWebHistory } from 'vue-router'

const LampsPage = () => import('@/pages/Lamps.vue')

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', name: 'lamps', component: LampsPage },
  ],
})

export default router
```

- [ ] **Step 3: Wire Pinia + router into main.ts**

Replace `software/lamp-app/src/main.ts` with:

```typescript
import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import router from './plugins/router'

const app = createApp(App)
app.use(createPinia())
app.use(router)
app.mount('#app')
```

- [ ] **Step 4: Simplify App.vue to a router outlet**

Replace `software/lamp-app/src/App.vue` with:

```vue
<script setup lang="ts">
</script>

<template>
  <router-view />
</template>
```

- [ ] **Step 5: Add the `@` alias to `vite.config.ts` and `tsconfig.app.json`**

Edit `software/lamp-app/vite.config.ts` — inside `defineConfig({ ... })` add:

```typescript
import { fileURLToPath, URL } from 'node:url'

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
})
```

Edit `software/lamp-app/tsconfig.app.json` — add to `compilerOptions`:

```json
"baseUrl": ".",
"paths": { "@/*": ["src/*"] }
```

- [ ] **Step 6: Build and verify**

```bash
npm run build && npx cap sync
```

Expected: Build succeeds, `dist/index.html` references `/assets/...` chunks (normal Vite output, NOT the lamp-ui inlined-single-file output).

- [ ] **Step 7: Commit**

```bash
git add software/lamp-app/src software/lamp-app/vite.config.ts software/lamp-app/tsconfig.app.json
git commit -m "feat(app): wire Pinia + Vue Router with placeholder Lamps page"
```

---

## Phase 2: Refactor `lamp.ts` to multi-target

Goal: Change `software/lamp-ui/src/stores/lamp.ts` so the WebSocket and HTTP target are parameters of `initialize()`, not build-time env vars. The mobile app will use this same store against any selected lamp.

### Task 2.1: Add a target parameter to the lamp store

**Files:**
- Modify: `software/lamp-ui/src/stores/lamp.ts`

- [ ] **Step 1: Read the current store**

Confirm the current state by reading `software/lamp-ui/src/stores/lamp.ts`. The key places that hardcode env vars are:
- Line 140: `ws.value = new WebSocket(`${import.meta.env.VITE_SERVER_WS}`)`
- Line 350: `await fetch(`${import.meta.env.VITE_SERVER_HTTP}/settings`, ...)` (PUT in `saveSettings`)
- Line 378: `await fetch(`${import.meta.env.VITE_SERVER_HTTP}/settings`)` (GET in `initialize`)

- [ ] **Step 2: Add a `LampTarget` type and a target ref**

Near the top of the store body (just inside `defineStore`), add:

```typescript
export interface LampTarget {
  baseUrl: string  // e.g. "http://192.168.1.42"
  wsUrl: string    // e.g. "ws://192.168.1.42/ws"
  password?: string  // optional, for future use; not yet sent
}

// Inside the defineStore callback, near the other refs:
const target = ref<LampTarget | null>(null)
```

- [ ] **Step 3: Change `initialize` to accept a target**

Replace the existing `initialize` body with:

```typescript
const initialize = async (newTarget: LampTarget) => {
  target.value = newTarget
  try {
    const response = await fetch(`${newTarget.baseUrl}/settings`)
    const data = await response.json()
    state.value = data
    originalState.value = JSON.stringify(data)
    loaded.value = true
    connectWebSocket()
  } catch (error) {
    console.error('Error loading settings:', error)
    loaded.value = true
  }
}
```

- [ ] **Step 4: Change `connectWebSocket` to use the target**

In `connectWebSocket`, replace:

```typescript
ws.value = new WebSocket(`${import.meta.env.VITE_SERVER_WS}`)
```

with:

```typescript
if (!target.value) {
  console.error('connectWebSocket called with no target set')
  return
}
ws.value = new WebSocket(target.value.wsUrl)
```

- [ ] **Step 5: Change `saveSettings` to use the target**

Replace:

```typescript
const response = await fetch(`${import.meta.env.VITE_SERVER_HTTP}/settings`, {
```

with:

```typescript
if (!target.value) return
const response = await fetch(`${target.value.baseUrl}/settings`, {
```

- [ ] **Step 6: Add a `setTarget` action that switches lamps cleanly**

Add after `cleanup`:

```typescript
const setTarget = async (newTarget: LampTarget) => {
  cleanup()
  state.value = {}
  originalState.value = ''
  loaded.value = false
  await initialize(newTarget)
}
```

Export `setTarget` and `target` (read-only) in the return object.

- [ ] **Step 7: Verify lamp-ui still type-checks**

```bash
cd software/lamp-ui && npm run type-check
```

Expected: PASS. (The lamp-ui itself still needs to pass the env-var-derived target somewhere — see Step 8.)

- [ ] **Step 8: Update `software/lamp-ui/src/layout/Lamp.vue` to pass an env-derived target on mount**

In `software/lamp-ui/src/layout/Lamp.vue`, replace:

```typescript
onMounted(() => {
  lampStore.initialize()
})
```

with:

```typescript
onMounted(() => {
  lampStore.initialize({
    baseUrl: import.meta.env.VITE_SERVER_HTTP,
    wsUrl: import.meta.env.VITE_SERVER_WS,
  })
})
```

This keeps lamp-ui working with the env-var deployment model (lamp-hosted SPA), while the mobile app will pass a per-lamp target.

- [ ] **Step 9: Run lamp-ui unit tests and build**

```bash
cd software/lamp-ui && npm run test:unit -- --run && npm run build
```

Expected: Tests pass, build succeeds.

- [ ] **Step 10: Commit**

```bash
git add software/lamp-ui/src/stores/lamp.ts software/lamp-ui/src/layout/Lamp.vue
git commit -m "refactor(ui): lamp store takes a target so it can connect to any lamp"
```

### Task 2.2: Copy lamp-ui into the mobile app

Strategy: For now, **copy** the relevant `software/lamp-ui/src/` source into `software/lamp-app/src/` rather than setting up a shared workspace package. Copying is faster to validate, and we can extract a shared package later if it proves worth the ceremony. This is explicitly a v1 tradeoff documented in the spec.

**Files:**
- Create: `software/lamp-app/src/stores/lamp.ts` (copied from lamp-ui)
- Create: `software/lamp-app/src/components/*` (copied from lamp-ui)
- Create: `software/lamp-app/src/layout/Lamp.vue` (copied + adapted)
- Create: `software/lamp-app/src/pages/{Index,Setup,Expressions,Info}.vue` (copied)
- Create: `software/lamp-app/src/composables/*`, `software/lamp-app/src/lib/*` (copied as needed)

- [ ] **Step 1: Copy the lamp-ui sources we need**

```bash
cd software/lamp-app
cp -r ../lamp-ui/src/stores src/
cp -r ../lamp-ui/src/components src/
cp -r ../lamp-ui/src/composables src/
cp -r ../lamp-ui/src/layout src/
cp -r ../lamp-ui/src/lib src/
cp -r ../lamp-ui/src/pages src/
cp -r ../lamp-ui/src/plugins src/  # we'll rewrite router below; keep globalComponents + initializeState
cp ../lamp-ui/src/types.ts src/
cp ../lamp-ui/src/assets/main.css src/assets/ 2>/dev/null || true
```

- [ ] **Step 2: Delete the lamp-ui-specific `initializeState` plugin if it depends on env vars**

Inspect `software/lamp-app/src/plugins/initializeState.ts`. If it calls `lampStore.initialize()` with env vars on app load, **delete** it from the app (we'll initialize per-lamp on route entry instead) but keep `globalComponents.ts`. Update `main.ts` accordingly (next task).

- [ ] **Step 3: Verify the copies type-check inside the app**

```bash
cd software/lamp-app && npx vue-tsc --noEmit
```

Expected: Errors only for `import.meta.env.VITE_SERVER_*` usages in `layout/Lamp.vue` — we'll fix in Phase 4 when we wire up per-lamp routing. Other errors mean the copy is incomplete.

- [ ] **Step 4: Commit (intentionally before app fully builds — clean partial)**

```bash
git add software/lamp-app/src
git commit -m "feat(app): copy lamp-ui sources into mobile app for reuse"
```

---

## Phase 3: Lamps inventory store + Lamps screen

Goal: A Pinia store that persists "your lamps" in Capacitor Preferences, and a Lamps screen that lists them with online indicators (reachability will be stubbed; real BLE/mDNS comes in Phase 5).

### Task 3.1: Define the inventory store

**Files:**
- Create: `software/lamp-app/src/stores/lampInventory.ts`

- [ ] **Step 1: Write the failing test**

`software/lamp-app/src/stores/__tests__/lampInventory.test.ts`:

```typescript
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
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd software/lamp-app && npx vitest run src/stores/__tests__/lampInventory.test.ts
```

Expected: FAIL — module doesn't exist.

- [ ] **Step 3: Implement the store**

`software/lamp-app/src/stores/lampInventory.ts`:

```typescript
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
```

- [ ] **Step 4: Mock `@capacitor/preferences` for tests**

`software/lamp-app/src/test/setup.ts`:

```typescript
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
```

Wire it into vitest by editing `software/lamp-app/vite.config.ts`:

```typescript
test: {
  environment: 'jsdom',
  globals: true,
  setupFiles: ['src/test/setup.ts'],
},
```

(If vite.config.ts doesn't have a `test` section yet, add it.)

- [ ] **Step 5: Run tests**

```bash
cd software/lamp-app && npx vitest run
```

Expected: All 4 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app/src/stores/lampInventory.ts software/lamp-app/src/stores/__tests__ software/lamp-app/src/test/setup.ts software/lamp-app/vite.config.ts
git commit -m "feat(app): lamp inventory store backed by Capacitor Preferences"
```

### Task 3.2: Build the Lamps page

**Files:**
- Modify: `software/lamp-app/src/pages/Lamps.vue`

- [ ] **Step 1: Replace the placeholder with a real list**

`software/lamp-app/src/pages/Lamps.vue`:

```vue
<script setup lang="ts">
import { onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useLampInventoryStore } from '@/stores/lampInventory'

const router = useRouter()
const inventory = useLampInventoryStore()

onMounted(() => {
  void inventory.load()
})

const openLamp = (id: string) => {
  router.push({ name: 'lamp-home', params: { id } })
}

const addLamp = () => {
  router.push({ name: 'add-lamp' })
}

const isOnline = (lamp: { lastSeen?: number }) => {
  if (!lamp.lastSeen) return false
  return Date.now() - lamp.lastSeen < 30_000
}
</script>

<template>
  <main class="lamps-page">
    <header>
      <h1>Your Lamps</h1>
      <button class="add-btn" @click="addLamp">+ Add a lamp</button>
    </header>

    <ul v-if="inventory.lamps.length" class="lamp-list">
      <li
        v-for="lamp in inventory.lamps"
        :key="lamp.id"
        class="lamp-row"
        @click="openLamp(lamp.id)"
      >
        <span class="status-dot" :class="{ online: isOnline(lamp) }"></span>
        <span class="lamp-name">{{ lamp.name }}</span>
        <span class="lamp-ip">{{ lamp.lastIp }}</span>
      </li>
    </ul>

    <p v-else class="empty">No lamps yet. Tap "Add a lamp" to get started.</p>
  </main>
</template>

<style scoped>
.lamps-page { padding: 24px; }
header { display: flex; justify-content: space-between; align-items: center; }
.add-btn { padding: 8px 16px; border-radius: 8px; }
.lamp-list { list-style: none; padding: 0; }
.lamp-row { display: flex; align-items: center; gap: 12px; padding: 16px; border-radius: 8px; }
.lamp-row:active { background: rgba(255,255,255,0.05); }
.status-dot { width: 10px; height: 10px; border-radius: 50%; background: #555; }
.status-dot.online { background: #4ade80; }
.lamp-name { flex: 1; font-weight: 600; }
.lamp-ip { color: #999; font-size: 0.85rem; }
.empty { color: #999; }
</style>
```

- [ ] **Step 2: Build to verify it compiles**

```bash
cd software/lamp-app && npm run build
```

Expected: succeeds. (Routes `lamp-home` and `add-lamp` don't exist yet — Vue Router will warn at runtime but the build is fine.)

- [ ] **Step 3: Commit**

```bash
git add software/lamp-app/src/pages/Lamps.vue
git commit -m "feat(app): Lamps page renders inventory with online indicators"
```

---

## Phase 4: Per-lamp routing scope

Goal: `/lamp/:id/{home,expressions,setup,info}` routes hydrate the lamp store with the selected lamp's target on entry and tear down on leave.

### Task 4.1: Adapt `layout/Lamp.vue` to pull target from the route

**Files:**
- Modify: `software/lamp-app/src/layout/Lamp.vue`

- [ ] **Step 1: Replace the env-var-based initialize with route-id-based**

Edit `software/lamp-app/src/layout/Lamp.vue`. Replace the `onMounted` / `onUnmounted` block with:

```typescript
import { onMounted, onUnmounted, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useLampStore, tabs } from '@/stores/lamp'
import { useLampInventoryStore } from '@/stores/lampInventory'

const route = useRoute()
const router = useRouter()
const lampStore = useLampStore()
const inventory = useLampInventoryStore()

const targetForLamp = (id: string) => {
  const lamp = inventory.findById(id)
  if (!lamp) return null
  return {
    baseUrl: `http://${lamp.lastIp}`,
    wsUrl: `ws://${lamp.lastIp}/ws`,
    password: lamp.password,
  }
}

onMounted(async () => {
  if (!inventory.loaded) await inventory.load()
  const id = String(route.params.id)
  const target = targetForLamp(id)
  if (!target) {
    router.replace({ name: 'lamps' })
    return
  }
  await lampStore.initialize(target)
})

watch(
  () => route.params.id,
  async (newId, oldId) => {
    if (newId === oldId) return
    const target = targetForLamp(String(newId))
    if (!target) {
      router.replace({ name: 'lamps' })
      return
    }
    await lampStore.setTarget(target)
  },
)

onUnmounted(() => {
  lampStore.cleanup()
})
```

The existing tab nav inside `Lamp.vue` stays — but update tab paths to be relative (`'home'`, `'expressions'`, etc.) and the nav handler to push to the per-lamp route:

```typescript
const handleTabChange = (tabId: string) => {
  router.push({ name: `lamp-${tabId}`, params: { id: route.params.id } })
}
```

You may need to adjust the existing tab → path mapping in `stores/lamp.ts` (the `tabs` array). Change to:

```typescript
export const tabs = [
  { id: 'home', label: 'Home' },
  { id: 'expressions', label: 'Expressions' },
  { id: 'lamp-setup', label: 'Setup' },
  { id: 'info', label: 'Info' },
]
```

(Drop the `path` field; routing is now handled by name in the layout.)

- [ ] **Step 2: Update router with the per-lamp scope**

Replace `software/lamp-app/src/plugins/router.ts`:

```typescript
import { createRouter, createWebHistory } from 'vue-router'

const LampsPage = () => import('@/pages/Lamps.vue')
const AddLampPage = () => import('@/pages/AddLamp.vue')
const LampLayout = () => import('@/layout/Lamp.vue')
const IndexPage = () => import('@/pages/Index.vue')
const ExpressionsPage = () => import('@/pages/Expressions.vue')
const SetupPage = () => import('@/pages/Setup.vue')
const InfoPage = () => import('@/pages/Info.vue')

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', name: 'lamps', component: LampsPage },
    { path: '/add', name: 'add-lamp', component: AddLampPage },
    {
      path: '/lamp/:id',
      component: LampLayout,
      children: [
        { path: '', name: 'lamp-home', component: IndexPage },
        { path: 'expressions', name: 'lamp-expressions', component: ExpressionsPage },
        { path: 'setup', name: 'lamp-lamp-setup', component: SetupPage },
        { path: 'info', name: 'lamp-info', component: InfoPage },
      ],
    },
  ],
})

export default router
```

- [ ] **Step 3: Create a stub AddLamp page (real implementation in Phase 6)**

`software/lamp-app/src/pages/AddLamp.vue`:

```vue
<script setup lang="ts">
import { useRouter } from 'vue-router'
const router = useRouter()
</script>

<template>
  <main class="add-lamp">
    <h1>Add a lamp</h1>
    <p>BLE / mDNS scan UI lives here (Phase 6).</p>
    <button @click="router.back()">Back</button>
  </main>
</template>

<style scoped>
.add-lamp { padding: 24px; }
</style>
```

- [ ] **Step 4: Verify it builds**

```bash
cd software/lamp-app && npm run build
```

Expected: PASS.

- [ ] **Step 5: Manual smoke test via dev server**

```bash
cd software/lamp-app && npm run dev
```

In another shell, seed an inventory entry by hand (browser console):

```javascript
const lamps = [{ id: 'test', name: 'desk', lastIp: '192.168.1.42', addedAt: Date.now() }]
localStorage.setItem('CapacitorStorage.lamp-inventory', JSON.stringify(lamps))
location.reload()
```

(Capacitor Preferences uses localStorage on web.) Confirm the lamp appears in the Lamps page list. Tap it — confirm the route changes to `/lamp/test/` and the Lamp layout tries to load settings (will fail since `192.168.1.42` is not a real lamp, but the routing should work).

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app/src/layout/Lamp.vue software/lamp-app/src/plugins/router.ts software/lamp-app/src/pages/AddLamp.vue software/lamp-app/src/stores/lamp.ts
git commit -m "feat(app): per-lamp routing scope, layout hydrates store from inventory"
```

---

## Phase 5: Native plugin spike — BLE + mDNS

Goal: A 1-day spike to decide which Capacitor plugins to use for BLE GATT and mDNS, validate them on iOS and Android, and write a thin service wrapper. After this phase Phase 6 (AddLamp wizard) can be planned in detail.

### Task 5.1: Spike `@capacitor-community/bluetooth-le`

**Files:**
- Create: `software/lamp-app/src/services/ble.ts`
- Create: `software/lamp-app/SPIKE.md` (notes only — delete before merge)

- [ ] **Step 1: Install the plugin**

```bash
cd software/lamp-app
npm install @capacitor-community/bluetooth-le
npx cap sync
```

- [ ] **Step 2: Add the iOS Info.plist string**

Edit `software/lamp-app/ios/App/App/Info.plist` and add:

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Used to find and configure your lamps.</string>
```

- [ ] **Step 3: Add the Android permission**

Edit `software/lamp-app/android/app/src/main/AndroidManifest.xml` and add inside `<manifest>`:

```xml
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
```

- [ ] **Step 4: Write a minimal service wrapper**

`software/lamp-app/src/services/ble.ts`:

```typescript
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
      const bytes = new Uint8Array(data as ArrayBuffer)
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
```

- [ ] **Step 5: Wire a temp button on Lamps page that calls `scanForLamps` and logs results**

In `software/lamp-app/src/pages/Lamps.vue`, add a temporary "Scan BLE" button that calls `initBle()` then `scanForLamps(console.log)`. (Remove after the spike.)

- [ ] **Step 6: Test on physical iOS device**

Build and deploy:

```bash
cd software/lamp-app && npm run build && npx cap sync && npx cap open ios
```

In Xcode, run on a connected iPhone. Approve the BLE permission prompt. Tap "Scan BLE" near a powered-on lamp. Confirm the lamp appears in the Xcode console with the expected colors.

- [ ] **Step 7: Test on physical Android device**

```bash
npx cap open android
```

Same flow in Android Studio.

- [ ] **Step 8: Document findings in `SPIKE.md`**

What worked, what didn't, gotchas (permission flow, scan duration limits, etc.). This informs Phase 6.

- [ ] **Step 9: Commit (keep the service code, delete the temp button before commit)**

```bash
git add software/lamp-app/src/services/ble.ts software/lamp-app/ios/App/App/Info.plist software/lamp-app/android/app/src/main/AndroidManifest.xml software/lamp-app/package.json software/lamp-app/package-lock.json software/lamp-app/SPIKE.md
git commit -m "spike(app): @capacitor-community/bluetooth-le validated on iOS + Android"
```

### Task 5.2: Spike mDNS

**Files:**
- Create: `software/lamp-app/src/services/mdns.ts`

- [ ] **Step 1: Evaluate plugin options**

Capacitor has no first-party mDNS plugin. Candidates:
- `@capacitor-community/zeroconf` (community, less active)
- `cordova-plugin-zeroconf` (older Cordova plugin; works in Capacitor with caveats)
- Custom Capacitor plugin wrapping `NSNetServiceBrowser` (iOS) + `NsdManager` (Android)

Try `@capacitor-community/zeroconf` first. If it doesn't work, fall back to a custom thin plugin.

- [ ] **Step 2: Install and integrate**

```bash
cd software/lamp-app
npm install @capacitor-community/zeroconf
npx cap sync
```

- [ ] **Step 3: Add `NSBonjourServices` to iOS Info.plist**

```xml
<key>NSBonjourServices</key>
<array>
  <string>_lamp._tcp</string>
</array>
<key>NSLocalNetworkUsageDescription</key>
<string>Used to find your lamps on the local network.</string>
```

- [ ] **Step 4: Write mDNS service wrapper**

`software/lamp-app/src/services/mdns.ts`:

```typescript
import { Zeroconf } from '@capacitor-community/zeroconf'

export interface MdnsLamp {
  name: string
  ip: string
  port: number
}

export async function scanMdnsLamps(
  onLamp: (lamp: MdnsLamp) => void,
  durationMs = 5_000,
) {
  const handle = await Zeroconf.watch(
    { type: '_lamp._tcp.', domain: 'local.' },
    (result) => {
      if (result.action === 'resolved' && result.service.ipv4Addresses?.[0]) {
        onLamp({
          name: result.service.name,
          ip: result.service.ipv4Addresses[0],
          port: result.service.port,
        })
      }
    },
  )
  setTimeout(() => { void Zeroconf.unwatch({ type: '_lamp._tcp.', domain: 'local.' }) }, durationMs)
}
```

- [ ] **Step 5: Test on iOS + Android against a lamp running new firmware**

This requires Phase 7 (firmware mDNS service record) to be done first, OR test against a hand-rolled `dns-sd -R "test" _lamp._tcp local 80` on a Mac.

- [ ] **Step 6: Document findings, then commit**

```bash
git add software/lamp-app/src/services/mdns.ts software/lamp-app/ios software/lamp-app/android software/lamp-app/package.json software/lamp-app/package-lock.json software/lamp-app/SPIKE.md
git commit -m "spike(app): mDNS plugin choice validated"
```

---

## Phase 6: AddLamp wizard

**OUTLINED — fill in detail after Phase 5 spike results are in.**

High-level flow (see spec):
1. AddLamp page mounts → starts BLE scan (Phase 5) + mDNS scan (Phase 5) in parallel.
2. Each discovered lamp is classified:
   - **Case A** (BLE advertising setup GATT service UUID — defined in firmware Phase 8) → "Nearby, not set up yet"
   - **Case B** (mDNS-discovered, not in inventory) → "On your WiFi"
3. User picks a lamp. Wizard branches based on case:
   - **Case A**: Connect via GATT, prompt for WiFi SSID + password + lamp name, write characteristics, trigger apply, wait for mDNS appearance of the new lamp, add to inventory.
   - **Case B**: Prompt for lamp password (if set), verify with `GET /settings`, add to inventory.
4. Both cases end with `router.replace({ name: 'lamp-home', params: { id } })`.

Tasks to be written after Phase 5: 6.1 AddLamp page UI; 6.2 BLE GATT client (Case A); 6.3 mDNS-already-configured client (Case B); 6.4 Add-to-inventory + navigation.

---

## Phase 7: Firmware — `_lamp._tcp` mDNS service record

Goal: Lamp advertises itself on the local network so the app can find it via Bonjour browsing.

### Task 7.1: Add mDNS service record

**Files:**
- Modify: `software/lamp-os/src/components/network/wifi.cpp`

- [ ] **Step 1: Read the current mDNS setup**

Inspect `software/lamp-os/src/components/network/wifi.cpp` around the existing `MDNS.begin("lamp")` (or similar — exact line varies). It currently advertises only the hostname.

- [ ] **Step 2: Add a service record after `MDNS.begin`**

After the existing `MDNS.begin(...)` call, add:

```cpp
MDNS.addService("lamp", "tcp", 80);
```

(ESP32 Arduino mDNS strips the leading underscore; the wire-level record is `_lamp._tcp`.)

- [ ] **Step 3: Verify with `dns-sd` from a Mac on the same network**

After flashing:

```bash
dns-sd -B _lamp._tcp local
```

Expected: The lamp appears in the listing within 5 seconds.

- [ ] **Step 4: Commit**

```bash
git add software/lamp-os/src/components/network/wifi.cpp
git commit -m "feat(firmware): advertise _lamp._tcp mDNS service for app discovery"
```

---

## Phase 8: Firmware — BLE GATT setup service

Goal: When the lamp is unconfigured, it exposes a connectable BLE GATT service through which the app writes WiFi credentials + lamp name.

### Task 8.1: Define the GATT service

**Files:**
- Create: `software/lamp-os/src/components/network/ble_setup.hpp`
- Create: `software/lamp-os/src/components/network/ble_setup.cpp`
- Modify: `software/lamp-os/src/components/network/bluetooth.cpp` (start/stop the setup service alongside the color-sync beacon)

- [ ] **Step 1: Pick stable UUIDs**

Generate once (any UUID generator, e.g. `uuidgen` on macOS):

```
Service:           5f64f4c1-d6d9-4a44-9b3f-3a8d6f7e6b40
Char ssid:         5f64f4c2-d6d9-4a44-9b3f-3a8d6f7e6b40
Char password:     5f64f4c3-d6d9-4a44-9b3f-3a8d6f7e6b40
Char name:         5f64f4c4-d6d9-4a44-9b3f-3a8d6f7e6b40
Char apply:        5f64f4c5-d6d9-4a44-9b3f-3a8d6f7e6b40
Char info (read):  5f64f4c6-d6d9-4a44-9b3f-3a8d6f7e6b40
```

Constants live in `ble_setup.hpp`.

- [ ] **Step 2: Write `ble_setup.hpp`**

```cpp
#pragma once
#include <Arduino.h>

namespace ble_setup {

constexpr const char* SERVICE_UUID  = "5f64f4c1-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_SSID     = "5f64f4c2-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_PASSWORD = "5f64f4c3-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_NAME     = "5f64f4c4-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_APPLY    = "5f64f4c5-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_INFO     = "5f64f4c6-d6d9-4a44-9b3f-3a8d6f7e6b40";

void start();
void stop();
bool isRunning();

}  // namespace ble_setup
```

- [ ] **Step 3: Implement `ble_setup.cpp` using NimBLE (already in the firmware)**

```cpp
#include "ble_setup.hpp"
#include <NimBLEDevice.h>
#include "../../config/config.hpp"

namespace ble_setup {

static NimBLEServer* server = nullptr;
static NimBLEService* service = nullptr;
static String pendingSsid;
static String pendingPassword;
static String pendingName;
static bool running = false;

class ApplyCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    if (pendingSsid.length() == 0) return;
    config::setWifiCredentials(pendingSsid, pendingPassword);
    if (pendingName.length() > 0) config::setLampName(pendingName);
    config::save();
    delay(200);
    ESP.restart();
  }
};

class FieldCallback : public NimBLECharacteristicCallbacks {
 public:
  FieldCallback(String* target) : target_(target) {}
  void onWrite(NimBLECharacteristic* c) override {
    *target_ = String(c->getValue().c_str());
  }
 private:
  String* target_;
};

void start() {
  if (running) return;
  if (!NimBLEDevice::getInitialized()) {
    NimBLEDevice::init(config::lampName().c_str());
  }
  server = NimBLEDevice::createServer();
  service = server->createService(SERVICE_UUID);

  service->createCharacteristic(CHAR_SSID, NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new FieldCallback(&pendingSsid));
  service->createCharacteristic(CHAR_PASSWORD, NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new FieldCallback(&pendingPassword));
  service->createCharacteristic(CHAR_NAME, NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new FieldCallback(&pendingName));
  service->createCharacteristic(CHAR_APPLY, NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new ApplyCallback());

  auto info = service->createCharacteristic(CHAR_INFO, NIMBLE_PROPERTY::READ);
  info->setValue(String("fw=") + FIRMWARE_VERSION + ";mac=" + WiFi.macAddress());

  service->start();

  auto adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();
  running = true;
}

void stop() {
  if (!running) return;
  NimBLEDevice::getAdvertising()->stop();
  if (server) {
    server->removeService(service, true);
  }
  running = false;
}

bool isRunning() { return running; }

}  // namespace ble_setup
```

(Adjust `config::setWifiCredentials` / `config::setLampName` calls to match the actual config API in `config_types.hpp` / `config.cpp`. Read those first to confirm signatures.)

- [ ] **Step 4: Start the setup service when the lamp is unconfigured**

In the main setup flow (likely `software/lamp-os/src/components/network/wifi.cpp` or `main.cpp`), before falling back to AP mode, call `ble_setup::start()` so the setup service runs alongside the AP. Stop it when WiFi credentials are stored and the lamp restarts into normal mode.

- [ ] **Step 5: Build and flash a test lamp**

```bash
cd software/lamp-os && python -m platformio run --target upload --environment upesy_wroom
```

- [ ] **Step 6: Verify with a BLE scanner app (nRF Connect)**

On an unconfigured lamp, scan with nRF Connect on a phone. Confirm the lamp advertises the GATT service UUID, that all five characteristics are present, and that writes to ssid/password/apply trigger a restart.

- [ ] **Step 7: Commit**

```bash
git add software/lamp-os/src/components/network/ble_setup.hpp software/lamp-os/src/components/network/ble_setup.cpp software/lamp-os/src/components/network/bluetooth.cpp software/lamp-os/src/components/network/wifi.cpp
git commit -m "feat(firmware): BLE GATT setup service for app-driven provisioning"
```

---

## Phase 9: Firmware — captive portal landing page + remove Vue bundle

Goal: Replace `software/lamp-os/data/index.html.gz` (the Vue bundle) with a tiny static landing page that points users to the App Store / Play Store. Same response served for every captive-portal probe URL.

### Task 9.1: Author the landing page

**Files:**
- Create: `software/lamp-os/data/index.html` (no `.gz`, no compression — it's tiny)
- Delete: `software/lamp-os/data/index.html.gz`

- [ ] **Step 1: Write the HTML**

```html
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>Lamp</title>
  <style>
    :root { color-scheme: dark; }
    body {
      margin: 0; min-height: 100vh; display: grid; place-items: center;
      background: #0b0d12; color: #f4f4f5; font-family: -apple-system, system-ui, sans-serif;
      padding: 24px; text-align: center;
    }
    main { max-width: 360px; }
    h1 { font-size: 1.5rem; margin: 24px 0 8px; }
    p { color: #a1a1aa; line-height: 1.5; }
    .badges { margin-top: 32px; display: flex; flex-direction: column; gap: 12px; }
    .badge {
      display: inline-block; padding: 14px 24px; border-radius: 12px;
      background: #fff; color: #000; text-decoration: none; font-weight: 600;
    }
  </style>
</head>
<body>
  <main>
    <h1>Welcome to your Lamp</h1>
    <p>Install the Lamp app to set up and control your lamp.</p>
    <div class="badges">
      <a class="badge" href="https://apps.apple.com/app/idTBD">App Store</a>
      <a class="badge" href="https://play.google.com/store/apps/details?id=com.lamplit.lamp">Google Play</a>
    </div>
  </main>
</body>
</html>
```

- [ ] **Step 2: Update firmware to serve this for every captive portal probe**

In `software/lamp-os/src/components/network/wifi.cpp`, replace the existing captive-portal handlers (`/generate_204`, `/hotspot-detect.html`, etc.) with a single handler that serves `/index.html`. The exact code depends on the existing web server framework; the principle is "always respond with the landing page, never 204."

- [ ] **Step 3: Delete the gzipped Vue bundle**

```bash
rm software/lamp-os/data/index.html.gz
```

- [ ] **Step 4: Build the filesystem image and verify**

```bash
cd software/lamp-os && python -m platformio run --target buildfs --environment upesy_wroom
```

Measure the new image size; confirm it's substantially smaller than before.

- [ ] **Step 5: Flash and test**

Flash the firmware + filesystem to a test lamp. From a phone, join the lamp's AP. Confirm the captive portal opens the landing page on iOS and Android.

- [ ] **Step 6: Commit**

```bash
git add software/lamp-os/data software/lamp-os/src/components/network/wifi.cpp
git commit -m "feat(firmware): replace Vue bundle with install-the-app captive portal page"
```

---

## Phase 10: End-to-end verification

### Task 10.1: Full verification sweep (per spec verification section)

Run through every numbered test in the verification section of the design doc at `/Users/jerrett/.claude/plans/what-would-it-take-velvety-music.md`. Each test gets its own checklist item; failures get new tasks created.

- [ ] **Tests 1, 2** — Everyday flow on iPhone + Android with two seeded lamps
- [ ] **Test 3** — Feature parity sweep across Home/Expressions/Setup/Info screens
- [ ] **Test 4** — Add a brand-new lamp (Case A, BLE GATT)
- [ ] **Test 5** — Add a lamp already on the network (Case B, mDNS + password)
- [ ] **Test 6** — Backwards compat with old firmware (HTTP/WS works, BLE add does not)
- [ ] **Test 7** — Captive portal with install-the-app page on a phone without the app
- [ ] **Test 8** — Firmware flash savings measurement

---

## Self-Review Notes

- **Spec coverage:** All sections of `/Users/jerrett/.claude/plans/what-would-it-take-velvety-music.md` are addressed by a phase. Phase 6 (AddLamp wizard) is intentionally outlined rather than fully detailed because plugin choices in Phase 5 will inform its API surface.
- **WIP blocker (Phase 0)** must run first; subagents working on phases 1–6 will see a wrong file layout otherwise.
- **Firmware phases (7, 8, 9)** are independent of app phases (0–6) and can be dispatched in parallel.
- **Phase 6 stays outlined on purpose.** Detailing it now would require fabricating API shapes for plugins we haven't validated. Fill it in after Phase 5 lands.
