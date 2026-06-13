#pragma once

#include <cstdint>

// Firmware identity for the wisp (BLE-pushed OTA target). Same shape as the
// lamp's `lamp::FIRMWARE_VERSION` for consistency with `sign_firmware.py`'s
// version parser (looks for `FIRMWARE_VERSION = 0xMMmmpp` literal).
//
// Layout: (major << 16) | (minor << 8) | patch. Bump on every release that
// should be distinguishable to the app's "push if newer" check.
namespace wisp {

constexpr uint32_t FIRMWARE_VERSION = 0x000100;  // 0.1.0 (BLE-OTA bring-up)

// Channel placeholder. Real `FIRMWARE_CHANNEL` comes from a -D flag baked at
// build time; until that's wired, every wisp build is on stable.
#ifndef FIRMWARE_CHANNEL
#define FIRMWARE_CHANNEL "stable"
#endif

constexpr const char* FIRMWARE_CHANNEL_STR = FIRMWARE_CHANNEL;

constexpr const char* FIRMWARE_BUILD_STAMP = __DATE__ "@" __TIME__;

}  // namespace wisp
