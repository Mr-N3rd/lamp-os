# Mesh deployment notes

Operational + empirical reference for shipping the lamp fleet. Pairs with
[`mesh-api.md`](mesh-api.md) (the wire-format spec). Update this doc when
the next venue deployment teaches us something new.

## Production lock-in (v0x03)

The wire format is frozen at `PROTOCOL_VERSION = 0x03`. The following are
locked in and must not change without bumping the protocol version:

- Wire frame layouts for every msgType (HELLO, CONTROL_OP, WISP_HELLO,
  MSG_EVENT, OVERRIDE_*, RESTORE_*) — see `mesh-api.md`
- `DedupRing::CAPACITY = 64` per msgType
- `LAMP_HELLO_INTERVAL_MS = 5000`
- Gossip-relay rule per msgType:
  - **Relay**: `MSG_HELLO`, `MSG_CONTROL_OP`, `MSG_WISP_HELLO`, `MSG_EVENT`
  - **No relay**: `MSG_OVERRIDE_*`, `MSG_RESTORE_*` (these are unicast with
    driver-level 802.11 ACK + retry; gossip would just amplify airtime)
- Reserved bits: `kReservedMsgTypeHighBit`, `kStaggerCountReservedHighBit`
- `EventKind::ExpressionTriggered = 0x01`

Everything else is flexible: new `EventKind` values (`0x02..0xFF` open),
additive JSON fields in MSG_EVENT (old lamps ignore), per-lamp reaction
logic, the `pendingEvent` slot's internal shape.

**Phase D additions (no protocol bump):**

- Wisp now runs its own 64-slot `controlOpDedup_` ring keyed on
  `(sourceMac, msgType, seq)` so gossip-relayed copies of an op (or its
  own emitted `wispStatus`) don't re-apply. Previously the wisp dropped
  duplicates implicitly because it only received `MSG_HELLO`.
- Wisp emits `MSG_CONTROL_OP` broadcasts carrying `{"char":"wispStatus",...}`
  on-change + every ≤30 s as a heartbeat. Lamps gossip-relay these per
  the v0x03 `MSG_CONTROL_OP` rule and cache the latest per wisp MAC for
  the app to read via `CHAR_WISP_STATUS`. See `mesh-api.md` for the
  payload shape, cadence triggers, and the 230-byte payload cap.

**Mixed-fleet warning**: `inspect()` rejects on version mismatch
(`lamp_protocol.hpp::inspect`), so an old-firmware lamp in a v0x03 mesh
stops receiving everyone's frames. All lamps + wisp must be flashed to
v0x03 before a venue deployment. This is loud and diagnosable — peers
just stop showing up — not silent.

## BLE coex impact (the big one)

ESP32-WROOM lamps run BLE + ESP-NOW on the same 2.4 GHz radio. The IDF
SW-coex implementation (IDF #14904) starves ESP-NOW recv during active
BLE traffic. We can't fix this in firmware:

- Switching to HW coex requires rebuilding `libcoexist.a`. pioarduino
  ships a precompiled `libcoexist.a` with SW coex baked in, so the
  `-D CONFIG_ESP_COEX_SW_COEXIST_ENABLE=0` build flag in
  `software/lamp-os/platformio.ini` is **a documentation contract, not
  a behavior change**. Left in place so a future move to ESP-IDF picks
  up the intent automatically.
- The mitigation strategy is therefore "minimise BLE radio time when not
  actively serving the user," not "make coex not starve us."

### Empirical recv rates (measured 2026-06-04, jacko + meloni, 2-lamp test)

Per-link cascade recv rate measured by spamming `glitchy` triggers via
the iOS app's expression-test characteristic on jacko and counting
`[meloni] [loop] drain event` lines:

| Originator BLE state         | Real-time recv rate | Notes                                    |
|------------------------------|---------------------|------------------------------------------|
| Wisp (no BLE)                | ~100%               | Reference floor — no coex contention      |
| Lamp, no GATT client         | ~60-100%            | Adv + central scan only, low duty cycle  |
| Lamp, GATT client + WIDE     | ~30-60%             | Idle conn-params (100-200 ms)            |
| Lamp, GATT client + TIGHT    | ~5-30%              | Active conn-params (15-30 ms)            |
| Pre-v0x03 baseline (no gossip)| ~22%                | TIGHT BLE, no gossip-relay               |

Recv rate is per direct link. Mesh coverage scales with the gossip
redundancy across all N lamps (next section).

### Gossip-relay math at fleet scale

A receiver doesn't have to hear the originator directly — it just has to
hear *any* of the N-1 other lamps' gossip-relay of the same frame. With
per-link recv probability `p` and N lamps in radio range:

```
fleet_coverage ≈ 1 - (1 - p)^N
```

At `p = 0.3` (TIGHT BLE on the originator, worst case):

| N  | Coverage |
|----|----------|
|  2 | 30%      |
|  5 | 83%      |
| 10 | 97%      |
| 22 | 99.9%    |
| 50 | 99.9997% |

The 2-lamp bench test is the **hardest** case. Adding lamps strictly
improves cascade reach. This is why a 22-lamp venue deployment is
expected to work even though the bench number looks scary.

### Adaptive TIGHT/WIDE BLE conn-params

To keep BLE responsive while leaving airtime for ESP-NOW when the user
isn't actively interacting, lamps adapt their GATT connection interval:

- **TIGHT = 15-30 ms** — ~30-60 BLE check-ins/sec. Used during active app
  writes (slider drag, color picker, expression test). Snappy UX, heavy
  coex penalty.
- **WIDE = 100-200 ms** — ~5-10 BLE check-ins/sec. Used after 2 s of no
  app writes. Frees ~5× more airtime for ESP-NOW broadcasts to land.

Transitions happen automatically via `ble_control::tick()`. The state
machine is driven by `s_lastBleWriteMs`, stamped by
`ble_control::markActivity()` at the top of every onWrite handler
(including `WriteRouter::onWrite`, via a forward-decl in
`write_router.hpp`). Pure GATT reads don't count as activity — they
don't generate the continuous coex pressure we're trying to dodge.

Log markers in firmware (LAMP_DEBUG only):

```
[ble_control] conn params → WIDE (100-200 ms) idle=2001 ms
[ble_control] conn params → TIGHT (15-30 ms) — activity
```

iOS as central typically honours the requested window. Android may
decline TIGHT requests (favours power saving); if it does, the link
keeps working at whatever interval the central chose.

## Deployment checklist

Before flashing a venue fleet:

1. **All hardware on v0x03.** Confirm every lamp + every wisp ESP32-C6
   gets the current firmware. `inspect()`'s version reject is loud
   (peers don't show up) so you'll know within seconds if one was
   missed, but easier to be thorough.
2. **Verify each lamp's mesh after boot.** Connect via app, check the
   "nearby lamps" list. Entries should carry both `viaBle: true` and
   `viaEspNow: true` flags. If only `viaBle: true`, ESP-NOW HELLO isn't
   reaching that lamp from this one — investigate (channel mismatch,
   radio environment, distance > ~30 m LoS).
3. **Spot-check cascade reach.** Trigger an expression from the app on
   one lamp; visually confirm at least one other lamp fires the
   cascade. (You don't need fleet-wide synchronisation for the gossip
   math to work — see table above.)
4. **Don't flash and immediately leave.** Stick around 60 s after flash
   so any new BLE-active recv issues surface in `dual_tap.log`.

## ArtNet bridge (pre-mesh lamp backwards compat)

The wisp can broadcast ArtNet DMX universe 1 in parallel with the v0x03
mesh, so pre-mesh lamps still running the `software/lamp-os` ArtNet
firmware (the snapshot on `main` that still ships `artnet.cpp` +
`BLE_STAGE_MAGIC` scanner) can be painted by the same Aurora palette.

**Wire-up at the venue:**

1. Set the wisp's WiFi creds via serial: `wifi:set <ssid> <pass>`. These
   persist in NVS, so reboot picks them back up.
2. `stage:on` to start advertising the BLE manufacturer-data beacon
   (magic ID `42007`, payload `<ssid>\0<password>\0`). Pre-mesh lamps
   scan for this and auto-join the same WiFi — no per-lamp provisioning.
3. `artnet:on` to start broadcasting ArtNet to `255.255.255.255:6454`.
   One frame per Aurora palette change plus a 1 s backstop.

**Coex caveat:** ESP-NOW remains pinned to channel 11 at boot, but
associating to the venue AP re-tunes the radio to whatever channel the
AP advertises. Mesh reliability degrades the further the AP sits from
channel 11. This is the same constraint Aurora already imposes; the
ArtNet path piggybacks the existing trade-off, it does not introduce a
new one. (Moved 1 → 11 on 2026-06-10 because channel 1 is the consumer-
router default and overwhelmingly the most congested 2.4 GHz channel.)

**Wire format produced** — see `software/wisp/src/artnet_frame.h`:
universe 1, 8 fixtures × 10 channels (shade RGBW, base RGBW, mode byte,
parameter byte). Pre-mesh lamps self-assign their fixture index
randomly at boot (`software/lamp-os/src/components/network/artnet.cpp`
on `main`, around the `lampNumber = random(0, 7)` line); we broadcast
all 8 slots and let them draw straws.

**No effect on v0x03 mesh lamps.** Current lamp firmware on
`flutter-rewrite` has neither the ArtNet listener nor the stage-beacon
scanner compiled in — mesh lamps will ignore both the BLE advert and
the UDP broadcast.

## Troubleshooting

### "I see lamps in the app's nearby list but cascades don't fire"

Check the `viaBle` / `viaEspNow` flags on the nearby-lamps payload
(`buildNearbyLampsJson` in `ble_control.cpp`). If lamps are
`viaBle: true, viaEspNow: false`, ESP-NOW HELLO isn't propagating but
BLE central scan still sees them. Common causes:

- **Channel drift.** A WiFi scan that hit `WIFI_SCAN_FAILED` could
  strand the radio off `LAMP_ESPNOW_CHANNEL=11`. Logged as `[wifi.sched]`
  lines. Fixed by the boot-scan delay + home-mode-gated scan in
  `wifi::tick`, but still worth checking if it bites.
- **BLE coex starvation.** Active GATT session with TIGHT conn-params.
  Disconnect the app or wait for WIDE to kick in (2 s of write
  silence). See section above.
- **Out of radio range.** ESP-NOW broadcast reach is ~30-50 m LoS,
  shorter through walls.

### "Cascade reception during BLE-active is below 30%"

Expected, but check:

- App is actively streaming writes (brightness slider, color picker
  drag)? Yes → originator lamp is in TIGHT. Numbers will recover when
  you stop touching the screen.
- App is just sitting connected idle? It should be in WIDE after 2 s.
  Look for `[ble_control] conn params → WIDE` log line. If missing,
  the WIDE transition isn't firing — debug `ble_control::tick`.

### "Wisp paint isn't reaching lamps"

`PaintDistributor` (`software/wisp/src/PaintDistributor.cpp`) sends
`MSG_OVERRIDE_COLORS` as **unicast** per lamp (not broadcast), so the
gossip-relay math above doesn't apply. The wisp needs an entry in its
`LampInventory` for each target — populated by gossip-relayed
`MSG_HELLO`. If wisp doesn't see a lamp, walk through the same channel
+ coex checks above.

## Out of scope (tracked, not blocking)

Items deferred from the v0x03 lock-in:

- **`pendingEvent` single-slot newest-wins.** A burst of MSG_EVENTs at
  receiver-faster-than-loop-drain rate loses everything but the latest.
  Instrumented via `[slot.overwrite] size=N` in `pending_typed_slot.hpp`.
  Address if it bites at 20-50 lamp scale.
- **`recentCascades_` 250 ms window** keyed on `(type, intervalIdx=0)`.
  Could drop legitimate concurrent same-type cascades from different
  sources within 250 ms. Tracked.
- **Hung-scan watchdog** in `wifi.cpp`. Earlier audit's medium-severity
  item; current mitigations (boot delay + home-mode gate) cover most
  cases.
- **Aurora subscription-fail terminal state** in wisp. Wisp-only
  concern, recovery is reboot today.
- **Wisp paint multi-room reach.** Wisp paint unicast won't naturally
  reach lamps the wisp can't directly hear via ESP-NOW. If venues need
  multi-room palette painting, options are (a) gossip-relay
  `OVERRIDE_*` messages (wire-format change → next protocol bump),
  (b) wisp broadcasts paint instead of per-lamp unicast (wisp-side
  change, simpler), or (c) deploy a wisp per room.

## What to measure next deployment

If you take new measurements at a venue, capture and append a row to
the recv-rate table above with the date + lamp count + BLE state, so
this doc keeps growing with empirical reality.
