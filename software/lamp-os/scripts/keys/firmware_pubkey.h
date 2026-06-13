#pragma once

// Ed25519 public key for verifying signed firmware images during OTA.
//
// REPLACED at sign time by gen_firmware_keys.py — do not commit a real key
// here. This stub is all-zeros so the firmware builds cleanly during
// development; ed25519 verify will always fail against a real signed image
// while this stub is in place, which is the safe default (no OTA can land).
//
// The private key is generated and held by the developer running
// gen_firmware_keys.py; the public key value is regenerated and pasted
// into this file by that script (it overwrites the array bytes only).

#include <cstdint>

namespace lamp { namespace firmware {

// 32-byte ed25519 public key, raw form (matches libsodium
// crypto_sign_ed25519_verify_detached's `pk` argument).
constexpr uint8_t kFirmwarePubkey[32] = {
    0x0b, 0x58, 0x84, 0x6d, 0x91, 0xf9, 0x44, 0x7c,
    0x90, 0xcd, 0xa2, 0x26, 0xa4, 0x91, 0xff, 0x33,
    0xd5, 0x5c, 0x0d, 0x14, 0xdb, 0xa9, 0xfe, 0xa4,
    0x89, 0xb0, 0x6e, 0x6c, 0x0d, 0xa6, 0x48, 0xec,
};

}}  // namespace lamp::firmware
