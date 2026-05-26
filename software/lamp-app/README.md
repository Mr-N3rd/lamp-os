# Lamp Mobile App

iOS + Android app for discovering, adding, and controlling Lamp devices over WiFi (HTTP REST + WebSocket) and BLE.

Built with Capacitor + Vue 3 + Pinia + TypeScript. Reuses components and state logic from `software/lamp-ui/`.

## Testing the app

Three modes, in order of speed.

### Browser dev — fastest, but no BLE / mDNS

```bash
npm install
npm run dev
```

Opens `http://localhost:5173`. The AddLamp scan won't find anything (no Web Bluetooth / mDNS in browsers), but you can seed an inventory entry in devtools to exercise the per-lamp screens:

```js
localStorage.setItem('CapacitorStorage.lamp-inventory', JSON.stringify([
  { id: 'test', name: 'desk', lastIp: '192.168.1.42', addedAt: Date.now() }
]))
location.reload()
```

Point `lastIp` at a real lamp's IP — the Home / Setup / Expressions / Info screens connect normally over HTTP + WS.

### iOS device — real test (BLE + mDNS work)

```bash
npm run build && npx cap sync
npx cap open ios
```

In Xcode:
1. Plug in your iPhone
2. Settings → Accounts: sign in with your Apple ID (personal team works for sideload — 7-day expiry)
3. Project navigator → `App` target → Signing & Capabilities → set Team
4. Pick your iPhone as the run destination
5. ▶ (Cmd+R)
6. On first run, iPhone will prompt: trust this developer (Settings → General → VPN & Device Management)

### Android device — real test

```bash
npm run build && npx cap sync
npx cap open android
```

Plug in an Android phone with USB debugging enabled, hit ▶ in Android Studio. No developer account needed.

### Full Case A (brand-new lamp via BLE GATT) test

1. Flash a lamp with the new firmware: `cd ../lamp-os && ~/.local/bin/pio run -t upload -e upesy_wroom`
2. Lamp must be unconfigured (factory reset or no WiFi credentials)
3. App: "Add a lamp" → lamp appears under "Nearby" → tap → fill SSID/password/name → "Set up" → wait ~30 s for lamp to join WiFi → lands on the lamp's Home screen

## Tests

```bash
npx vitest run       # 52 unit tests
npx vue-tsc --noEmit # type-check
npm run build        # full production build (lints types via vue-tsc -b)
```

## Architecture

```
src/
├── pages/
│   ├── Lamps.vue           Your lamps + live BLE/mDNS reachability polling
│   ├── AddLamp.vue         Wizard: scan + Case A (BLE GATT) + Case B (mDNS)
│   ├── Index.vue           Lamp Home screen (per-lamp)
│   ├── Setup.vue           Lamp Setup screen (per-lamp)
│   ├── Expressions.vue     Expression editor (per-lamp)
│   ├── Info.vue            Lamp info (per-lamp)
│   └── Form.vue            Generic form utility
├── layout/Lamp.vue         Per-lamp shell — hydrates store from route :id
├── stores/
│   ├── lampInventory.ts    Your lamps, persisted to Capacitor Preferences
│   ├── lamp.ts             Selected lamp's state + WS + REST (multi-target)
│   └── expressions.ts
├── services/
│   ├── ble.ts              BLE scan + GATT setup write
│   ├── mdns.ts             mDNS browse via capacitor-zeroconf
│   └── scan.ts             BLE+mDNS aggregator for AddLamp
├── components/             Field components, expression editor, form, etc.
└── plugins/
    ├── router.ts           Routes: /, /add, /lamp/:id/{home,...}
    └── globalComponents.ts Registers field components for dynamic form rendering
```

### Routing model

- `/` — Lamps list (your curated inventory)
- `/add` — Add a Lamp wizard
- `/lamp/:id/{home,expressions,setup,info}` — Per-lamp screens, scoped by inventory id

The Lamp layout (`layout/Lamp.vue`) reads `:id` from the route, looks up the lamp in `lampInventory`, builds an HTTP+WS target, and calls `lampStore.initialize(target)` on entry / `setTarget(...)` on change.

### Adding a lamp

**Case B — Lamp already on your home WiFi:** Discovered via mDNS (`_lamp._tcp`) and/or BLE color-sync beacon. User taps it, enters password if set, app verifies with `GET /settings`, adds to inventory.

**Case A — Brand new (unconfigured) lamp:** Discovered via BLE setup GATT service (`5f64f4c1-…`) advertised by lamps in unconfigured mode. User fills SSID/password/name form. App writes credentials via GATT, triggers `apply` which reboots the lamp. App polls mDNS for up to 60 s waiting for the lamp to join the network and appear by name.

### Native capabilities

- BLE: `@capacitor-community/bluetooth-le` — scan, connect, GATT write
- mDNS: `capacitor-zeroconf` — browse `_lamp._tcp`
- Preferences: `@capacitor/preferences` — persists inventory

iOS Info.plist has:
- `NSBluetoothAlwaysUsageDescription`
- `NSLocalNetworkUsageDescription`
- `NSBonjourServices: ["_lamp._tcp"]`

Android manifest has:
- `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, `ACCESS_FINE_LOCATION`

## Conventions

- All deps pinned to exact versions (no `^`) — match `lamp-ui` where shared
- WebSocket protocol unchanged from `lamp-ui` — see `stores/lamp.ts` action set
- Components copied (not symlinked) from `lamp-ui` for v1; revisit shared-package extraction if maintenance overhead becomes painful
