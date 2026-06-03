// Native-host round-trip tests for the Phase C protocol additions:
// MSG_WISP_HELLO, MSG_OVERRIDE_COLORS, MSG_RESTORE_COLORS,
// MSG_OVERRIDE_BRIGHTNESS, MSG_RESTORE_BRIGHTNESS, MSG_EVENT.
//
// The point is to pin the wire format so a refactor of the header
// can't silently shift byte offsets or drop a field. We include the
// production header directly — it's self-contained and provides a
// no-op portMUX shim when neither ARDUINO nor ESP_PLATFORM is defined.

#include <unity.h>

#include <cstdint>
#include <cstring>

#include "components/network/lamp_protocol.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace lp = lamp_protocol;

static const uint8_t kSrcMac[6]    = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
static const uint8_t kTargetMac[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};

// --- MSG_WISP_HELLO ---

void test_wisp_hello_roundtrip() {
  uint8_t buf[lp::MAX_PACKET_SIZE];
  const char palette[] = "palette7";   // exactly 8 bytes, no terminator on wire
  const char channel[] = "stable\0\0"; // shorter — buildWispHello zero-pads
  const size_t n = lp::buildWispHello(
      buf, sizeof(buf), /*seq=*/0x1234, kSrcMac,
      /*wispVersion=*/0x01020304,
      /*flags=*/lp::WISP_HELLO_FLAG_PAINT_MODE | lp::WISP_HELLO_FLAG_AURORA_CONNECTED,
      palette, 8,
      channel, 6,  // only first 6 chars meaningful
      /*carriedFwVersion=*/0xCAFEF00D);
  TEST_ASSERT_EQUAL_UINT32(lp::WISP_HELLO_FIXED_SIZE, n);
  TEST_ASSERT_EQUAL_UINT8(lp::MSG_WISP_HELLO, lp::inspect(buf, n));

  lp::ParsedWispHello out;
  TEST_ASSERT_TRUE(lp::parseWispHello(buf, n, out));
  TEST_ASSERT_EQUAL_UINT16(0x1234, out.seq);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kSrcMac, out.sourceMac, 6);
  TEST_ASSERT_EQUAL_UINT32(0x01020304u, out.wispVersion);
  TEST_ASSERT_EQUAL_UINT8(lp::WISP_HELLO_FLAG_PAINT_MODE | lp::WISP_HELLO_FLAG_AURORA_CONNECTED,
                          out.flags);
  TEST_ASSERT_EQUAL_STRING("palette7", out.paletteIdPrefix);
  TEST_ASSERT_EQUAL_STRING("stable", out.carriedFwChannel);
  TEST_ASSERT_EQUAL_UINT32(0xCAFEF00Du, out.carriedFwVersion);
}

void test_wisp_hello_too_short_rejected() {
  uint8_t buf[lp::WISP_HELLO_FIXED_SIZE];
  std::memset(buf, 0, sizeof(buf));
  // Valid frame
  TEST_ASSERT_EQUAL_UINT32(lp::WISP_HELLO_FIXED_SIZE,
      lp::buildWispHello(buf, sizeof(buf), 1, kSrcMac, 0, 0, "", 0, "", 0, 0));
  lp::ParsedWispHello out;
  TEST_ASSERT_FALSE(lp::parseWispHello(buf, lp::WISP_HELLO_FIXED_SIZE - 1, out));
}

// --- MSG_OVERRIDE_COLORS ---

void test_override_colors_roundtrip_min_and_max() {
  uint8_t buf[lp::OVERRIDE_COLORS_MAX_SIZE];

  // N=1 (minimum)
  const uint8_t one[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  size_t n = lp::buildOverrideColors(buf, sizeof(buf), 7, kSrcMac, kTargetMac,
                                     lp::OverrideSurface::Base,
                                     lp::OverrideSource::Wisp,
                                     /*fadeMs=*/500,
                                     one, 1);
  TEST_ASSERT_EQUAL_UINT32(lp::OVERRIDE_COLORS_FIXED_SIZE + 4, n);
  lp::ParsedOverrideColors out;
  TEST_ASSERT_TRUE(lp::parseOverrideColors(buf, n, out));
  TEST_ASSERT_EQUAL_UINT16(7, out.seq);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(lp::OverrideSurface::Base),
                          static_cast<uint8_t>(out.surface));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(lp::OverrideSource::Wisp),
                          static_cast<uint8_t>(out.sourceKind));
  TEST_ASSERT_EQUAL_UINT16(500, out.fadeDurationMs);
  TEST_ASSERT_EQUAL_UINT8(1, out.numColors);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(one, out.colors[0], 4);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kSrcMac, out.sourceMac, 6);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kTargetMac, out.targetMac, 6);

  // N=8 (maximum)
  uint8_t eight[8 * 4];
  for (size_t i = 0; i < sizeof(eight); ++i) eight[i] = static_cast<uint8_t>(i ^ 0x5A);
  n = lp::buildOverrideColors(buf, sizeof(buf), 8, kSrcMac, kTargetMac,
                              lp::OverrideSurface::Any,
                              lp::OverrideSource::Any,
                              /*fadeMs=*/0xFFFF,
                              eight, 8);
  TEST_ASSERT_EQUAL_UINT32(lp::OVERRIDE_COLORS_FIXED_SIZE + 32, n);
  TEST_ASSERT_TRUE(lp::parseOverrideColors(buf, n, out));
  TEST_ASSERT_EQUAL_UINT8(8, out.numColors);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(lp::OverrideSurface::Any),
                          static_cast<uint8_t>(out.surface));
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, out.fadeDurationMs);
  for (uint8_t i = 0; i < 8; ++i) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&eight[i * 4], out.colors[i], 4);
  }
}

void test_override_colors_zero_numcolors_rejected_by_builder() {
  uint8_t buf[lp::OVERRIDE_COLORS_MAX_SIZE];
  const uint8_t dummy[4] = {0};
  TEST_ASSERT_EQUAL_UINT32(0,
      lp::buildOverrideColors(buf, sizeof(buf), 1, kSrcMac, kTargetMac,
                              lp::OverrideSurface::Base,
                              lp::OverrideSource::Wisp, 0, dummy, 0));
}

void test_override_colors_over_max_rejected_by_builder() {
  uint8_t buf[lp::OVERRIDE_COLORS_MAX_SIZE + 16];
  uint8_t many[9 * 4] = {0};
  TEST_ASSERT_EQUAL_UINT32(0,
      lp::buildOverrideColors(buf, sizeof(buf), 1, kSrcMac, kTargetMac,
                              lp::OverrideSurface::Base,
                              lp::OverrideSource::Wisp, 0, many, 9));
}

void test_override_colors_unknown_surface_byte_rejected_by_parser() {
  // Hand-craft a frame with an illegal surface byte (0x42).
  uint8_t buf[lp::OVERRIDE_COLORS_FIXED_SIZE + 4];
  const uint8_t one[4] = {1, 2, 3, 4};
  const size_t n = lp::buildOverrideColors(buf, sizeof(buf), 1, kSrcMac, kTargetMac,
                                           lp::OverrideSurface::Base,
                                           lp::OverrideSource::Wisp, 100, one, 1);
  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  buf[18] = 0x42;  // corrupt surface byte
  lp::ParsedOverrideColors out;
  TEST_ASSERT_FALSE(lp::parseOverrideColors(buf, n, out));
}

void test_override_colors_reserved_source_byte_rejected_by_parser() {
  uint8_t buf[lp::OVERRIDE_COLORS_FIXED_SIZE + 4];
  const uint8_t one[4] = {1, 2, 3, 4};
  const size_t n = lp::buildOverrideColors(buf, sizeof(buf), 1, kSrcMac, kTargetMac,
                                           lp::OverrideSurface::Base,
                                           lp::OverrideSource::Wisp, 100, one, 1);
  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  buf[19] = 0x05;  // reserved range 0x03..0x0F
  lp::ParsedOverrideColors out;
  TEST_ASSERT_FALSE(lp::parseOverrideColors(buf, n, out));
}

void test_override_colors_length_mismatch_rejected_by_parser() {
  uint8_t buf[lp::OVERRIDE_COLORS_FIXED_SIZE + 8];
  const uint8_t two[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const size_t n = lp::buildOverrideColors(buf, sizeof(buf), 1, kSrcMac, kTargetMac,
                                           lp::OverrideSurface::Base,
                                           lp::OverrideSource::Wisp, 100, two, 2);
  // Truncate the frame so length != fixed + numColors*4
  lp::ParsedOverrideColors out;
  TEST_ASSERT_FALSE(lp::parseOverrideColors(buf, n - 1, out));
  // Extend the frame by one byte (still over) — also rejected.
  TEST_ASSERT_FALSE(lp::parseOverrideColors(buf, n + 1, out));
}

// --- MSG_RESTORE_COLORS ---

void test_restore_colors_roundtrip() {
  uint8_t buf[lp::RESTORE_FIXED_SIZE];
  const size_t n = lp::buildRestoreColors(buf, sizeof(buf), 0xBEEF, kSrcMac, kTargetMac,
                                          lp::OverrideSurface::Shade,
                                          lp::OverrideSource::PeerSwap,
                                          /*fadeMs=*/1000);
  TEST_ASSERT_EQUAL_UINT32(lp::RESTORE_FIXED_SIZE, n);
  lp::ParsedRestoreColors out;
  TEST_ASSERT_TRUE(lp::parseRestoreColors(buf, n, out));
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, out.seq);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(lp::OverrideSurface::Shade),
                          static_cast<uint8_t>(out.surface));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(lp::OverrideSource::PeerSwap),
                          static_cast<uint8_t>(out.sourceKind));
  TEST_ASSERT_EQUAL_UINT16(1000, out.fadeDurationMs);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kSrcMac, out.sourceMac, 6);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kTargetMac, out.targetMac, 6);
}

// --- MSG_OVERRIDE_BRIGHTNESS ---

void test_override_brightness_roundtrip() {
  uint8_t buf[lp::OVERRIDE_BRIGHTNESS_FIXED_SIZE];
  const size_t n = lp::buildOverrideBrightness(buf, sizeof(buf), 42,
                                               kSrcMac, kTargetMac,
                                               lp::OverrideSurface::Any,
                                               lp::OverrideSource::Wisp,
                                               /*fadeMs=*/2500,
                                               /*brightness=*/75);
  TEST_ASSERT_EQUAL_UINT32(lp::OVERRIDE_BRIGHTNESS_FIXED_SIZE, n);
  lp::ParsedOverrideBrightness out;
  TEST_ASSERT_TRUE(lp::parseOverrideBrightness(buf, n, out));
  TEST_ASSERT_EQUAL_UINT16(42, out.seq);
  TEST_ASSERT_EQUAL_UINT8(75, out.brightness);
  TEST_ASSERT_EQUAL_UINT16(2500, out.fadeDurationMs);
}

void test_override_brightness_too_low_rejected_by_builder() {
  uint8_t buf[lp::OVERRIDE_BRIGHTNESS_FIXED_SIZE];
  TEST_ASSERT_EQUAL_UINT32(0,
      lp::buildOverrideBrightness(buf, sizeof(buf), 1, kSrcMac, kTargetMac,
                                  lp::OverrideSurface::Base,
                                  lp::OverrideSource::Wisp, 0,
                                  /*brightness=*/lp::kBrightnessOverrideMin - 1));
}

void test_override_brightness_over_100_rejected_by_builder() {
  uint8_t buf[lp::OVERRIDE_BRIGHTNESS_FIXED_SIZE];
  TEST_ASSERT_EQUAL_UINT32(0,
      lp::buildOverrideBrightness(buf, sizeof(buf), 1, kSrcMac, kTargetMac,
                                  lp::OverrideSurface::Base,
                                  lp::OverrideSource::Wisp, 0,
                                  /*brightness=*/101));
}

void test_override_brightness_out_of_range_rejected_by_parser() {
  uint8_t buf[lp::OVERRIDE_BRIGHTNESS_FIXED_SIZE];
  const size_t n = lp::buildOverrideBrightness(buf, sizeof(buf), 1,
                                               kSrcMac, kTargetMac,
                                               lp::OverrideSurface::Base,
                                               lp::OverrideSource::Wisp, 0,
                                               /*brightness=*/50);
  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  buf[22] = 200;  // corrupt brightness past 100
  lp::ParsedOverrideBrightness out;
  TEST_ASSERT_FALSE(lp::parseOverrideBrightness(buf, n, out));
}

// --- MSG_RESTORE_BRIGHTNESS ---

void test_restore_brightness_roundtrip() {
  uint8_t buf[lp::RESTORE_FIXED_SIZE];
  const size_t n = lp::buildRestoreBrightness(buf, sizeof(buf), 9, kSrcMac, kTargetMac,
                                              lp::OverrideSurface::Shade,
                                              lp::OverrideSource::None,
                                              /*fadeMs=*/300);
  TEST_ASSERT_EQUAL_UINT32(lp::RESTORE_FIXED_SIZE, n);
  lp::ParsedRestoreBrightness out;
  TEST_ASSERT_TRUE(lp::parseRestoreBrightness(buf, n, out));
  TEST_ASSERT_EQUAL_UINT16(9, out.seq);
  TEST_ASSERT_EQUAL_UINT16(300, out.fadeDurationMs);
}

// --- MSG_EVENT ---

void test_event_roundtrip_no_stagger_no_payload() {
  uint8_t buf[lp::EVENT_MAX_SIZE];
  const size_t n = lp::buildEvent(buf, sizeof(buf), /*seq=*/0x55AA, kSrcMac,
                                  /*eventKind=*/static_cast<uint8_t>(lp::EventKind::ExpressionTriggered),
                                  /*staggerMacs=*/nullptr,
                                  /*staggerDelays=*/nullptr,
                                  /*numStaggerEntries=*/0,
                                  /*payload=*/nullptr, /*payloadLen=*/0);
  TEST_ASSERT_EQUAL_UINT32(lp::EVENT_FIXED_SIZE, n);
  lp::ParsedEvent out;
  TEST_ASSERT_TRUE(lp::parseEvent(buf, n, out));
  TEST_ASSERT_EQUAL_UINT16(0x55AA, out.seq);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(lp::EventKind::ExpressionTriggered),
                          out.eventKindRaw);
  TEST_ASSERT_EQUAL_UINT8(0, out.numStaggerEntries);
  TEST_ASSERT_EQUAL_UINT16(0, out.payloadLen);
}

void test_event_roundtrip_full_stagger_and_payload() {
  uint8_t buf[lp::EVENT_MAX_SIZE];

  // 12 stagger entries
  uint8_t staggerMacs[lp::kMaxStaggerEntries * 6];
  uint16_t staggerDelays[lp::kMaxStaggerEntries];
  for (size_t i = 0; i < lp::kMaxStaggerEntries; ++i) {
    for (size_t b = 0; b < 6; ++b) {
      staggerMacs[i * 6 + b] = static_cast<uint8_t>((i << 4) | b);
    }
    staggerDelays[i] = static_cast<uint16_t>(100 * i + 7);
  }

  // Payload: small JSON-ish blob
  const char payload[] = "{\"expression\":\"hello world\"}";
  const uint16_t payloadLen = static_cast<uint16_t>(sizeof(payload) - 1);

  const size_t n = lp::buildEvent(buf, sizeof(buf), 1, kSrcMac,
                                  /*eventKind=*/0xA5,  // user-defined
                                  staggerMacs, staggerDelays,
                                  lp::kMaxStaggerEntries,
                                  reinterpret_cast<const uint8_t*>(payload),
                                  payloadLen);
  TEST_ASSERT_EQUAL_UINT32(
      lp::EVENT_FIXED_SIZE + lp::kMaxStaggerEntries * lp::EVENT_STAGGER_ENTRY + payloadLen,
      n);

  lp::ParsedEvent out;
  TEST_ASSERT_TRUE(lp::parseEvent(buf, n, out));
  TEST_ASSERT_EQUAL_UINT8(0xA5, out.eventKindRaw);
  TEST_ASSERT_EQUAL_UINT8(lp::kMaxStaggerEntries, out.numStaggerEntries);
  for (size_t i = 0; i < lp::kMaxStaggerEntries; ++i) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&staggerMacs[i * 6], out.staggerEntries[i].mac, 6);
    TEST_ASSERT_EQUAL_UINT16(staggerDelays[i], out.staggerEntries[i].delayMs);
  }
  TEST_ASSERT_EQUAL_UINT16(payloadLen, out.payloadLen);
  TEST_ASSERT_NOT_NULL(out.payload);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out.payload, payloadLen);
}

void test_event_too_many_stagger_entries_rejected_by_builder() {
  uint8_t buf[lp::EVENT_MAX_SIZE];
  uint8_t macs[6] = {0};
  uint16_t delays[1] = {0};
  TEST_ASSERT_EQUAL_UINT32(0,
      lp::buildEvent(buf, sizeof(buf), 1, kSrcMac, 0x01,
                     macs, delays,
                     /*numStaggerEntries=*/lp::kMaxStaggerEntries + 1,
                     nullptr, 0));
}

void test_event_too_many_stagger_entries_rejected_by_parser() {
  // Hand-craft a valid header but with numStaggerEntries=13 (over cap).
  uint8_t buf[lp::EVENT_MAX_SIZE];
  std::memset(buf, 0, sizeof(buf));
  buf[0] = 'L';
  buf[1] = 'M';
  buf[2] = lp::PROTOCOL_VERSION;
  buf[3] = lp::MSG_EVENT;
  buf[4] = 0x01;
  buf[5] = 0x00;
  std::memcpy(&buf[6], kSrcMac, 6);
  buf[12] = 0x01;                                          // eventKind
  buf[13] = static_cast<uint8_t>(lp::kMaxStaggerEntries + 1);  // over cap
  lp::ParsedEvent out;
  TEST_ASSERT_FALSE(lp::parseEvent(buf, lp::EVENT_FIXED_SIZE +
                                        (lp::kMaxStaggerEntries + 1) * lp::EVENT_STAGGER_ENTRY,
                                    out));
}

void test_inspect_masks_local_only_flag_bit() {
  // CONTROL_OP with localOnly=true sets the high bit. inspect() must
  // mask it so msgType comparisons keep working.
  uint8_t buf[lp::CONTROL_MAX_SIZE];
  const size_t n = lp::buildControlOp(buf, sizeof(buf), 1, kTargetMac, kSrcMac,
                                      nullptr, 0, /*localOnly=*/true);
  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_EQUAL_UINT8(lp::MSG_CONTROL_OP, lp::inspect(buf, n));
  TEST_ASSERT_TRUE(lp::isLocalOnly(buf, n));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_wisp_hello_roundtrip);
  RUN_TEST(test_wisp_hello_too_short_rejected);

  RUN_TEST(test_override_colors_roundtrip_min_and_max);
  RUN_TEST(test_override_colors_zero_numcolors_rejected_by_builder);
  RUN_TEST(test_override_colors_over_max_rejected_by_builder);
  RUN_TEST(test_override_colors_unknown_surface_byte_rejected_by_parser);
  RUN_TEST(test_override_colors_reserved_source_byte_rejected_by_parser);
  RUN_TEST(test_override_colors_length_mismatch_rejected_by_parser);

  RUN_TEST(test_restore_colors_roundtrip);

  RUN_TEST(test_override_brightness_roundtrip);
  RUN_TEST(test_override_brightness_too_low_rejected_by_builder);
  RUN_TEST(test_override_brightness_over_100_rejected_by_builder);
  RUN_TEST(test_override_brightness_out_of_range_rejected_by_parser);

  RUN_TEST(test_restore_brightness_roundtrip);

  RUN_TEST(test_event_roundtrip_no_stagger_no_payload);
  RUN_TEST(test_event_roundtrip_full_stagger_and_payload);
  RUN_TEST(test_event_too_many_stagger_entries_rejected_by_builder);
  RUN_TEST(test_event_too_many_stagger_entries_rejected_by_parser);

  RUN_TEST(test_inspect_masks_local_only_flag_bit);

  return UNITY_END();
}
