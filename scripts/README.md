# Lamp-OS firmware OTA tooling

Three host scripts integrate with PlatformIO to produce signed firmware
that wisp can carry and push to lamps over the ESP-NOW mesh:

| Script | Hook | Role |
|---|---|---|
| `gen_firmware_keys.py` | manual, one-shot | Generate the ed25519 keypair that signs all OTA firmware. |
| `sign_firmware.py` | PIO `post:` on lamp build | Append the LSIG footer + ed25519 signature to `firmware.bin`. |
| `embed_firmware.py` | PIO `pre:` on wisp build | Convert the signed lamp binary into a C++ byte array the wisp carries. |

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

## Building signed firmware for OTA push

The lamp + wisp builds are coupled. Run in this order:

```sh
cd software/lamp-os && pio run -e upesy_wroom
  # post-build sign_firmware.py appends LSIG footer to firmware.bin,
  # writes firmware-signed.bin alongside it.

cd software/wisp && pio run -e seeed_xiao_esp32_c6
  # pre-build embed_firmware.py reads the lamp's firmware-signed.bin,
  # writes software/wisp/include/embedded_firmware_generated.h.
  # FirmwareCarrier reads this at boot and offers it over the mesh.
```

If the lamp build is missing when the wisp builds, the wisp pre-build
fails with a clear error. Set `WISP_ALLOW_STUB_FIRMWARE=1` to build the
wisp with a 200-byte 0xFF placeholder (FirmwareCarrier rejects it at
runtime, distributor stays Disabled, wisp continues palette work).

## Channels

The LSIG footer carries an 8-byte ASCII channel field. Default is
`"stable"`. To build for the `beta` channel:

```sh
LAMP_FIRMWARE_CHANNEL=beta pio run -e upesy_wroom
cd ../wisp && pio run -e seeed_xiao_esp32_c6
```

A `beta` wisp's OFFER carries `channel="beta"`; a `stable` lamp silently
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
  `software/lamp-os/platformio.ini`.
- Reads `firmware.bin` from PIO's build dir, appends 96-byte LSIG footer
  (magic + channel + version + signedRegionLen + reserved + ed25519
  signature), writes `firmware-signed.bin` alongside.
- Version source: parses `software/lamp-os/include/firmware_version.h`
  (`FIRMWARE_VERSION_MAJOR/MINOR/PATCH` defines). Falls back to parsing
  `src/version.hpp`'s `FIRMWARE_VERSION = 0xXXXXXX` literal if the
  defines file is absent.
- Idempotent: if `firmware-signed.bin` is newer than `firmware.bin` AND
  newer than the private key, skips re-signing.

### `embed_firmware.py`

- PIO pre-build hook for `[env:seeed_xiao_esp32_c6]` in
  `software/wisp/platformio.ini`.
- Reads
  `software/lamp-os/.pio/build/upesy_wroom/firmware-signed.bin`, writes
  `software/wisp/include/embedded_firmware_generated.h` containing the
  bytes as a 16-bytes-per-line hex array.
- Content-based staleness check (sha256 over the blob); skips
  regeneration when the embed is current.
- Honors `WISP_ALLOW_STUB_FIRMWARE=1` to emit a 200-byte 0xFF stub when
  the lamp build is missing.
