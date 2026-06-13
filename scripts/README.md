# Lamp-OS firmware OTA tooling

Two host scripts integrate with PlatformIO to produce signed firmware
for the gossip-OTA mesh:

| Script | Hook | Role |
|---|---|---|
| `gen_firmware_keys.py` | manual, one-shot | Generate the ed25519 keypair that signs all OTA firmware. |
| `sign_firmware.py` | PIO `post:` on lamp + wisp builds | Append the LSIG footer + ed25519 signature to `firmware.bin`. |

The gossip-OTA model: the Flutter app downloads a signed lamp binary
from GitHub Releases, verifies the LSIG footer locally, then pushes it
to a paired lamp over BLE. The lamp accepts the OTA, reboots into the
new version, and then propagates that exact same image to peers it
meets over ESP-NOW mesh — every running lamp is its own
"distributor", sourcing the bytes directly out of its running OTA
partition. No wisp involvement, no embedded blob, no separate
distribution channel.

## One-time setup (per workstation)

```sh
python3 scripts/gen_firmware_keys.py
```

This generates a fresh ed25519 keypair:

- Private key → `~/.lamp-os-firmware-key.bin` (32 bytes, mode 0600).
  **NEVER commit.** Lives outside the repo on purpose.
- Public key → `software/lamp-os/scripts/keys/firmware_pubkey.h` (32 bytes).
  IS committed. Every lamp firmware build bakes this in; every lamp in
  the fleet verifies incoming OTA against this exact public key.

The script refuses to overwrite an existing private key unless `--force`
is passed.

### Losing or rotating the private key

If you lose `~/.lamp-os-firmware-key.bin`, the existing fleet is
PERMANENTLY locked out of OTA — there's no recovery path over the air
because every lamp will reject any signature not matching its baked-in
`kFirmwarePubkey`. The ONLY way back is:

1. Generate a new keypair (`gen_firmware_keys.py --force`).
2. USB re-flash EVERY lamp with a firmware build containing the new
   `firmware_pubkey.h`.
3. Future OTAs use the new key.

Same path applies to deliberate rotation (e.g. dev key → production key).
Plan accordingly.

## Building signed firmware

```sh
cd software/lamp-os && pio run -e upesy_wroom
  # post-build sign_firmware.py appends LSIG footer to firmware.bin,
  # writes firmware-signed.bin alongside it. This is what the app pushes
  # over BLE to seed gossip-OTA across the fleet.

cd software/wisp && pio run -e seeed_xiao_esp32_c6
  # post-build sign_firmware.py runs the same way. The signed wisp
  # binary has no consumer in v1 (wisps don't gossip-OTA), but the hook
  # stays so a future wisp self-OTA path doesn't need re-plumbing.
```

## Channels

The LSIG footer carries an 8-byte ASCII channel field. Default is
`"stable"`. To build for the `beta` channel:

```sh
LAMP_FIRMWARE_CHANNEL=beta pio run -e upesy_wroom
```

A `beta` lamp's OFFER carries `channel="beta"`; a `stable` peer silently
drops it. Cross-channel migration only via USB re-flash.

## Files in detail

### `gen_firmware_keys.py`

- Prefers PyNaCl (libsodium bindings); falls back to `cryptography`.
- Round-trip sign+verify self-test before writing files; aborts on
  failure rather than emitting unusable keys.
- Header rewrite is round-trip safe: only the 32 byte values inside the
  `kFirmwarePubkey[32]` literal change; `#pragma once`, namespaces, and
  closing braces are preserved.

### `sign_firmware.py`

- PIO post-build hook for `[env:upesy_wroom]` in
  `software/lamp-os/platformio.ini` and `[env:seeed_xiao_esp32_c6]` in
  `software/wisp/platformio.ini`.
- Reads `firmware.bin` from PIO's build dir, appends 96-byte LSIG footer
  (magic + channel + version + signedRegionLen + reserved + ed25519
  signature), writes `firmware-signed.bin` alongside.
- Version source: parses `software/lamp-os/include/firmware_version.h`
  (`FIRMWARE_VERSION_MAJOR/MINOR/PATCH` defines). Falls back to parsing
  `src/version.hpp`'s `FIRMWARE_VERSION = 0xXXXXXX` literal if the
  defines file is absent.
- Idempotent: if `firmware-signed.bin` is newer than `firmware.bin` AND
  newer than the private key, skips re-signing.
