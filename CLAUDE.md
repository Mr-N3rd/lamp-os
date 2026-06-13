# Lamp OS

ESP32-based smart-lamp fleet. Three components:

- **`software/lamp-os/`** — lamp firmware (ESP32-WROOM). BLE GATT control
  service for the Flutter app + ESP-NOW mesh for lamp-to-lamp. Build env
  `upesy_wroom`.
- **`software/wisp/`** — wisp infrastructure node (Seeed Xiao ESP32-C6,
  external antenna). Aurora palette subscriber + mesh paint distributor +
  status beacon. Build env `seeed_xiao_esp32_c6`.
- **`software/lamp-app-flutter/`** — iOS/Android control app (Flutter).
  Talks BLE GATT to lamps; no direct mesh participation.

## Authoritative references

Read these before changing networking, protocol, or BLE behavior:

- **[`docs/mesh-api.md`](docs/mesh-api.md)** — wire-format spec for every
  message type. The code wins ties; update this doc when it doesn't.
- **[`docs/mesh-deployment.md`](docs/mesh-deployment.md)** — operational
  reference: the v0x03 production lock-in, the BLE coex story, empirical
  per-link recv rates, the gossip-relay-at-scale math, deployment
  checklist, and the troubleshooting playbook. Captures hard-won findings
  from real hardware testing; read it before chasing a "mesh feels
  unreliable" report.

## Lock-ins (don't change without a protocol version bump)

`PROTOCOL_VERSION = 0x03` is the production wire format. The frozen list
lives in `docs/mesh-deployment.md` under "Production lock-in". Mixed-fleet
across protocol versions does not interoperate (loud, diagnosable
failure: peers don't show up).

Note: the firmware-side `ble_svc_gatt_changed()` Service Changed indication
+ bonded SMP path shipped in commit `49e19c9` was fully reverted in
`a5fa8cb`. The current lamp firmware does NOT fire Service Changed on
boot — Android relies on its own per-session cache invalidation. App-side
`clearGattCache()` was also removed (hidden-API blocklisted on Android
14+). Re-flashing a lamp with a changed GATT layout will surface as
"silent wrong-handle reads" until the user force-stops + reopens the app.
Acceptable trade-off in dev workflow; needs a fix before OTA cycles.

## Build + test

```sh
# Native unit tests (lamp side, runs in CI)
cd software/lamp-os && pio test -e native

# Build lamp firmware
cd software/lamp-os && pio run -e upesy_wroom

# Build wisp firmware
cd software/wisp && pio run -e seeed_xiao_esp32_c6
```

The native suite covers protocol parsers, dedup ring, color math, fade
math, cascade dedup. Keep it green — currently 144/144.

## Tail two lamps simultaneously

A small Python helper at `/tmp/dual_tap.py` (local-only, not in repo)
opens both lamp serial ports and prefixes each line with the lamp name.
Output goes to `/tmp/dual_tap.log`. Useful for diff-checking cascade
sender vs receiver behavior across two physical lamps during firmware
iteration.
