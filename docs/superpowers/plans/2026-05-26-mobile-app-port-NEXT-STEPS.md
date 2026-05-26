# Mobile App Port — What's left

State: implementation phases 1-9 complete. 23 commits on `worktree-mobile-app-port`. Not pushed (per user instruction). What's left is physical-device validation, store-publishing chores, and a few v2 features that were intentionally deferred.

## Physical device verification

None of this can run in an autonomous session. Each test below is a thing to actually do on hardware.

- [ ] **App boots on iPhone.** `cd software/lamp-app && npm run build && npx cap sync && npx cap open ios`, run on a connected iPhone. Confirm the empty Lamps page shows "No lamps yet."
- [ ] **App boots on Android.** Same flow via `npx cap open android`.
- [ ] **Case B (already-on-network) end-to-end on iPhone.** Flash an existing lamp on home WiFi. Open the app → "Add a lamp" → confirm it appears under "On your WiFi" within 5 s → tap → enter password (or leave empty) → confirm `GET /settings` succeeds → lands on lamp Home screen. Drag brightness slider — confirm real-time lamp response.
- [ ] **Case B on Android.** Same.
- [ ] **Case A (brand-new) end-to-end.** Flash a factory-state lamp with the new firmware (containing Phase 8 BLE GATT service). From the app: "Add a lamp" → confirm it appears under "Nearby" within 5 s → tap → fill out SSID + password + name → tap "Set up" → wait for "Waiting for lamp…" → confirm lamp appears on home WiFi within 60 s → lands on lamp Home screen.
- [ ] **Multi-lamp dashboard.** Add two lamps. Open one, drag a slider, back out, open the other, drag a slider. No leaked WebSocket, no stale state. Online dots refresh every 30 s.
- [ ] **Feature parity.** Walk through every control on each screen (Home, Expressions, Setup, Info) and compare against the current web UI. Pay attention to:
  - Brightness/shade realtime preview (regression risk: WS debounce from `ee3047a`)
  - Slider drag handling (regression risk: prop-sync-during-drag from `1958aef`)
  - Expression editor add/remove/preview
  - Knockout pixels (advanced setup)
- [ ] **Captive portal landing.** From a phone *without* the app installed, join an unconfigured lamp's AP. Confirm the captive portal opens the install-the-app landing page. Verify both store badge links work (after URLs are real — see next section).
- [ ] **Backwards compat.** Install the app on phone, point it at an *old* lamp running pre-Phase-7 firmware (no `_lamp._tcp` mDNS service). Confirm:
  - Lamp does NOT appear in AddLamp's "On your WiFi" list (no mDNS record)
  - Manually adding by IP (if the app gains that UI later) still works via `/settings` + `/ws`
  - This is acceptable — old firmware can be updated to gain discoverability
- [ ] **Firmware flash savings confirmed.** Phase 9 reported `data/` dropped from ~68 KB to 4.5 KB. Build a release firmware image and compare partition usage to a pre-port build.

## App Store / Play Store publishing

- [ ] **Apple Developer account** — needed before TestFlight or App Store. ~$99/yr.
- [ ] **Google Play Developer account** — one-time $25.
- [ ] **App icons** — Capacitor scaffolds are still the default. Generate icons from the Lamp logo (see `software/lamp-app/public/logo.svg`). Use `@capacitor/assets` or hand-export per platform.
- [ ] **Splash screen** — branded splash, especially on iOS where the launch screen storyboard is empty.
- [ ] **TestFlight build** — provision a first internal build for testing on a few personal devices.
- [ ] **Replace placeholder store URLs** in `software/lamp-os/data/index.html` once the app listings exist:
  ```html
  <a class="badge" href="https://apps.apple.com/app/idTBD">App Store</a>
  <a class="badge" href="https://play.google.com/store/apps/details?id=com.lamplit.lamp">Google Play</a>
  ```
- [ ] **Apple privacy disclosures** — declare BLE + Local Network usage in App Store Connect.

## Deferred polish items (v2)

- [ ] **mDNS plugin Swift Package Manager fallback** (`cap sync` warns that `capacitor-zeroconf` lacks a `Package.swift`). It works via the legacy SPM shim; verify the iOS native compile succeeds in Xcode. If not, write a custom mDNS plugin or pin to a newer release.
- [ ] **GATT info characteristic** — currently returns just `mac=XX:XX:…`. Could add firmware version, free heap, etc. for a richer "Info" tab on brand-new lamps before they're configured.
- [ ] **Inline AddLamp UX polish** — replace `window.alert` calls with toast / banner. Replace the BLE/mDNS "Scanning…" copy with a spinner.
- [ ] **Manual-IP add path** — for users behind multi-VLAN setups where mDNS doesn't traverse. Add a "or enter the IP manually" button on AddLamp.
- [ ] **Lamp inventory iCloud / Drive sync** — currently inventory is local-only. Sync across the user's devices.
- [ ] **Background mDNS** for push-style "your lamp is online" notifications.
- [ ] **Extract shared package** — `lamp-app/src/components/*` was copied (not workspace-shared) from `lamp-ui/src/`. If maintenance becomes painful, extract a `lamp-ui-shared` workspace package.

## Cleanup / housekeeping

- [ ] **Resolve the parent beta uncommitted state.** When user is ready to merge worktree back to beta, they should:
  - Commit (or stash + apply) any remaining changes on `beta` working tree (none should remain — Phase 9 superseded the captive portal redirect tweak, and Phase 9 also rebuilt the firmware data dir)
  - `git merge worktree-mobile-app-port` from beta
  - Then push when ready
- [ ] **Delete or document the `software/lamp-ui` package's future role.** Now that the mobile app exists, lamp-ui is no longer the primary control UI. It still builds the captive portal page (theoretically — though Phase 9 wrote index.html by hand). Decide: keep as legacy / dev-tool, or sunset entirely.
- [ ] **Replace `placeholder` `appId` `com.lamplit.lamp`** if the chosen bundle ID needs to differ (e.g., if `lamplit` ownership status is uncertain).

## Tasks that depend on someone else

- **App Store review** (Apple): expect 1-3 days. WebView-wrapped apps get extra scrutiny but ours is a real native app with BLE + mDNS, not a website wrapper.
- **Play Store review** (Google): typically same-day for established accounts.
