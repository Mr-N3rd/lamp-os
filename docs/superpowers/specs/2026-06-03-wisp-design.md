# Wisp — palette bridge + mesh maintenance node

## Context

The `software/artnet-repeater/` board (Seeed Xiao ESP32-C6 with a remote antenna) is dedicated infrastructure looking for a new identity now that ArtNet is gone (see `software/lamp-os/TODO.md:65`). At the same time we have two unmet needs in the lamp ecosystem:

1. **Live palette following.** When the lamps are at a show driven by an Aurora device, the user wants the whole grid to drift to whatever color palette the Aurora is currently advertising — without manually editing each lamp, and without losing the ability for expressions (Glitchy, Pulse, etc.) to keep running over the top.
2. **Mesh firmware push.** Today, updating lamps means USB-flashing each one. We want one piece of infrastructure that carries the newest signed firmware and pushes it to any lamp on the mesh running an older version, with safe rollback if something breaks.

Both needs land on the same hardware. The board has WiFi (for Aurora ingest), enough flash for a carried firmware blob, an external antenna (reach across a deployment), and is already a non-lamp piece of infrastructure (doesn't compete with lamp activity for radio or CPU). This spec calls the rebuilt device **wisp** — a small magical light that signals to others. New directory `software/wisp/` rather than mutating the old artnet-repeater tree; the old code stays as a reference.

**Goal:** wisp discovers an Aurora on the LAN, follows its palette, distributes paired colors out to the lamp mesh as transient base-color overrides, and acts as the carrier for signed lamp firmware updates pushed over ESP-NOW. The user sees what wisp is doing — and toggles whether it paints — from a new pane in the app reached via any connected lamp.

## Architecture

### Key decisions

- **Wisp has two jobs on one board.** Palette bridge (primary) and firmware carrier (secondary). They share the ESP-NOW transport, the lamp inventory, and the status surface.
- **Palette paint is off by default at boot.** Discovery, status reporting, mesh-inventory tracking, and firmware push all run unconditionally. Paint requires explicit user opt-in via the app each boot. Safer default — a power blip doesn't surprise lamps.
- **Transient overrides, never NVS.** Wisp sends `MSG_WISP_PAINT` over ESP-NOW with a color tuple. The lamp swaps its base configurator's color array in RAM but never calls the NVS-persisting code path. On wisp dropping (or paint mode off), the lamp re-renders its saved base. The user's stored config is untouched.
- **Two colors per lamp, deterministic by MAC.** Each lamp samples two positions on the gradient seeded by its MAC; same lamp always gets the same pairing for a given palette, so the visual feels designed rather than random. Spread across the gradient by hash distribution so neighbors don't all look the same.
- **Wisp has no BLE.** The app talks to wisp via any nearby lamp acting as a proxy. The lamp caches the latest `MSG_WISP_HELLO` it overheard and exposes it on a new BLE characteristic. Writes go the other direction: lamp receives a BLE write, re-broadcasts it as `MSG_WISP_OP` over mesh.
- **Wisp WiFi creds via mesh, not USB.** Wisp boots with no WiFi configured; it joins the mesh over ESP-NOW immediately (the radio doesn't need WiFi association for that) and advertises `wifiConnected=false` in its hello. The user connects via BLE to any lamp, opens the Wisp pane, and configures the SSID + password from there. Wisp receives the op via the mesh proxy, attempts to associate, and persists on success. Out of the box: flash wisp once over USB, plug it in, configure from the app.
- **Reuse the existing palette client code.** `~/Downloads/esp32-palette-client/` already implements mDNS + WebSocket + protobuf decode + HTTP palette fetch for Aurora. We vendor its `src/` and `lib/miniz/` into `software/wisp/src/aurora/`. The non-Arduino pieces test under `pio test -e native` the same way lamp-os pure-logic does.
- **Aurora protocol is NOT OSC.** It's mDNS discovery + WebSocket subscription + Protobuf message envelope + HTTP for palette colors. The reference handles all of this; we adopt as-is.
- **Ed25519-signed firmware, build-time.** A post-build script signs `firmware.bin` with a private key kept outside the repo. Public key baked into every lamp build. Wisp carries the signed firmware blob; the lamp's OTA receiver verifies the signature against the embedded public key before applying. A peer with a rogue ESP32-C6 cannot push arbitrary firmware.
- **Firmware channels are build-time, not runtime.** A lamp build is tagged with a `FIRMWARE_CHANNEL` constant (`"stable"`, `"beta"`, etc.) baked into both the binary and the LSIG signature footer. Wisp doesn't have internet access so it can't switch channels by URL; instead the developer builds the channel they want, signs it, and pushes that signed blob to wisp. The Wisp pane displays the channel + version of what wisp is currently carrying. Lamps only accept OTA offers from a matching channel — a stable lamp ignores a beta wisp; a beta lamp ignores a stable wisp. Cross-channel migration only happens via USB.
- **Single-peer-at-a-time OTA.** Wisp pushes one lamp at a time, retries naturally via its rescan loop. Simpler than parallel; the practical case is "I signed a new version, let it propagate over the next hour."
- **ESP-IDF rollback for free.** Lamp marks the new partition `PENDING_VERIFY` until it has reached steady-state in `setup()`; if it crashes earlier, ESP-IDF auto-reverts on next boot. Manual rollback exposed as an advanced-only BLE write + Setup row.

### Out of scope

- Wisp self-OTA. Wisp is flashed via USB; mesh OTA flows wisp → lamps only.
- Multiple wisps on one mesh. Single carrier in v1; document don't enforce.
- Per-zone palette assignment (Aurora supports multiple "zones"). v1 listens to the first zone seen and ignores the rest.
- Multi-color tuples (>2 colors per lamp). The protocol's `numColors` field lets a future wisp send more without a lamp-side change.
- Compression / chunk-size negotiation. Hard-coded 200 bytes raw for OTA chunks.
- Cross-version protocol negotiation. If we ever break the wire format, bump the protocol byte; older lamps drop frames they don't understand.

## System diagram

```
+---------------+        WiFi LAN          +-------------+      ESP-NOW mesh      +--------+
|  Aurora       |  mDNS + WS + HTTP   --→  |   wisp      |  ──── unicast paint ─→ | lamp 1 |
|  device       |  ←──  subscribe          | (ESP32-C6)  |  ──── unicast paint ─→ | lamp 2 |
+---------------+                          +-------------+  ──── broadcast HELLO  | lamp N |
                                                ↑ ↑      |  ←──  HELLO + version  +--------+
                                                | |      |
                                                | └─ status query (read-only, via mesh proxy)
                                                |
                                                └─ phone (BLE → any lamp → mesh → wisp)
```

The phone never reaches wisp directly. App → BLE to any lamp → that lamp surfaces wisp's last-overheard status via a new characteristic, and forwards ops to wisp via a new mesh message.

## Subsystems

### 1. Mesh awareness (Phases 0/A)

Every lamp adds a `uint32_t firmwareVersion` field to its periodic `MSG_HELLO` beacon. Wisp listens to HELLOs and builds a `LampInventory` keyed by MAC, tracking `{name, baseColor, shadeColor, firmwareVersion, lastSeenMs}`. Pruned at the same `LAMP_PRUNE_TIME_MS` (2 min) as the lamp-side `NearbyLamps`. This inventory is what the app's wisp pane shows.

### 2. Aurora ingest (Phase B)

Vendored from `~/Downloads/esp32-palette-client/`. The reference's `AuroraPaletteClient` is a facade running discovery → connect → subscribe → resolve. It calls back `onActivePalette(zone, Palette)` whenever the active palette changes. Wisp listens to the first zone and stores the resolved palette in a `CurrentPalette` holder — vector of `{r,g,b,w}` colors parsed from either `palette.hexColors` (w=0) or `palette.colors[].{r,g,b,w}`. Aurora's `w/am/u` channels map onto our `w`; we ignore amber and UV.

### 3. Tuple sampler + mesh paint (Phase C)

When a new palette resolves AND paint mode is on, wisp computes per-lamp color assignments:

1. **Normalize the gradient stops.** Sort, dedupe near-identical (Δ < ~8/255 per channel), and interpolate down to ≥4 stops if Aurora published fewer.
2. **For each lamp** (each MAC in the inventory):
   - `a = (hash32(mac) % 1000) / 1000.0` — first position on `[0,1]`
   - `b = (hash32(mac ^ 0x9E3779B9) % 1000) / 1000.0` — second position; XOR with the golden-ratio constant gives uncorrelated positions without a second RNG
   - Sample the gradient at `a` and `b`. If `|a - b| < 0.15`, re-roll up to 3 times to avoid sad near-identical pairings.
3. **Send** `MSG_WISP_PAINT` unicast to each lamp with `[r,g,b,w]×2`.

Re-paint triggers: palette change (fan-out across all lamps, paced); new lamp joining the mesh (HELLO seen for a previously-unknown MAC); per-lamp backstop every 10 s in case a frame was missed.

On paint mode toggle off: wisp broadcasts `MSG_WISP_RELEASE` (no targetMAC) and lamps drop their override.

### 4. Lamp-side override (Phase C, lamp)

The base configurator already reads from `config.base.colors` to render its gradient. On `MSG_WISP_PAINT`, the lamp swaps that vector for the wisp-provided colors and marks an "override active" timestamp. On `MSG_WISP_RELEASE` (or watchdog: no paint in 60 s + no `MSG_WISP_HELLO` in 30 s), the override clears and the saved `config.base.colors` re-renders. Expressions are unchanged — they paint over whatever base is current.

### 5. App proxy (Phase D)

Wisp broadcasts `MSG_WISP_HELLO` every 2 s carrying `{wispVersion, paintMode, paletteIdPrefix, auroraConnected, wifiConnected, carriedFirmwareChannel(8), carriedFirmwareVersion(4)}`. Lamps cache the latest one in `NearbyLamps`. A new BLE characteristic `CHAR_WISP_STATUS` (encrypted, read+notify) returns a JSON view: wisp's cached status plus the lamp's `NearbyLamps` inventory with firmware versions and current paint colors. A second characteristic `CHAR_WISP_OP` (write-only, encrypted) takes `{op, value}` JSON; the lamp re-broadcasts as `MSG_WISP_OP` to wisp, which acts on it and rebroadcasts `MSG_WISP_HELLO` immediately for fast feedback.

Supported ops in `MSG_WISP_OP`:

| Op | Payload | Purpose |
|---|---|---|
| `setPaintMode` | `{value: bool}` | Toggle whether wisp is actively painting lamps |
| `setWifi` | `{ssid: str, password: str}` | Configure wisp's WiFi (also forgets prior creds) |
| `forgetWifi` | `(none)` | Drop saved creds so wisp goes back to mesh-only |
| `forceFirmwareScan` | `(none)` | Accelerate the OTA scan tick once (Phase F) |

App side: `features/wisp/` with the usual Riverpod + presentation split. A new "Wisp" row in the Setup tab (gated on `advancedSessionProvider`) drills into `WispScreen` — status header (wisp version + WiFi status + Aurora status + carried firmware channel + version), paint mode toggle, WiFi configure button if disconnected, lamp list rendering each peer with the existing `LampIcon` widget colored by the peer's current base + shade and its firmware version label.

### 6. Signed firmware (Phase E)

Post-build script appends a 96-byte LSIG footer to `firmware.bin`:

```
[magic 'LSIG'        ( 4 B)]
[footer_version       ( 1 B)] (= 1)
[reserved             ( 3 B)]
[channel              ( 8 B)] (null-padded ASCII, e.g. "stable\0\0" or "beta\0\0\0\0")
[firmware_version     ( 4 B)] (= FIRMWARE_VERSION constant — semver packed)
[firmware_byte_length ( 4 B)] (bytes covered by the signature)
[reserved             ( 8 B)] (room for future fields)
[signature            (64 B)] (ed25519 over sha256(firmware_bytes ++ first 32 bytes of footer))
```

Private key lives outside the repo (default `~/.config/lamp-os/firmware_signing_key.ed25519`). Public key emitted as a generated `scripts/keys/firmware_pubkey.h` baked into the lamp build. Verify code (`verifySignedFirmware`) uses mbedTLS ed25519, which ships with ESP-IDF.

Channel is set per build environment in `platformio.ini`: `[env:upesy_wroom]` sets `-D FIRMWARE_CHANNEL='"stable"'`, `[env:upesy_wroom_beta]` sets `-D FIRMWARE_CHANNEL='"beta"'`. The constant ends up in both the running binary (so `MSG_HELLO` can advertise it later if we ever care) AND the LSIG footer (so wisp + lamp OTA can enforce channel match). The push-to-wisp script reads the channel from the footer of the file it's uploading and reports it back to the user.

### 7. Mesh OTA (Phase F)

Six new ESP-NOW message types for the OTA protocol:

| Type | Direction | Payload |
|---|---|---|
| `MSG_FW_OFFER (0x30)` | wisp → lamp | `version(4) + totalLen(4) + chunkSize(2) + channel(8) + sha256Prefix(8)` |
| `MSG_FW_ACCEPT (0x31)` | lamp → wisp | `(empty)` |
| `MSG_FW_CHUNK (0x32)` | wisp → lamp | `offset(4) + len(2) + bytes(≤200)` |
| `MSG_FW_REQ (0x33)` | lamp → wisp | `offset(4) + len(2)` — resend |
| `MSG_FW_DONE (0x34)` | wisp → lamp | `version(4) + totalLen(4)` — verify now |
| `MSG_FW_RESULT (0x35)` | lamp → wisp | `code(1)` — 0=verified+rebooting, 1=verify failed, 2=write error |

Wisp state machine: Idle → Offering → Streaming → Finalizing → Idle. Idle scan every 5 s picks the lowest-version out-of-date lamp from inventory. Streaming paces chunks off the `esp_now_send` completion callback (no tight loop). On any timeout: back to Idle; next scan retries the same peer naturally.

Lamp state machine: Idle waits for OFFER → accept if `offer.channel == FIRMWARE_CHANNEL` AND `offer.version > FIRMWARE_VERSION` → `esp_ota_begin` on inactive partition → write chunks at-offset with `esp_ota_write_with_offset` → maintain received-range bitmap → request gaps on stall. On DONE: `esp_ota_end`, read back, `verifySignedFirmware`. Valid → `esp_ota_set_boot_partition` → `MSG_FW_RESULT(0)` → `esp_restart`. Invalid → `MSG_FW_RESULT(1)` → abort. Failed writes never touch the active partition.

Wisp ingest: HTTP server on port 80. `POST /firmware` validates LSIG footer + signature, stores in a dedicated `firmware_payload` partition (with an NVS "ready" marker so half-written uploads don't get distributed). `GET /firmware/version` returns `{version, channel, length, sha256_prefix, ready}` for the laptop-side push script.

**Throughput envelope.** ESP-NOW sustains 300–800 chunks/sec in good RF; receiver SPI flash writes land 100–300 KB/s. End-to-end: **~15–30 s for a 1.6 MB push** with a single peer in the same room. Edge-of-range or competing radio traffic multiplies that 2–4×. Practical for the "occasional update" use case.

### 8. Rollback (Phase G)

**Automatic.** `esp_ota_mark_app_valid_cancel_rollback()` called in `standard_lamp.cpp setup()` after the lamp reaches steady state (10-second timer after boot — BLE up, strips drawing, mesh online). Crash before that line → watchdog reboot → ESP-IDF auto-reverts.

**Manual.** Encrypted BLE characteristic `CHAR_FIRMWARE_ROLLBACK` (write-only, advanced-gated app-side) calls `esp_ota_mark_app_invalid_rollback_and_reboot()`. New Setup row "Roll back firmware" alongside Factory Reset, same confirm-dialog pattern. Firmware checks `esp_ota_get_last_invalid_partition() == nullptr` AND the previous partition is bootable before calling the API; returns an error otherwise so the app can show "no previous version available."

Survives at the OTA layer regardless of who pushed the firmware — wisp OR USB.

## Wire format additions (summary)

Single new field in `HelloFixed`:
- `uint32_t firmwareVersion` (semver packed: `(major << 16) | (minor << 8) | patch`)

New message types in `lamp_protocol.hpp`:

| Type | Purpose |
|---|---|
| `MSG_WISP_HELLO (0x20)` | wisp → broadcast, status beacon every 2 s |
| `MSG_WISP_PAINT (0x21)` | wisp → lamp, transient base color override |
| `MSG_WISP_RELEASE (0x22)` | wisp → broadcast, "drop all overrides" |
| `MSG_WISP_OP (0x23)` | lamp → wisp, app proxy ops (e.g. `setPaintMode`, `forceScan`) |
| `MSG_FW_OFFER (0x30)` | wisp → lamp, "I have version X for you" |
| `MSG_FW_ACCEPT (0x31)` | lamp → wisp, "begin streaming" |
| `MSG_FW_CHUNK (0x32)` | wisp → lamp, ordered firmware bytes |
| `MSG_FW_REQ (0x33)` | lamp → wisp, retransmit request |
| `MSG_FW_DONE (0x34)` | wisp → lamp, "verify now" |
| `MSG_FW_RESULT (0x35)` | lamp → wisp, terminal status code |

## Tunables hard-coded in v1

- Wisp HELLO cadence: 2 s
- Per-lamp paint refresh backstop: 10 s
- Lamp wisp-watchdog: 60 s no paint + 30 s no hello → auto-release
- Tuple sampler: 2 colors per lamp, MAC-hash positions, 0.15 min separation, 3 retries
- Aurora rediscovery cadence: every 5 failed WS connects (per the reference)
- OTA: chunk size 200 bytes, idle scan 5 s, per-peer accept timeout 2 s, result timeout 30 s

## Open questions

- **Aurora instance id format.** Default to `"wisp-<chipid>"` so multiple wisps on the same network don't collide. Cosmetic; resolve at implementation.
- **Aurora authentication.** Reference doesn't auth; if Aurora ever requires it, add a token field to wisp's persisted config (set via a new `MSG_WISP_OP`). Out of scope until it's a real problem.
- **Repeat-palette skip.** What if the active palette changes back to one we already painted recently. v1: just repaint. v1.1: cache the last paint and skip if unchanged.
- **WiFi scan UX in the app.** v1 has the user type SSID + password manually. v1.1 could add a `scanWifi` op where wisp scans and returns a list; defer until manual entry proves annoying.
- **Old artnet-repeater directory.** Archive (`software/artnet-repeater.archive/`) or delete? Either works; user call at scaffolding time.
- **Lamp-side channel display.** Should the lamp itself show its `FIRMWARE_CHANNEL` somewhere user-visible (Info pane?) so the user knows which channel a given lamp is on? Probably yes; small follow-up. Out of scope for v1 but worth adding to TODO.md.

## Critical files to read before implementing

- `software/lamp-os/src/components/network/lamp_protocol.hpp` (lines 14-61) — packet types, `HelloFixed`
- `software/lamp-os/src/components/network/show_receiver.cpp` (lines 63-234) — CONTROL_OP framing + HELLO send/recv
- `software/lamp-os/src/components/network/espnow_link.{hpp,cpp}` — `peer.channel = 0` discipline
- `software/lamp-os/src/components/network/nearby_lamps.{hpp,cpp}` — peer registry pattern to mirror in wisp
- `software/artnet-repeater/src/main.cpp` + `platformio.ini` — current state of the board's bring-up code
- `software/lamp-app-flutter/lib/features/lamp_shell/presentation/setup_screen.dart` (lines 103-123) — advanced-gating + row layout
- `software/lamp-app-flutter/lib/features/social/presentation/social_screen.dart` (LampIcon usage ~line 153) — the existing lamp-icon widget to reuse
- `~/Downloads/esp32-palette-client/` (whole tree) — Phase B vendoring source

## Implementation plan

[`docs/superpowers/plans/2026-06-03-wisp.md`](../plans/2026-06-03-wisp.md) — phased delivery, file-by-file tasks, per-phase verification.
