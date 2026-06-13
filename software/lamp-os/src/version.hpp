#pragma once

#include <cstdint>

// Firmware identity advertised over the mesh (HELLO) and embedded into the
// signed firmware footer in Phase E. Bump on every release that should be
// distinguishable to wisp's OTA picker.
//
// Layout: (major << 16) | (minor << 8) | patch. Stored as uint32_t for cheap
// `<` comparisons in the distributor's "lowest version peer" pick.
namespace lamp {

constexpr uint32_t FIRMWARE_VERSION = 0x010051;  // 1.0.81 (wisp carrier — OTA target)

// Channel is purely a placeholder string in Phase A. In Phase E it comes from
// a -D flag baked at build time (`-D FIRMWARE_CHANNEL='"beta"'`) and gates
// cross-channel OTA offers (a stable lamp ignores a beta offer and vice
// versa). Until then, every build is on stable.
#ifndef FIRMWARE_CHANNEL
#define FIRMWARE_CHANNEL "stable"
#endif

constexpr const char* FIRMWARE_CHANNEL_STR = FIRMWARE_CHANNEL;

// Build timestamp lets us tell two builds of the same SemVer apart on the
// serial console without a clean re-flash. Cheap; ~20 bytes of .rodata.
constexpr const char* FIRMWARE_BUILD_STAMP = __DATE__ "@" __TIME__;

}  // namespace lamp
