#pragma once

// Wire format for lamp <-> lamp ESP-NOW broadcast (HELLO + CONTROL_OP +
// Phase C wisp/transient/event additions).

#include <cstdint>
#include <cstring>

// portMUX is FreeRTOS-only. The header is also indirectly mirrored in
// native unit tests — guard the include so a hypothetical native compile
// of THIS header doesn't break, and provide a no-op fallback.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <portmacro.h>
#define LAMP_PROTOCOL_PORTMUX_TYPE        portMUX_TYPE
#define LAMP_PROTOCOL_PORTMUX_INIT        portMUX_INITIALIZER_UNLOCKED
#define LAMP_PROTOCOL_PORTMUX_ENTER(mux)  portENTER_CRITICAL(mux)
#define LAMP_PROTOCOL_PORTMUX_EXIT(mux)   portEXIT_CRITICAL(mux)
#else
struct LampProtocolNullMux {};
#define LAMP_PROTOCOL_PORTMUX_TYPE        LampProtocolNullMux
#define LAMP_PROTOCOL_PORTMUX_INIT        {}
#define LAMP_PROTOCOL_PORTMUX_ENTER(mux)  ((void)(mux))
#define LAMP_PROTOCOL_PORTMUX_EXIT(mux)   ((void)(mux))
#endif

namespace lamp_protocol {

constexpr uint8_t MAGIC_0 = 'L';
constexpr uint8_t MAGIC_1 = 'M';
// Phase A bumped this from 0x01 to 0x02 to add `firmwareVersion` to the HELLO
// fixed prefix. The bump is a one-way break: a v0x01 lamp on the mesh will
// drop v0x02 frames (and vice versa), so all peers must be re-flashed before
// they re-discover each other. Spec'd as acceptable in
// docs/superpowers/specs/2026-06-03-wisp-design.md.
constexpr uint8_t PROTOCOL_VERSION = 0x02;

enum MsgType : uint8_t {
  MSG_HELLO               = 0x01,
  // Forwarded BLE control write. Payload is JSON tagged with a `char` field
  // naming the local control surface to invoke (brightness, shadeColors,
  // baseColors, knockout, expressionOp, settings, ...). The local
  // pending-slot post functions handle the routing; downstream drain in
  // loop() runs unchanged.
  MSG_CONTROL_OP          = 0x03,
  // --- Phase C additions (wisp + transient overrides + events) ---
  MSG_WISP_HELLO          = 0x20,  // wisp presence beacon
  MSG_OVERRIDE_COLORS     = 0x21,
  MSG_RESTORE_COLORS      = 0x22,
  MSG_OVERRIDE_BRIGHTNESS = 0x23,
  MSG_RESTORE_BRIGHTNESS  = 0x24,
  MSG_EVENT               = 0x30,
};

// Phase C: explicit reserve of the high bit on msgType. Previously
// FLAG_LOCAL_ONLY rode there for the cascade-locality hack; C.3 retires
// that path. Keep the bit reserved so future protocol changes don't
// silently corrupt parsing.
constexpr uint8_t kReservedMsgTypeHighBit = 0x80;

// High bit of the msgType byte. When set, receivers apply the frame locally
// but skip the rebroadcast relay — cascade stays within direct radio range
// instead of fanning across the grid. Set by the expression-cascade path;
// other senders leave it off and the existing relay extends reach as before.
// inspect() masks this bit out so existing msgType comparisons keep working.
constexpr uint8_t FLAG_LOCAL_ONLY = 0x80;
constexpr uint8_t MSG_TYPE_MASK   = 0x7F;

// Phase C single-source-of-truth caps.
constexpr size_t kMaxOverrideColorsPerFrame = 8;   // ESP-NOW 250-byte cap math
constexpr size_t kMaxStaggerEntries         = 12;  // ESP-NOW 250-byte cap math
constexpr uint8_t kBrightnessOverrideMin    = 5;   // anti-defeat floor

// Surface byte values used by the override/restore family. `Any` means the
// override targets every surface on the lamp (base + shade).
enum class OverrideSurface : uint8_t {
  Base  = 0x01,
  Shade = 0x02,
  Any   = 0xFF,
};

// Discriminator for who originated an override. PeerSwap is reserved for
// the upcoming social-cascade swap-source so receivers can dedup against
// matching restores. 0x10..0xFE is user-defined for forward compat.
enum class OverrideSource : uint8_t {
  None     = 0x00,
  Wisp     = 0x01,
  PeerSwap = 0x02,
  Any      = 0xFF,
};

// MSG_EVENT discriminator. 0x02..0x0F are reserved for built-ins so the
// firmware can add new well-known events without colliding with
// user-defined ones in the 0x10..0xFF range.
enum class EventKind : uint8_t {
  ExpressionTriggered = 0x01,
};

constexpr size_t HEADER_SIZE = 6;
// HELLO fixed prefix: header(6) + sourceMac(6) + shade(4) + base(4) + firmwareVersion(4).
// Name length byte + name bytes follow this prefix.
constexpr size_t HELLO_FIXED_SIZE = 23;
constexpr size_t HELLO_MAX_NAME = 32;
constexpr size_t HELLO_MAX_SIZE = HELLO_FIXED_SIZE + 1 + HELLO_MAX_NAME;  // +1 for name length byte

// MSG_CONTROL_OP frame: header(6) + targetMac(6) + sourceMac(6) + payloadLen(2) + payload(N).
// ESP-NOW max frame is 250 bytes; subtract the 20-byte fixed prefix.
constexpr size_t CONTROL_FIXED       = HEADER_SIZE + 6 + 6 + 2;
constexpr size_t CONTROL_MAX_PAYLOAD = 250 - CONTROL_FIXED;  // 230
constexpr size_t CONTROL_MAX_SIZE    = CONTROL_FIXED + CONTROL_MAX_PAYLOAD;

// --- Phase C wire-format sizes ---
// MSG_WISP_HELLO: header(6) + sourceMac(6) + wispVersion(4) + flags(1)
//                 + paletteIdPrefix(8) + carriedFwChannel(8) + carriedFwVersion(4)
//               = 37 bytes fixed.
constexpr size_t WISP_HELLO_PALETTE_ID_PREFIX_LEN = 8;
constexpr size_t WISP_HELLO_FW_CHANNEL_LEN        = 8;
constexpr size_t WISP_HELLO_FIXED_SIZE            = HEADER_SIZE + 6 + 4 + 1 +
                                                    WISP_HELLO_PALETTE_ID_PREFIX_LEN +
                                                    WISP_HELLO_FW_CHANNEL_LEN + 4;  // 37
constexpr uint8_t WISP_HELLO_FLAG_PAINT_MODE        = 0x01;
constexpr uint8_t WISP_HELLO_FLAG_WIFI_CONNECTED    = 0x02;
constexpr uint8_t WISP_HELLO_FLAG_AURORA_CONNECTED  = 0x04;

// MSG_OVERRIDE_COLORS fixed prefix:
//   header(6) + sourceMac(6) + targetMac(6) + surface(1) + sourceKind(1)
//   + fadeDurationMs(2) + numColors(1)
// = 23 bytes; colors[numColors * 4] follow. Min numColors=1 → 27 total.
constexpr size_t OVERRIDE_COLORS_FIXED_SIZE = HEADER_SIZE + 6 + 6 + 1 + 1 + 2 + 1;  // 23
constexpr size_t OVERRIDE_COLORS_MAX_SIZE   = OVERRIDE_COLORS_FIXED_SIZE +
                                              kMaxOverrideColorsPerFrame * 4;       // 55

// MSG_RESTORE_COLORS / MSG_RESTORE_BRIGHTNESS:
//   header(6) + sourceMac(6) + targetMac(6) + surface(1) + sourceKind(1)
//   + fadeDurationMs(2)
// = 22 bytes fixed (no payload tail).
constexpr size_t RESTORE_FIXED_SIZE = HEADER_SIZE + 6 + 6 + 1 + 1 + 2;  // 22

// MSG_OVERRIDE_BRIGHTNESS:
//   header(6) + sourceMac(6) + targetMac(6) + surface(1) + sourceKind(1)
//   + fadeDurationMs(2) + brightness(1)
// = 23 bytes fixed.
constexpr size_t OVERRIDE_BRIGHTNESS_FIXED_SIZE = HEADER_SIZE + 6 + 6 + 1 + 1 + 2 + 1;  // 23

// MSG_EVENT fixed prefix when numStaggerEntries=0 and payloadLen=0:
//   header(6) + sourceMac(6) + eventKind(1) + numStaggerEntries(1)
//   + payloadLen(2)
// = 16 bytes minimum. (Spec'd as 17 in the plan; the math here is the
// authoritative one — header is 6, not 7.)
constexpr size_t EVENT_FIXED_SIZE      = HEADER_SIZE + 6 + 1 + 1 + 2;  // 16
constexpr size_t EVENT_STAGGER_ENTRY   = 6 + 2;                        // mac + delayMs
// Cap the JSON tail so a full-stagger event still fits the 250 ESP-NOW frame.
// 250 - 16 - 12*8 = 138.
constexpr size_t EVENT_MAX_PAYLOAD     = 250 - EVENT_FIXED_SIZE -
                                         (kMaxStaggerEntries * EVENT_STAGGER_ENTRY);
constexpr size_t EVENT_MAX_SIZE        = EVENT_FIXED_SIZE +
                                         kMaxStaggerEntries * EVENT_STAGGER_ENTRY +
                                         EVENT_MAX_PAYLOAD;  // 250

// MAX_PACKET_SIZE: receiver buffer sizing. CONTROL_MAX_SIZE has historically
// been the biggest (250); EVENT_MAX_SIZE is also 250; the override family
// is well under. Use a max() over the candidates so a future shrink of any
// one doesn't silently shrink the buffer.
constexpr size_t MAX_PACKET_SIZE =
    (CONTROL_MAX_SIZE > HELLO_MAX_SIZE ? CONTROL_MAX_SIZE : HELLO_MAX_SIZE) >
            EVENT_MAX_SIZE
        ? (CONTROL_MAX_SIZE > HELLO_MAX_SIZE ? CONTROL_MAX_SIZE : HELLO_MAX_SIZE)
        : EVENT_MAX_SIZE;

struct ParsedHello {
  uint16_t seq;
  uint8_t sourceMac[6];
  uint8_t shade[4];
  uint8_t base[4];
  // Semver packed (major<<16)|(minor<<8)|patch. Zero on pre-Phase-A peers
  // (which never reach parseHello() anyway because their PROTOCOL_VERSION
  // mismatches and inspect() rejects them).
  uint32_t firmwareVersion;
  uint8_t nameLen;
  char name[HELLO_MAX_NAME + 1];  // null-terminated copy
};

struct ParsedControlOp {
  uint16_t seq;
  uint8_t targetMac[6];
  uint8_t sourceMac[6];
  uint16_t payloadLen;
  const uint8_t* payload;  // points into the recv buffer; caller must not retain past this call
  bool localOnly;          // FLAG_LOCAL_ONLY was set on the wire
};

// --- Phase C parsed structs ---

struct ParsedWispHello {
  uint16_t seq;
  uint8_t  sourceMac[6];
  uint32_t wispVersion;          // packed semver, same convention as ParsedHello
  uint8_t  flags;                // WISP_HELLO_FLAG_* bitfield
  // utf-8 bytes. Not necessarily null-terminated in-buffer; copied into a
  // larger-by-one storage so the parser can write a trailing '\0' for
  // easy logging. Trailing nulls inside the on-wire 8-byte slot are
  // preserved as-is (caller may treat as opaque ID prefix).
  char     paletteIdPrefix[WISP_HELLO_PALETTE_ID_PREFIX_LEN + 1];
  char     carriedFwChannel[WISP_HELLO_FW_CHANNEL_LEN + 1];
  uint32_t carriedFwVersion;
};

struct ParsedOverrideColors {
  uint16_t        seq;
  uint8_t         sourceMac[6];
  uint8_t         targetMac[6];
  OverrideSurface surface;
  OverrideSource  sourceKind;
  uint16_t        fadeDurationMs;
  uint8_t         numColors;
  uint8_t         colors[kMaxOverrideColorsPerFrame][4];  // RGBW per entry
};

struct ParsedRestoreColors {
  uint16_t        seq;
  uint8_t         sourceMac[6];
  uint8_t         targetMac[6];
  OverrideSurface surface;
  OverrideSource  sourceKind;
  uint16_t        fadeDurationMs;
};

struct ParsedOverrideBrightness {
  uint16_t        seq;
  uint8_t         sourceMac[6];
  uint8_t         targetMac[6];
  OverrideSurface surface;
  OverrideSource  sourceKind;
  uint16_t        fadeDurationMs;
  uint8_t         brightness;   // 0..100, validated against kBrightnessOverrideMin..100
};

struct ParsedRestoreBrightness {
  uint16_t        seq;
  uint8_t         sourceMac[6];
  uint8_t         targetMac[6];
  OverrideSurface surface;
  OverrideSource  sourceKind;
  uint16_t        fadeDurationMs;
};

struct ParsedEvent {
  uint16_t       seq;
  uint8_t        sourceMac[6];
  EventKind      eventKind;            // typed for built-ins
  uint8_t        eventKindRaw;         // raw byte for forward-compat / user-defined
  uint8_t        numStaggerEntries;
  struct StaggerEntry {
    uint8_t  mac[6];
    uint16_t delayMs;
  } staggerEntries[kMaxStaggerEntries];
  uint16_t       payloadLen;
  const uint8_t* payload;  // points into the recv buffer; caller must not retain past this call
};

// --- Internal helpers (header-internal; not part of the public ABI). ---
namespace detail {

inline bool isValidOverrideSurfaceByte(uint8_t b) {
  return b == static_cast<uint8_t>(OverrideSurface::Base) ||
         b == static_cast<uint8_t>(OverrideSurface::Shade) ||
         b == static_cast<uint8_t>(OverrideSurface::Any);
}

// 0x00 None, 0x01 Wisp, 0x02 PeerSwap, 0xFF Any, 0x10..0xFE user-defined.
// Reject 0x03..0x0F (reserved) explicitly. 0xFF maps to Any.
inline bool isValidOverrideSourceByte(uint8_t b) {
  if (b == static_cast<uint8_t>(OverrideSource::None)) return true;
  if (b == static_cast<uint8_t>(OverrideSource::Wisp)) return true;
  if (b == static_cast<uint8_t>(OverrideSource::PeerSwap)) return true;
  if (b == static_cast<uint8_t>(OverrideSource::Any)) return true;
  if (b >= 0x10 && b <= 0xFE) return true;
  return false;  // 0x03..0x0F reserved
}

// Write the 6-byte header (magic + version + msgType + seq LE) to `buf`.
inline void writeHeader(uint8_t* buf, uint8_t msgType, uint16_t seq) {
  buf[0] = MAGIC_0;
  buf[1] = MAGIC_1;
  buf[2] = PROTOCOL_VERSION;
  buf[3] = msgType;
  buf[4] = static_cast<uint8_t>(seq & 0xFF);
  buf[5] = static_cast<uint8_t>((seq >> 8) & 0xFF);
}

}  // namespace detail

// Build a HELLO frame into `buf`. `name` is utf-8, NOT null-terminated on the wire.
// `nameLen` clamped to HELLO_MAX_NAME. `firmwareVersion` is the sender's packed
// semver (see version.hpp). Returns 0 on bad args, total bytes written on success.
inline size_t buildHello(uint8_t* buf, size_t bufLen, uint16_t seq,
                         const uint8_t sourceMac[6],
                         const uint8_t shadeRGBW[4], const uint8_t baseRGBW[4],
                         uint32_t firmwareVersion,
                         const char* name, size_t nameLen) {
  if (!buf || !sourceMac || !shadeRGBW || !baseRGBW) return 0;
  if (nameLen > HELLO_MAX_NAME) nameLen = HELLO_MAX_NAME;
  const size_t total = HELLO_FIXED_SIZE + 1 + nameLen;  // +1 for nameLen byte
  if (bufLen < total) return 0;
  buf[0] = MAGIC_0;
  buf[1] = MAGIC_1;
  buf[2] = PROTOCOL_VERSION;
  buf[3] = MSG_HELLO;
  buf[4] = static_cast<uint8_t>(seq & 0xFF);
  buf[5] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  std::memcpy(&buf[6], sourceMac, 6);
  std::memcpy(&buf[12], shadeRGBW, 4);
  std::memcpy(&buf[16], baseRGBW, 4);
  // Little-endian on the wire; matches every ESP32 we ship and matches the
  // memcpy-into-uint32 pattern parseHello uses on the receive side.
  buf[20] = static_cast<uint8_t>(firmwareVersion & 0xFF);
  buf[21] = static_cast<uint8_t>((firmwareVersion >> 8) & 0xFF);
  buf[22] = static_cast<uint8_t>((firmwareVersion >> 16) & 0xFF);
  buf[23] = static_cast<uint8_t>((firmwareVersion >> 24) & 0xFF);
  buf[24] = static_cast<uint8_t>(nameLen);
  if (nameLen && name) std::memcpy(&buf[25], name, nameLen);
  return total;
}

// Build a CONTROL_OP frame. Payload is opaque (JSON in practice). Returns
// total bytes written on success, 0 on bad args / oversize.
inline size_t buildControlOp(uint8_t* buf, size_t bufLen, uint16_t seq,
                             const uint8_t targetMac[6],
                             const uint8_t sourceMac[6],
                             const uint8_t* payload, size_t payloadLen,
                             bool localOnly = false) {
  if (!buf || !targetMac || !sourceMac) return 0;
  if (payloadLen > CONTROL_MAX_PAYLOAD) return 0;
  const size_t total = CONTROL_FIXED + payloadLen;
  if (bufLen < total) return 0;
  buf[0] = MAGIC_0;
  buf[1] = MAGIC_1;
  buf[2] = PROTOCOL_VERSION;
  // High bit signals "apply locally + skip relay" to receivers. inspect()
  // masks this so msgType comparisons keep working unchanged.
  buf[3] = MSG_CONTROL_OP | (localOnly ? FLAG_LOCAL_ONLY : 0);
  buf[4] = static_cast<uint8_t>(seq & 0xFF);
  buf[5] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  std::memcpy(&buf[6], targetMac, 6);
  std::memcpy(&buf[12], sourceMac, 6);
  buf[18] = static_cast<uint8_t>(payloadLen & 0xFF);
  buf[19] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  if (payloadLen && payload) std::memcpy(&buf[CONTROL_FIXED], payload, payloadLen);
  return total;
}

// --- Phase C builders ---

// Build a MSG_WISP_HELLO frame. `paletteIdPrefix` and `carriedFwChannel`
// are fixed-width 8-byte slots, zero-padded if shorter. Strings longer than
// 8 bytes are truncated. Returns WISP_HELLO_FIXED_SIZE on success, 0 on
// bad args.
inline size_t buildWispHello(uint8_t* buf, size_t bufLen, uint16_t seq,
                             const uint8_t sourceMac[6],
                             uint32_t wispVersion,
                             uint8_t flags,
                             const char* paletteIdPrefix, size_t paletteIdPrefixLen,
                             const char* carriedFwChannel, size_t carriedFwChannelLen,
                             uint32_t carriedFwVersion) {
  if (!buf || !sourceMac) return 0;
  if (bufLen < WISP_HELLO_FIXED_SIZE) return 0;
  detail::writeHeader(buf, MSG_WISP_HELLO, seq);
  std::memcpy(&buf[6], sourceMac, 6);
  buf[12] = static_cast<uint8_t>(wispVersion & 0xFF);
  buf[13] = static_cast<uint8_t>((wispVersion >> 8) & 0xFF);
  buf[14] = static_cast<uint8_t>((wispVersion >> 16) & 0xFF);
  buf[15] = static_cast<uint8_t>((wispVersion >> 24) & 0xFF);
  buf[16] = flags;
  // Zero-pad the two fixed-width string slots before copying so short
  // values land cleanly.
  std::memset(&buf[17], 0, WISP_HELLO_PALETTE_ID_PREFIX_LEN);
  if (paletteIdPrefix && paletteIdPrefixLen) {
    const size_t n = paletteIdPrefixLen > WISP_HELLO_PALETTE_ID_PREFIX_LEN
                         ? WISP_HELLO_PALETTE_ID_PREFIX_LEN
                         : paletteIdPrefixLen;
    std::memcpy(&buf[17], paletteIdPrefix, n);
  }
  std::memset(&buf[17 + WISP_HELLO_PALETTE_ID_PREFIX_LEN], 0,
              WISP_HELLO_FW_CHANNEL_LEN);
  if (carriedFwChannel && carriedFwChannelLen) {
    const size_t n = carriedFwChannelLen > WISP_HELLO_FW_CHANNEL_LEN
                         ? WISP_HELLO_FW_CHANNEL_LEN
                         : carriedFwChannelLen;
    std::memcpy(&buf[17 + WISP_HELLO_PALETTE_ID_PREFIX_LEN],
                carriedFwChannel, n);
  }
  const size_t fwOff = 17 + WISP_HELLO_PALETTE_ID_PREFIX_LEN +
                       WISP_HELLO_FW_CHANNEL_LEN;
  buf[fwOff]     = static_cast<uint8_t>(carriedFwVersion & 0xFF);
  buf[fwOff + 1] = static_cast<uint8_t>((carriedFwVersion >> 8) & 0xFF);
  buf[fwOff + 2] = static_cast<uint8_t>((carriedFwVersion >> 16) & 0xFF);
  buf[fwOff + 3] = static_cast<uint8_t>((carriedFwVersion >> 24) & 0xFF);
  return WISP_HELLO_FIXED_SIZE;
}

// Build a MSG_OVERRIDE_COLORS frame. `numColors` must be 1..kMaxOverrideColorsPerFrame.
// `colorsRGBW` is numColors * 4 bytes. Returns total bytes written on success.
inline size_t buildOverrideColors(uint8_t* buf, size_t bufLen, uint16_t seq,
                                  const uint8_t sourceMac[6],
                                  const uint8_t targetMac[6],
                                  OverrideSurface surface,
                                  OverrideSource sourceKind,
                                  uint16_t fadeDurationMs,
                                  const uint8_t* colorsRGBW,
                                  uint8_t numColors) {
  if (!buf || !sourceMac || !targetMac || !colorsRGBW) return 0;
  if (numColors == 0 || numColors > kMaxOverrideColorsPerFrame) return 0;
  if (!detail::isValidOverrideSurfaceByte(static_cast<uint8_t>(surface))) return 0;
  if (!detail::isValidOverrideSourceByte(static_cast<uint8_t>(sourceKind))) return 0;
  const size_t total = OVERRIDE_COLORS_FIXED_SIZE + numColors * 4u;
  if (bufLen < total) return 0;
  detail::writeHeader(buf, MSG_OVERRIDE_COLORS, seq);
  std::memcpy(&buf[6], sourceMac, 6);
  std::memcpy(&buf[12], targetMac, 6);
  buf[18] = static_cast<uint8_t>(surface);
  buf[19] = static_cast<uint8_t>(sourceKind);
  buf[20] = static_cast<uint8_t>(fadeDurationMs & 0xFF);
  buf[21] = static_cast<uint8_t>((fadeDurationMs >> 8) & 0xFF);
  buf[22] = numColors;
  std::memcpy(&buf[OVERRIDE_COLORS_FIXED_SIZE], colorsRGBW, numColors * 4u);
  return total;
}

// Build a MSG_RESTORE_COLORS frame. Returns RESTORE_FIXED_SIZE on success.
inline size_t buildRestoreColors(uint8_t* buf, size_t bufLen, uint16_t seq,
                                 const uint8_t sourceMac[6],
                                 const uint8_t targetMac[6],
                                 OverrideSurface surface,
                                 OverrideSource sourceKind,
                                 uint16_t fadeDurationMs) {
  if (!buf || !sourceMac || !targetMac) return 0;
  if (bufLen < RESTORE_FIXED_SIZE) return 0;
  if (!detail::isValidOverrideSurfaceByte(static_cast<uint8_t>(surface))) return 0;
  if (!detail::isValidOverrideSourceByte(static_cast<uint8_t>(sourceKind))) return 0;
  detail::writeHeader(buf, MSG_RESTORE_COLORS, seq);
  std::memcpy(&buf[6], sourceMac, 6);
  std::memcpy(&buf[12], targetMac, 6);
  buf[18] = static_cast<uint8_t>(surface);
  buf[19] = static_cast<uint8_t>(sourceKind);
  buf[20] = static_cast<uint8_t>(fadeDurationMs & 0xFF);
  buf[21] = static_cast<uint8_t>((fadeDurationMs >> 8) & 0xFF);
  return RESTORE_FIXED_SIZE;
}

// Build a MSG_OVERRIDE_BRIGHTNESS frame. `brightness` clamped to
// [kBrightnessOverrideMin, 100] — values outside are rejected (returns 0),
// not silently clamped, so callers can spot bugs.
inline size_t buildOverrideBrightness(uint8_t* buf, size_t bufLen, uint16_t seq,
                                      const uint8_t sourceMac[6],
                                      const uint8_t targetMac[6],
                                      OverrideSurface surface,
                                      OverrideSource sourceKind,
                                      uint16_t fadeDurationMs,
                                      uint8_t brightness) {
  if (!buf || !sourceMac || !targetMac) return 0;
  if (bufLen < OVERRIDE_BRIGHTNESS_FIXED_SIZE) return 0;
  if (brightness < kBrightnessOverrideMin || brightness > 100) return 0;
  if (!detail::isValidOverrideSurfaceByte(static_cast<uint8_t>(surface))) return 0;
  if (!detail::isValidOverrideSourceByte(static_cast<uint8_t>(sourceKind))) return 0;
  detail::writeHeader(buf, MSG_OVERRIDE_BRIGHTNESS, seq);
  std::memcpy(&buf[6], sourceMac, 6);
  std::memcpy(&buf[12], targetMac, 6);
  buf[18] = static_cast<uint8_t>(surface);
  buf[19] = static_cast<uint8_t>(sourceKind);
  buf[20] = static_cast<uint8_t>(fadeDurationMs & 0xFF);
  buf[21] = static_cast<uint8_t>((fadeDurationMs >> 8) & 0xFF);
  buf[22] = brightness;
  return OVERRIDE_BRIGHTNESS_FIXED_SIZE;
}

// Build a MSG_RESTORE_BRIGHTNESS frame. Same layout as RESTORE_COLORS.
inline size_t buildRestoreBrightness(uint8_t* buf, size_t bufLen, uint16_t seq,
                                     const uint8_t sourceMac[6],
                                     const uint8_t targetMac[6],
                                     OverrideSurface surface,
                                     OverrideSource sourceKind,
                                     uint16_t fadeDurationMs) {
  if (!buf || !sourceMac || !targetMac) return 0;
  if (bufLen < RESTORE_FIXED_SIZE) return 0;
  if (!detail::isValidOverrideSurfaceByte(static_cast<uint8_t>(surface))) return 0;
  if (!detail::isValidOverrideSourceByte(static_cast<uint8_t>(sourceKind))) return 0;
  detail::writeHeader(buf, MSG_RESTORE_BRIGHTNESS, seq);
  std::memcpy(&buf[6], sourceMac, 6);
  std::memcpy(&buf[12], targetMac, 6);
  buf[18] = static_cast<uint8_t>(surface);
  buf[19] = static_cast<uint8_t>(sourceKind);
  buf[20] = static_cast<uint8_t>(fadeDurationMs & 0xFF);
  buf[21] = static_cast<uint8_t>((fadeDurationMs >> 8) & 0xFF);
  return RESTORE_FIXED_SIZE;
}

// Build a MSG_EVENT frame. `eventKind` is the raw byte so callers can emit
// user-defined kinds in 0x10..0xFF without re-extending the enum. Stagger
// entries are `numStaggerEntries * (mac(6) + delayMs(2 LE))`; capped at
// kMaxStaggerEntries. Payload is opaque (JSON in practice); capped at
// EVENT_MAX_PAYLOAD so the full frame stays within ESP-NOW's 250-byte limit
// even with a full stagger list.
inline size_t buildEvent(uint8_t* buf, size_t bufLen, uint16_t seq,
                         const uint8_t sourceMac[6],
                         uint8_t eventKind,
                         const uint8_t* staggerMacs,    // numStaggerEntries * 6
                         const uint16_t* staggerDelays, // numStaggerEntries
                         uint8_t numStaggerEntries,
                         const uint8_t* payload, uint16_t payloadLen) {
  if (!buf || !sourceMac) return 0;
  if (numStaggerEntries > kMaxStaggerEntries) return 0;
  if (numStaggerEntries > 0 && (!staggerMacs || !staggerDelays)) return 0;
  if (payloadLen > EVENT_MAX_PAYLOAD) return 0;
  const size_t total = EVENT_FIXED_SIZE +
                       static_cast<size_t>(numStaggerEntries) * EVENT_STAGGER_ENTRY +
                       payloadLen;
  if (bufLen < total) return 0;
  detail::writeHeader(buf, MSG_EVENT, seq);
  std::memcpy(&buf[6], sourceMac, 6);
  buf[12] = eventKind;
  buf[13] = numStaggerEntries;
  size_t off = 14;
  for (uint8_t i = 0; i < numStaggerEntries; ++i) {
    std::memcpy(&buf[off], &staggerMacs[i * 6], 6);
    off += 6;
    buf[off]     = static_cast<uint8_t>(staggerDelays[i] & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((staggerDelays[i] >> 8) & 0xFF);
    off += 2;
  }
  buf[off]     = static_cast<uint8_t>(payloadLen & 0xFF);
  buf[off + 1] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  off += 2;
  if (payloadLen && payload) std::memcpy(&buf[off], payload, payloadLen);
  return total;
}

// Validate magic + version. Returns the msg type (with the FLAG_LOCAL_ONLY
// high bit masked out so MSG_TYPE comparisons keep working) or 0 if invalid.
// TODO(C.3): remove FLAG_LOCAL_ONLY masking once cascade migrates to MSG_EVENT.
inline uint8_t inspect(const uint8_t* data, size_t len) {
  if (!data || len < HEADER_SIZE) return 0;
  if (data[0] != MAGIC_0 || data[1] != MAGIC_1) return 0;
  if (data[2] != PROTOCOL_VERSION) return 0;
  return data[3] & MSG_TYPE_MASK;
}

// Peek at the FLAG_LOCAL_ONLY bit without parsing the rest of the frame.
// Returns false for malformed / non-matching frames.
inline bool isLocalOnly(const uint8_t* data, size_t len) {
  if (!data || len < HEADER_SIZE) return false;
  if (data[0] != MAGIC_0 || data[1] != MAGIC_1) return false;
  if (data[2] != PROTOCOL_VERSION) return false;
  return (data[3] & FLAG_LOCAL_ONLY) != 0;
}

inline bool parseControlOp(const uint8_t* data, size_t len, ParsedControlOp& out) {
  if (inspect(data, len) != MSG_CONTROL_OP || len < CONTROL_FIXED) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.targetMac, &data[6], 6);
  std::memcpy(out.sourceMac, &data[12], 6);
  out.payloadLen = static_cast<uint16_t>(data[18]) | (static_cast<uint16_t>(data[19]) << 8);
  if (out.payloadLen > CONTROL_MAX_PAYLOAD) return false;
  if (len < CONTROL_FIXED + out.payloadLen) return false;
  out.payload = &data[CONTROL_FIXED];
  out.localOnly = (data[3] & FLAG_LOCAL_ONLY) != 0;
  return true;
}

inline bool parseHello(const uint8_t* data, size_t len, ParsedHello& out) {
  if (inspect(data, len) != MSG_HELLO || len < HELLO_FIXED_SIZE + 1) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.sourceMac, &data[6], 6);
  std::memcpy(out.shade, &data[12], 4);
  std::memcpy(out.base, &data[16], 4);
  out.firmwareVersion =
       static_cast<uint32_t>(data[20])
     | (static_cast<uint32_t>(data[21]) << 8)
     | (static_cast<uint32_t>(data[22]) << 16)
     | (static_cast<uint32_t>(data[23]) << 24);
  const uint8_t rawLen = data[24];
  const uint8_t nameLen = rawLen > HELLO_MAX_NAME ? HELLO_MAX_NAME : rawLen;
  if (len < static_cast<size_t>(HELLO_FIXED_SIZE + 1 + nameLen)) return false;
  out.nameLen = nameLen;
  if (nameLen) std::memcpy(out.name, &data[25], nameLen);
  out.name[nameLen] = '\0';
  return true;
}

// --- Phase C parsers ---

inline bool parseWispHello(const uint8_t* data, size_t len, ParsedWispHello& out) {
  if (inspect(data, len) != MSG_WISP_HELLO || len < WISP_HELLO_FIXED_SIZE) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.sourceMac, &data[6], 6);
  out.wispVersion =
       static_cast<uint32_t>(data[12])
     | (static_cast<uint32_t>(data[13]) << 8)
     | (static_cast<uint32_t>(data[14]) << 16)
     | (static_cast<uint32_t>(data[15]) << 24);
  out.flags = data[16];
  std::memcpy(out.paletteIdPrefix, &data[17], WISP_HELLO_PALETTE_ID_PREFIX_LEN);
  out.paletteIdPrefix[WISP_HELLO_PALETTE_ID_PREFIX_LEN] = '\0';
  std::memcpy(out.carriedFwChannel,
              &data[17 + WISP_HELLO_PALETTE_ID_PREFIX_LEN],
              WISP_HELLO_FW_CHANNEL_LEN);
  out.carriedFwChannel[WISP_HELLO_FW_CHANNEL_LEN] = '\0';
  const size_t fwOff = 17 + WISP_HELLO_PALETTE_ID_PREFIX_LEN +
                       WISP_HELLO_FW_CHANNEL_LEN;
  out.carriedFwVersion =
       static_cast<uint32_t>(data[fwOff])
     | (static_cast<uint32_t>(data[fwOff + 1]) << 8)
     | (static_cast<uint32_t>(data[fwOff + 2]) << 16)
     | (static_cast<uint32_t>(data[fwOff + 3]) << 24);
  return true;
}

inline bool parseOverrideColors(const uint8_t* data, size_t len,
                                ParsedOverrideColors& out) {
  if (inspect(data, len) != MSG_OVERRIDE_COLORS) return false;
  if (len < OVERRIDE_COLORS_FIXED_SIZE) return false;
  if (!detail::isValidOverrideSurfaceByte(data[18])) return false;
  if (!detail::isValidOverrideSourceByte(data[19])) return false;
  const uint8_t numColors = data[22];
  if (numColors < 1 || numColors > kMaxOverrideColorsPerFrame) return false;
  const size_t expected = OVERRIDE_COLORS_FIXED_SIZE + numColors * 4u;
  if (len != expected) return false;  // exact-match — silent drop on length mismatch
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.sourceMac, &data[6], 6);
  std::memcpy(out.targetMac, &data[12], 6);
  out.surface    = static_cast<OverrideSurface>(data[18]);
  out.sourceKind = static_cast<OverrideSource>(data[19]);
  out.fadeDurationMs =
       static_cast<uint16_t>(data[20])
     | (static_cast<uint16_t>(data[21]) << 8);
  out.numColors = numColors;
  std::memcpy(out.colors, &data[OVERRIDE_COLORS_FIXED_SIZE], numColors * 4u);
  return true;
}

inline bool parseRestoreColors(const uint8_t* data, size_t len,
                               ParsedRestoreColors& out) {
  if (inspect(data, len) != MSG_RESTORE_COLORS) return false;
  if (len < RESTORE_FIXED_SIZE) return false;
  if (!detail::isValidOverrideSurfaceByte(data[18])) return false;
  if (!detail::isValidOverrideSourceByte(data[19])) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.sourceMac, &data[6], 6);
  std::memcpy(out.targetMac, &data[12], 6);
  out.surface    = static_cast<OverrideSurface>(data[18]);
  out.sourceKind = static_cast<OverrideSource>(data[19]);
  out.fadeDurationMs =
       static_cast<uint16_t>(data[20])
     | (static_cast<uint16_t>(data[21]) << 8);
  return true;
}

inline bool parseOverrideBrightness(const uint8_t* data, size_t len,
                                    ParsedOverrideBrightness& out) {
  if (inspect(data, len) != MSG_OVERRIDE_BRIGHTNESS) return false;
  if (len < OVERRIDE_BRIGHTNESS_FIXED_SIZE) return false;
  if (!detail::isValidOverrideSurfaceByte(data[18])) return false;
  if (!detail::isValidOverrideSourceByte(data[19])) return false;
  const uint8_t brightness = data[22];
  if (brightness < kBrightnessOverrideMin || brightness > 100) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.sourceMac, &data[6], 6);
  std::memcpy(out.targetMac, &data[12], 6);
  out.surface    = static_cast<OverrideSurface>(data[18]);
  out.sourceKind = static_cast<OverrideSource>(data[19]);
  out.fadeDurationMs =
       static_cast<uint16_t>(data[20])
     | (static_cast<uint16_t>(data[21]) << 8);
  out.brightness = brightness;
  return true;
}

inline bool parseRestoreBrightness(const uint8_t* data, size_t len,
                                   ParsedRestoreBrightness& out) {
  if (inspect(data, len) != MSG_RESTORE_BRIGHTNESS) return false;
  if (len < RESTORE_FIXED_SIZE) return false;
  if (!detail::isValidOverrideSurfaceByte(data[18])) return false;
  if (!detail::isValidOverrideSourceByte(data[19])) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.sourceMac, &data[6], 6);
  std::memcpy(out.targetMac, &data[12], 6);
  out.surface    = static_cast<OverrideSurface>(data[18]);
  out.sourceKind = static_cast<OverrideSource>(data[19]);
  out.fadeDurationMs =
       static_cast<uint16_t>(data[20])
     | (static_cast<uint16_t>(data[21]) << 8);
  return true;
}

inline bool parseEvent(const uint8_t* data, size_t len, ParsedEvent& out) {
  if (inspect(data, len) != MSG_EVENT) return false;
  if (len < EVENT_FIXED_SIZE) return false;
  const uint8_t numStaggerEntries = data[13];
  if (numStaggerEntries > kMaxStaggerEntries) return false;
  const size_t staggerBytes =
      static_cast<size_t>(numStaggerEntries) * EVENT_STAGGER_ENTRY;
  // Need room for the stagger block + the 2-byte payloadLen.
  if (len < EVENT_FIXED_SIZE + staggerBytes) return false;
  const size_t payloadLenOff = HEADER_SIZE + 6 + 1 + 1 + staggerBytes;  // 14 + staggerBytes
  const uint16_t payloadLen =
       static_cast<uint16_t>(data[payloadLenOff])
     | (static_cast<uint16_t>(data[payloadLenOff + 1]) << 8);
  if (payloadLen > EVENT_MAX_PAYLOAD) return false;
  const size_t expected = EVENT_FIXED_SIZE + staggerBytes + payloadLen;
  if (len != expected) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.sourceMac, &data[6], 6);
  out.eventKindRaw = data[12];
  out.eventKind = static_cast<EventKind>(out.eventKindRaw);
  out.numStaggerEntries = numStaggerEntries;
  size_t off = 14;
  for (uint8_t i = 0; i < numStaggerEntries; ++i) {
    std::memcpy(out.staggerEntries[i].mac, &data[off], 6);
    off += 6;
    out.staggerEntries[i].delayMs =
         static_cast<uint16_t>(data[off])
       | (static_cast<uint16_t>(data[off + 1]) << 8);
    off += 2;
  }
  out.payloadLen = payloadLen;
  out.payload    = payloadLen ? &data[payloadLenOff + 2] : nullptr;
  return true;
}

// Gossip dedup: small fixed-size ring tracking (sourceMac, msgType, seq) tuples
// seen recently. Drops duplicates so re-broadcasts terminate.
//
// Concurrency: record() can be called from BOTH the ESP-NOW recv task
// (Core 0, via ShowReceiver::handleRecv) AND the Arduino loop task
// (Core 1, via ShowReceiver::sendControlOp recording our own sent ops
// so the inbound re-broadcast doesn't loop back). The critical section
// is the compare loop + slot write — kept SHORT: no allocations, no
// network calls, no logging. See audit finding #7 / Stability #3.
class DedupRing {
 public:
  static constexpr size_t CAPACITY = 32;

  // Returns true if (mac, msgType, seq) is new (and records it); false if seen.
  bool record(const uint8_t mac[6], uint8_t msgType, uint16_t seq) {
    LAMP_PROTOCOL_PORTMUX_ENTER(&mux_);
    for (size_t i = 0; i < CAPACITY; i++) {
      const Entry& e = entries_[i];
      if (e.used && e.msgType == msgType && e.seq == seq &&
          std::memcmp(e.mac, mac, 6) == 0) {
        LAMP_PROTOCOL_PORTMUX_EXIT(&mux_);
        return false;
      }
    }
    Entry& slot = entries_[head_];
    slot.used = true;
    slot.msgType = msgType;
    slot.seq = seq;
    std::memcpy(slot.mac, mac, 6);
    head_ = (head_ + 1) % CAPACITY;
    LAMP_PROTOCOL_PORTMUX_EXIT(&mux_);
    return true;
  }

 private:
  struct Entry {
    bool used = false;
    uint8_t msgType = 0;
    uint16_t seq = 0;
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  };
  Entry entries_[CAPACITY];
  size_t head_ = 0;
  LAMP_PROTOCOL_PORTMUX_TYPE mux_ = LAMP_PROTOCOL_PORTMUX_INIT;
};

}  // namespace lamp_protocol
