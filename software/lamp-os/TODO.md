# Follow-ups from PR #92 audit

Found while cleaning up `topic/breath-expression`. Intentionally **not** part of that PR so the breathing-expression review can land cleanly — file as separate work afterward.

## 1. Replace `std::mt19937` with a lightweight RNG across all expressions

- **Where:** `src/expressions/expression.hpp:40` — `std::mt19937 rng{esp_random()};`
- **Cost today:** ~2.5 KB SRAM per expression instance (mt19937 state = 624 × uint32_t). With 4 expression types instantiated, ≈10 KB of SRAM spent on randomness none of them needs.
- **Proposed:** Replace with a seeded `xorshift32` (8 bytes) or LCG. ESP32 already exposes `esp_random()` for seeding.
- **Touches:** Every expression that calls `getRandomColor()` / draws from `rng` (`expression.cpp`, `shifty_expression.cpp::getRandomShiftDuration`, anywhere using `std::uniform_int_distribution`).

## 2. Unified empty-`colors` fallback policy

After PR #92: breathing and pulse both fall back to `Color(0, 0, 0, 255)` (W-channel only). Shifty does the same at `startShift()`. Glitchy doesn't have a `configureFromParameters()` path that takes colors — it uses `getRandomColor()` from the palette.

- **Where:** Each expression's `configureFromParameters()` and any state-entry paths.
- **Proposed:** Either lift the empty-check into `Expression::configure()` (one place, one policy) or define a static `kSafeFallbackColor` constant in `expression.hpp` so all four expressions reference the same value.
- **Why post-merge:** Lifting into the base class is a contract change that affects every expression's configure flow — too much for the breathing PR.

## 3. Document or remove `pulse_expression.cpp:80` deltaMs cap

```cpp
uint32_t deltaMs = std::min(currentMs - lastUpdateMs, (uint32_t)100);
```

- **Question:** Why 100 ms? Likely a workaround for frame-rate stutter, but no comment explains it.
- **Action:** Either add a `// Why:` comment with the original reasoning, or remove the cap and rely on the frame-rate clamp at the compositor.

## 4. (Lower priority) Audit findings worth a pass later

- **`breathing_expression.cpp:62`** — `if (currentColorIndex >= colors.size() - 1)`. Safe today because we always check `colors.size() > 1` first, but the unsigned subtraction is a footgun if the guard ever moves.
- **Multiple `millis()` reads per frame** across expressions — could be hoisted to one read at the compositor tick and threaded through `control()` / `draw()` as a `frameStartMs` argument. Micro-optimization; only worth doing if profiling shows it matters.
- **Empty no-op blocks** in `glitchy_expression.cpp:65–66` and `pulse_expression.cpp:167–168` — dead `if` bodies; remove or convert to TODO comments.

---

# Follow-ups from ESP-NOW grid + NearbyLamps unification + UI work

Captured at end of the session that landed:
- ESP-NOW grid mesh (HELLO + COLORS + relay) at `LAMP_HELLO_INTERVAL_MS = 2000`
- BLE → ESP-NOW control bridging via `CHAR_REMOTE_OP` (brightness, colors, knockout, expressionOp, mqtt forwardable; settings deferred)
- Unified `NearbyLamps` store replacing `BluetoothPool` + `ShowReceiver::peers_`
- BLE LESC pairing + WRITE_ENC on AUTH / WIFI_OP / SETTINGS_BLOB / MQTT_OP / REMOTE_OP
- Smart Home (MQTT) form lives in Setup's Home Mode section (gated on `homeSsid`), out of the Choose-Home-Network sheet
- Three-state status dot (grey / dim green / bright pulsing green) and a vector `LampIcon.vue`

## Built but never end-to-end verified

- **MQTT / Home Assistant integration.** No real broker exercised yet. Expect a Light entity with brightness + on/off once connected.
- **Remote-op forwarding for expressionOp / mqtt / knockout.** Brightness + colors tested across two lamps; the others share the same code path but the UI hasn't actually been used against a remote target.
- **BLE LESC encryption on the wire.** Pairing works; haven't sniffed traffic to confirm payloads are encrypted vs cleartext. ~30 s with an nRF sniffer would close it.

## V1 limitations explicitly documented

- **Remote-read protocol.** Configuring a remote lamp shows the LOCAL lamp's data as a starting point — far lamp's actual config isn't visible. Banner documents this.
- **`MSG_CONTROL_ACK` / retries.** Send-and-pray. Reliability comes from flood-relay.
- **Settings-blob forwarding.** Can't save remote-lamp settings via the mesh in v1 (would reboot the remote — UX over the grid TBD).
- **Live updates of Nearby Lamps list.** Removed the rate-limited `notifyNearbyLamps()` because the payload occasionally exceeded MTU on Android and caused flicker. The list updates only on bootstrap read for now. A leaner-payload notify path is a natural followup.
- **GATT Service Changed indication.** Every BLE service-profile change currently requires manual Android cache clearing. Adding the standard `0x2A05` Service Changed characteristic auto-busts the cache. ~10-line firmware change; saves the next-time-you-flash-schema dance.

## Larger ideas raised but not started

- **OTA firmware updates.** Info-page button that polls a release manifest URL when on home WiFi, downloads `firmware.bin`, verifies hash, flashes via `FadeOutBehavior` reboot. Gotcha: `partitions.csv` needs to be OTA-capable; the first OTA-enabling flash must go over USB.
- **Web UI hybrid.** Small `ESPAsyncWebServer` bound to STA, serves a stripped-down `lamp-ui` for desktop browser config when the lamp is on home WiFi. BLE stays the primary control path.
- **Repeater repurpose.** `software/artnet-repeater/` hardware needs a new identity now that ArtNet is gone. Likely "scene controller" emitting scripted COLORS into the grid.

## Stale concerns to verify or close

- **WiFi connect** was reported "doesn't seem to work anymore" mid-session but not retested after the Smart Home form was moved out of the sheet. Likely working now — confirm next setup.
- **Glitchy expression white-out crash.** Test-callback race fix landed; heavy diagnostic logging exists if it recurs. Hasn't been reported since.

## Suggested next-up (high payoff / low effort)

1. **GATT Service Changed indication** — small firmware add, removes the "clear Android Bluetooth cache" dance from every future schema change.
2. **Exercise MQTT with a real broker** before you need it for an event.
