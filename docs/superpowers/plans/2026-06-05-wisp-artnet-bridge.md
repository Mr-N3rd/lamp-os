# Wisp ArtNet Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add backwards-compatible ArtNet emission to the wisp so pre-mesh lamps (still running the `software/lamp-os` ArtNet-only firmware) can be painted by the same Aurora palette the v0x03 mesh receives — in parallel with the existing ESP-NOW path. Reproduce the old `software/artnet-repeater` BLE-beacon trick so old lamps can auto-join the wisp's WiFi without per-lamp provisioning.

**Architecture:** Three new collaborators in `software/wisp/src/`:
1. **`WifiLink`** — STA-mode join with NVS-persisted creds. Currently `main.cpp` has a TODO for this; we fill it in.
2. **`StageBeacon`** — NimBLE manufacturer-data advertisement (magic ID `42007`, payload `<ssid>\0<password>\0`) lifted byte-for-byte from `software/artnet-repeater/src/main.cpp:42-56`. Old lamps already scan for this in `software/lamp-os/src/components/network/bluetooth.cpp:39-72`. No lamp-side changes.
3. **`ArtnetEmitter`** — opens `AsyncUDP` on port 6454, broadcasts a 530-byte ArtNet DMX universe 1 frame containing 8 fixtures × 10 channels (shade RGBW + base RGBW + mode byte + parameter byte), matching the wire format the lamp listener expects at `software/lamp-os/src/components/network/artnet.cpp`. Per-fixture colors come from `wisp::TupleSampler::sampleTupleForMac(palette, syntheticMac)` so the fixture-index → color mapping is stable across frames but draws from the same gradient the mesh path uses.

A pure-C++ helper `artnet_frame.{h,cpp}` builds the 530-byte buffer, host-portable so it's testable in the existing `[env:native]` Unity suite.

**Tech Stack:**
- ESP32-C6 (Seeed Xiao), Arduino framework via pioarduino platform 55.03.30-2.
- `<AsyncUDP.h>` (already in ESP32 Arduino core — no new lib_deps).
- `NimBLE-Arduino@2.3.6` (already in `platformio.ini`; first BLE consumer on the wisp).
- `<Preferences.h>` (NVS, ESP32 Arduino core) for WiFi cred persistence.
- Unity test framework for `[env:native]` (existing — pattern in `test/test_tuple_sampler/`).

---

## Pre-flight context

Read these before starting:
- `software/wisp/src/main.cpp` — current entry point + serial command dispatcher.
- `software/wisp/src/MeshLink.cpp` — coex anchor: pins ESP-NOW to channel 1 via `esp_wifi_set_channel(LAMP_ESPNOW_CHANNEL=1, ...)` before `esp_now_init`. After we add `WiFi.begin(ssid, pass)`, the radio re-tunes to whatever channel the AP advertises. Mesh reliability degrades the further the AP sits from channel 1. This is documented in `docs/mesh-deployment.md` and is the same constraint Aurora already faces; we do not solve it in this PR.
- `software/wisp/src/TupleSampler.h` — pure function we reuse for synthetic-MAC fixture colors.
- `software/artnet-repeater/src/main.cpp` — reference for BLE advert payload format and IP addressing convention (Gen 1 repeater used static `10.0.0.2`; we use DHCP).
- `software/lamp-os/src/components/network/artnet.cpp` — authoritative wire format the bridge must produce.
- `software/lamp-os/src/components/network/bluetooth.cpp:23-72` — confirms lamp-side BLE-beacon scan logic. Magic `BLE_STAGE_MAGIC_NUMBER = 42007`. Payload `<2 bytes magic><ssid\0><password\0>`.

## File structure

**New files:**
- `software/wisp/src/artnet_frame.h` — pure, host-portable frame-builder declarations.
- `software/wisp/src/artnet_frame.cpp` — pure builder implementation.
- `software/wisp/src/ArtnetEmitter.h` — Arduino-coupled emit class declarations.
- `software/wisp/src/ArtnetEmitter.cpp` — `AsyncUDP` socket + frame-send glue.
- `software/wisp/src/WifiLink.h` — STA join + NVS creds, declarations.
- `software/wisp/src/WifiLink.cpp` — `Preferences`-backed creds + `WiFi.begin`.
- `software/wisp/src/StageBeacon.h` — NimBLE advert declarations.
- `software/wisp/src/StageBeacon.cpp` — NimBLE manufacturer-data advert.
- `software/wisp/test/test_artnet_frame/artnet_frame.cpp` — Unity tests for the pure frame builder.

**Modified files:**
- `software/wisp/src/main.cpp` — instantiate new components, wire palette callback to `ArtnetEmitter::onPaletteChanged()`, add serial commands.
- `software/wisp/platformio.ini` — extend `[env:native]` `build_src_filter` exclusions for the Arduino-coupled new files (keep `artnet_frame.cpp` included so the native build covers it indirectly; Unity test redeclares the algorithm per existing house style).

## Wire format reference (must produce exactly this)

Total frame: **530 bytes**.

| Offset | Length | Field          | Value                                                   |
|--------|--------|----------------|---------------------------------------------------------|
| 0..7   | 8      | ART_NET_ID     | `"Art-Net\0"` (8 bytes including trailing NUL)          |
| 8..9   | 2      | OpCode (LE)    | `0x5000` → bytes `{0x00, 0x50}`                         |
| 10..11 | 2      | ProtVer (BE)   | `0x000E` → bytes `{0x00, 0x0E}` (lamp ignores; mirror Gen 1) |
| 12     | 1      | Sequence       | rolling counter `0..255`                                |
| 13     | 1      | Physical       | `0x00`                                                  |
| 14..15 | 2      | Universe (LE)  | `0x0001` → bytes `{0x01, 0x00}`                         |
| 16..17 | 2      | Length (BE)    | `0x0200` (=512) → bytes `{0x02, 0x00}`                  |
| 18..29 | 12     | Fixture 0 ch.  | shade R,G,B,W; base R,G,B,W; mode=0; param=0            |
| 28..37 | 10     | Fixture 1 ch.  | (10 channels per fixture, ×8 fixtures = 80 bytes total) |
| ...    | ...    | ...            | ...                                                     |
| 97     | last fixture's last byte (param) — fixture 7 starts at offset 88 |
| 98..529 | 432   | Padding        | `0x00`                                                  |

Note: fixture 0 occupies offsets `[18..27]` (10 bytes), fixture 1 `[28..37]`, ..., fixture 7 `[88..97]`. The lamp computes its slot as `lampNumber * 10` into the DMX payload (`artnet.cpp:60`) where `lampNumber = random(0, 7)`.

**Channel layout per fixture (matches `software/lamp-os/src/components/network/artnet.cpp:62-69`):**

| Channel | Byte                              |
|---------|-----------------------------------|
| 0       | shade R                           |
| 1       | shade G                           |
| 2       | shade B                           |
| 3       | shade W                           |
| 4       | base R                            |
| 5       | base G                            |
| 6       | base B                            |
| 7       | base W                            |
| 8       | mode (0 = ArtNet pass-through)    |
| 9       | parameter (unused when mode = 0)  |

`TupleSampler` returns `ColorTuple` with two RGBW colors indexed `[0]` and `[1]`. The existing `PaintDistributor` uses `[0]` for whichever surface `MSG_OVERRIDE_COLORS` targets (it sends surface=Base; see `PaintDistributor.cpp:99-101`). For ArtNet we send both surfaces in every frame: `t[0]` → shade, `t[1]` → base. This matches the implicit convention in the wisp's mesh path that `[0]` is the "primary" sampled color.

**Synthetic MAC per fixture:** `{0x02, 0x57, 0x49, 0x53, 0x50, i}` where `i ∈ [0, 7]`. The `0x02` first byte sets the locally-administered bit (avoids collision with any real OUI). Bytes 1..4 spell `WISP` (`0x57 0x49 0x53 0x50`). `i` is the fixture index.

---

## Task 1: Pure ArtNet frame builder (TDD, host-portable)

**Files:**
- Create: `software/wisp/src/artnet_frame.h`
- Create: `software/wisp/src/artnet_frame.cpp`
- Create: `software/wisp/test/test_artnet_frame/artnet_frame.cpp`

**Approach:** Build the 530-byte frame deterministically. Pure function so it lives in the host build. Follows the test pattern in `software/wisp/test/test_tuple_sampler/tuple_sampler.cpp` — the test redeclares the algorithm to keep it self-contained from `CurrentPalette` / Arduino includes, and the production `.cpp` is what actually links into the firmware.

- [ ] **Step 1.1: Write the failing test file**

Create `software/wisp/test/test_artnet_frame/artnet_frame.cpp`:

```cpp
// Native tests for the wisp ArtNet frame builder.
//
// Pins the on-the-wire layout that
// software/lamp-os/src/components/network/artnet.cpp decodes. If production
// drifts, these tests still express the spec.

#include <unity.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct RGBW {
  uint8_t r = 0, g = 0, b = 0, w = 0;
};

struct ColorTuple {
  uint8_t r[2] = {0, 0};
  uint8_t g[2] = {0, 0};
  uint8_t b[2] = {0, 0};
  uint8_t w[2] = {0, 0};
};

constexpr size_t kArtnetFrameSize = 530;
constexpr size_t kDmxStart = 18;
constexpr size_t kBytesPerFixture = 10;
constexpr size_t kNumFixtures = 8;

// Synthetic MAC byte 0..4 are fixed; byte 5 is the fixture index.
constexpr std::array<uint8_t, 5> kFixtureMacPrefix = {0x02, 0x57, 0x49,
                                                     0x53, 0x50};

// Tuple sampler stub: deterministic, returns fixture-index-derived colors.
ColorTuple sampleTupleForFixture(const std::vector<RGBW>& palette,
                                 uint8_t fixtureIndex) {
  ColorTuple t;
  if (palette.empty()) return t;
  const auto& base = palette[fixtureIndex % palette.size()];
  const auto& shade = palette[(fixtureIndex + 1) % palette.size()];
  t.r[0] = shade.r; t.g[0] = shade.g; t.b[0] = shade.b; t.w[0] = shade.w;
  t.r[1] = base.r;  t.g[1] = base.g;  t.b[1] = base.b;  t.w[1] = base.w;
  return t;
}

// Mirror of artnet_frame.cpp build logic.
// Returns bytes written, or 0 on insufficient buffer.
size_t buildFrame(const std::vector<RGBW>& palette,
                  uint8_t seq,
                  uint8_t* out, size_t outLen) {
  if (outLen < kArtnetFrameSize) return 0;
  std::memset(out, 0, kArtnetFrameSize);
  // ART_NET_ID
  const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', '\0'};
  std::memcpy(out, kId, 8);
  // OpCode 0x5000 (ART_DMX), little-endian
  out[8] = 0x00;
  out[9] = 0x50;
  // ProtVer 0x000E, big-endian
  out[10] = 0x00;
  out[11] = 0x0E;
  // Sequence
  out[12] = seq;
  // Physical
  out[13] = 0x00;
  // Universe 1, little-endian
  out[14] = 0x01;
  out[15] = 0x00;
  // Length 0x0200 = 512, big-endian
  out[16] = 0x02;
  out[17] = 0x00;
  // Fixtures
  for (uint8_t i = 0; i < kNumFixtures; ++i) {
    ColorTuple t = sampleTupleForFixture(palette, i);
    uint8_t* f = out + kDmxStart + (i * kBytesPerFixture);
    f[0] = t.r[0]; f[1] = t.g[0]; f[2] = t.b[0]; f[3] = t.w[0];
    f[4] = t.r[1]; f[5] = t.g[1]; f[6] = t.b[1]; f[7] = t.w[1];
    f[8] = 0;  // mode = pass-through
    f[9] = 0;  // parameter
  }
  return kArtnetFrameSize;
}

}  // namespace

void test_frame_size_is_530() {
  std::vector<RGBW> palette = {{255, 0, 0, 0}, {0, 255, 0, 0}};
  uint8_t buf[600] = {0};
  size_t n = buildFrame(palette, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_size_t(530u, n);
}

void test_frame_rejects_small_buffer() {
  std::vector<RGBW> palette = {{255, 0, 0, 0}};
  uint8_t buf[529] = {0};
  size_t n = buildFrame(palette, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_size_t(0u, n);
}

void test_header_is_artnet_dmx_universe_1() {
  std::vector<RGBW> palette = {{1, 2, 3, 4}};
  uint8_t buf[530] = {0};
  buildFrame(palette, 0x42, buf, sizeof(buf));
  // ID
  TEST_ASSERT_EQUAL_MEMORY("Art-Net\0", buf, 8);
  // OpCode ART_DMX 0x5000 LE
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[8]);
  TEST_ASSERT_EQUAL_UINT8(0x50, buf[9]);
  // ProtVer 0x000E BE
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[10]);
  TEST_ASSERT_EQUAL_UINT8(0x0E, buf[11]);
  // Sequence
  TEST_ASSERT_EQUAL_UINT8(0x42, buf[12]);
  // Physical
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[13]);
  // Universe 1 LE
  TEST_ASSERT_EQUAL_UINT8(0x01, buf[14]);
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[15]);
  // Length 512 BE
  TEST_ASSERT_EQUAL_UINT8(0x02, buf[16]);
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[17]);
}

void test_first_fixture_carries_shade_then_base() {
  // Two-color palette; fixture 0's tuple is {palette[1], palette[0]} per the
  // stub sampler (base = palette[0], shade = palette[1]).
  std::vector<RGBW> palette = {{10, 20, 30, 40}, {50, 60, 70, 80}};
  uint8_t buf[530] = {0};
  buildFrame(palette, 0, buf, sizeof(buf));
  // Fixture 0 starts at offset 18.
  // Shade (t[0]) = palette[1] = {50, 60, 70, 80}.
  TEST_ASSERT_EQUAL_UINT8(50, buf[18]);
  TEST_ASSERT_EQUAL_UINT8(60, buf[19]);
  TEST_ASSERT_EQUAL_UINT8(70, buf[20]);
  TEST_ASSERT_EQUAL_UINT8(80, buf[21]);
  // Base (t[1]) = palette[0] = {10, 20, 30, 40}.
  TEST_ASSERT_EQUAL_UINT8(10, buf[22]);
  TEST_ASSERT_EQUAL_UINT8(20, buf[23]);
  TEST_ASSERT_EQUAL_UINT8(30, buf[24]);
  TEST_ASSERT_EQUAL_UINT8(40, buf[25]);
  // Mode + parameter zeroed.
  TEST_ASSERT_EQUAL_UINT8(0, buf[26]);
  TEST_ASSERT_EQUAL_UINT8(0, buf[27]);
}

void test_unused_fixtures_zeroed_with_empty_palette() {
  std::vector<RGBW> palette;
  uint8_t buf[530];
  std::memset(buf, 0xFF, sizeof(buf));
  buildFrame(palette, 0, buf, sizeof(buf));
  // All 80 fixture bytes zero.
  for (size_t i = 18; i < 18 + 80; ++i) {
    TEST_ASSERT_EQUAL_UINT8(0, buf[i]);
  }
  // Padding past channel 80 also zero.
  for (size_t i = 18 + 80; i < 530; ++i) {
    TEST_ASSERT_EQUAL_UINT8(0, buf[i]);
  }
}

void test_eight_fixtures_emitted() {
  // Palette with 8 distinct red levels — easy to spot which fixture got which.
  std::vector<RGBW> palette;
  for (uint8_t i = 0; i < 8; ++i) palette.push_back({uint8_t(i * 16), 0, 0, 0});
  uint8_t buf[530] = {0};
  buildFrame(palette, 0, buf, sizeof(buf));
  // The sampler's shade for fixture i is palette[(i+1) % 8].r — verify each.
  for (uint8_t i = 0; i < 8; ++i) {
    size_t off = 18 + (i * 10);
    uint8_t expectedShadeR = ((i + 1) % 8) * 16;
    TEST_ASSERT_EQUAL_UINT8(expectedShadeR, buf[off + 0]);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_frame_size_is_530);
  RUN_TEST(test_frame_rejects_small_buffer);
  RUN_TEST(test_header_is_artnet_dmx_universe_1);
  RUN_TEST(test_first_fixture_carries_shade_then_base);
  RUN_TEST(test_unused_fixtures_zeroed_with_empty_palette);
  RUN_TEST(test_eight_fixtures_emitted);
  return UNITY_END();
}
```

- [ ] **Step 1.2: Run test to verify it fails (compile-only check first)**

Run: `cd software/wisp && pio test -e native -f test_artnet_frame`
Expected: tests pass (the test redeclares the algorithm self-contained, so it passes immediately — this verifies the *test* is correct before we write production code).

If output shows compile errors, fix the test first.

- [ ] **Step 1.3: Write production header `software/wisp/src/artnet_frame.h`**

```cpp
// artnet_frame — pure, host-portable ArtNet DMX frame builder for the wisp
// backwards-compat ArtNet bridge. Produces the exact 530-byte wire format
// that software/lamp-os/src/components/network/artnet.cpp decodes.
//
// No Arduino, no FreeRTOS. Caller owns the buffer; we just fill it in.

#pragma once

#include <cstddef>
#include <cstdint>

#include "CurrentPalette.h"

namespace wisp {

// Total bytes the lamp listener requires (it rejects anything else).
constexpr size_t kArtnetFrameSize = 530;

// Number of 10-channel fixtures we emit into universe 1. Matches the lamp
// firmware's lampNumber range [0, 7].
constexpr size_t kArtnetNumFixtures = 8;

// Fill `out` with one ArtNet DMX universe-1 frame for the current palette.
// Each fixture's two surfaces (shade, base) get a stable TupleSampler-derived
// pair sampled at a synthetic per-fixture MAC. Returns kArtnetFrameSize on
// success, 0 if `outLen < kArtnetFrameSize`.
//
// `seq` is the rolling sequence counter the caller maintains (0..255). Lamps
// don't strictly require it but ArtNet senders are expected to.
size_t buildArtnetDmxFrame(const CurrentPalette& palette, uint8_t seq,
                           uint8_t* out, size_t outLen);

}  // namespace wisp
```

- [ ] **Step 1.4: Write production `.cpp`**

Create `software/wisp/src/artnet_frame.cpp`:

```cpp
#include "artnet_frame.h"

#include <cstring>

#include "TupleSampler.h"

namespace wisp {

namespace {
constexpr size_t kDmxStart = 18;
constexpr size_t kBytesPerFixture = 10;
}  // namespace

size_t buildArtnetDmxFrame(const CurrentPalette& palette, uint8_t seq,
                           uint8_t* out, size_t outLen) {
  if (outLen < kArtnetFrameSize) return 0;
  std::memset(out, 0, kArtnetFrameSize);

  // ART_NET_ID: 8 bytes including trailing NUL.
  static const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', '\0'};
  std::memcpy(out, kId, 8);

  // OpCode ART_DMX = 0x5000, little-endian.
  out[8] = 0x00;
  out[9] = 0x50;

  // ProtVer = 0x000E, big-endian. Lamp listener ignores; mirror what
  // Gen 1 senders typically emit.
  out[10] = 0x00;
  out[11] = 0x0E;

  out[12] = seq;
  out[13] = 0x00;  // physical

  // Universe 1, little-endian.
  out[14] = 0x01;
  out[15] = 0x00;

  // DMX payload length = 512, big-endian (matches lamp decode at
  // artnet.cpp:54: dmxDataLength = artnetPacket[17] | artnetPacket[16] << 8).
  out[16] = 0x02;
  out[17] = 0x00;

  for (uint8_t i = 0; i < kArtnetNumFixtures; ++i) {
    // Synthetic MAC: 02:57:49:53:50:<fixture>. The 0x02 sets the
    // locally-administered bit so we can't collide with a real OUI.
    // 0x57 0x49 0x53 0x50 = "WISP".
    uint8_t mac[6] = {0x02, 0x57, 0x49, 0x53, 0x50, i};
    ColorTuple t = sampleTupleForMac(palette, mac);
    uint8_t* f = out + kDmxStart + (i * kBytesPerFixture);
    // shade = tuple[0], base = tuple[1] (mirrors the "primary sampled
    // color = [0]" convention in PaintDistributor.cpp).
    f[0] = t.r[0]; f[1] = t.g[0]; f[2] = t.b[0]; f[3] = t.w[0];
    f[4] = t.r[1]; f[5] = t.g[1]; f[6] = t.b[1]; f[7] = t.w[1];
    f[8] = 0;  // mode = ArtNet pass-through
    f[9] = 0;  // parameter (unused when mode = 0)
  }

  return kArtnetFrameSize;
}

}  // namespace wisp
```

- [ ] **Step 1.5: Confirm native suite still green**

Run: `cd software/wisp && pio test -e native`
Expected: all existing tests still pass, plus `test_artnet_frame` passes (`6/6` new tests).

- [ ] **Step 1.6: Commit**

```bash
git add software/wisp/src/artnet_frame.h software/wisp/src/artnet_frame.cpp \
        software/wisp/test/test_artnet_frame/artnet_frame.cpp
git commit -m "$(cat <<'EOF'
feat(wisp): pure ArtNet DMX frame builder

Builds the 530-byte universe-1 frame the old ArtNet-only lamp listener
(software/lamp-os/src/components/network/artnet.cpp) expects: 8 fixtures
× 10 channels (shade RGBW + base RGBW + mode + param). Per-fixture
colors via TupleSampler with synthetic stable MACs so the gradient
mapping matches the mesh path. Pure C++, no Arduino, covered by native
Unity tests.
EOF
)"
```

---

## Task 2: WifiLink — STA join with NVS-persisted credentials

**Files:**
- Create: `software/wisp/src/WifiLink.h`
- Create: `software/wisp/src/WifiLink.cpp`

**Why:** `main.cpp:172-176` has a TODO acknowledging WiFi STA isn't wired up. ArtNet emit and (downstream) Aurora streaming both need it. We persist creds in NVS so the wisp reconnects on boot without user intervention.

- [ ] **Step 2.1: Write `software/wisp/src/WifiLink.h`**

```cpp
// WifiLink — STA-mode bring-up with NVS-persisted credentials.
//
// Owns nothing exotic: just the small subset of WiFi state the wisp needs
// (current SSID, connection state) plus a Preferences-backed store so the
// last-good creds survive reboots.
//
// Coex caveat (read once): MeshLink::begin() calls esp_wifi_set_channel(1)
// before init'ing ESP-NOW. Once we associate to an AP here, the radio
// switches to whatever channel the AP picked. Mesh peers pinned to channel
// 1 won't see broadcasts unless the venue AP is also on channel 1. See
// docs/mesh-deployment.md "channel coex" — same constraint Aurora faces.

#pragma once

#include <Arduino.h>

#include <string>

namespace wisp {

class WifiLink {
 public:
  // Load any persisted creds and, if both are present, kick a WiFi.begin().
  // Idempotent — safe to call once during setup().
  void begin();

  // Persist new creds to NVS and (re)connect. Either argument may be empty
  // to clear that field; if both are non-empty, association is attempted.
  void setCredentials(const std::string& ssid, const std::string& password);

  // Snapshot accessors. Cheap. Callable from any task.
  bool isConnected() const;
  const std::string& ssid() const { return ssid_; }
  const std::string& password() const { return password_; }

 private:
  void connectIfReady();
  void loadFromNvs();
  void saveToNvs();

  std::string ssid_;
  std::string password_;
  bool started_ = false;
};

}  // namespace wisp
```

- [ ] **Step 2.2: Write `software/wisp/src/WifiLink.cpp`**

```cpp
#include "WifiLink.h"

#include <Preferences.h>
#include <WiFi.h>

namespace wisp {

namespace {
constexpr const char* kNvsNamespace = "wisp_wifi";
constexpr const char* kKeySsid = "ssid";
constexpr const char* kKeyPass = "pass";
}  // namespace

void WifiLink::begin() {
  if (started_) return;
  started_ = true;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t /*info*/) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("[wifi] got IP: %s\n",
                    WiFi.localIP().toString().c_str());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.println("[wifi] disconnected");
    }
  });

  loadFromNvs();
  connectIfReady();
}

void WifiLink::setCredentials(const std::string& ssid,
                              const std::string& password) {
  ssid_ = ssid;
  password_ = password;
  saveToNvs();
  // Drop any existing association so the new creds take effect.
  WiFi.disconnect(false, false);
  connectIfReady();
}

bool WifiLink::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

void WifiLink::connectIfReady() {
  if (ssid_.empty() || password_.empty()) {
    Serial.println("[wifi] no creds; staying offline");
    return;
  }
  Serial.printf("[wifi] connecting to '%s'\n", ssid_.c_str());
  WiFi.begin(ssid_.c_str(), password_.c_str());
}

void WifiLink::loadFromNvs() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
    return;
  }
  ssid_ = prefs.getString(kKeySsid, "").c_str();
  password_ = prefs.getString(kKeyPass, "").c_str();
  prefs.end();
}

void WifiLink::saveToNvs() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    Serial.println("[wifi] NVS begin failed; creds not persisted");
    return;
  }
  prefs.putString(kKeySsid, ssid_.c_str());
  prefs.putString(kKeyPass, password_.c_str());
  prefs.end();
}

}  // namespace wisp
```

- [ ] **Step 2.3: Build firmware to verify the file compiles**

Run: `cd software/wisp && pio run -e seeed_xiao_esp32_c6 -t checkprogsize`

(This only checks compile + link; we wire it up in Task 5.)

Expected: clean build. If link errors mention undefined `WifiLink` symbols, that's expected because main.cpp doesn't reference it yet — proceed.

To force-include for build verification, temporarily add `#include "WifiLink.h"` to a file that's already included. **Easier:** just verify compilation directly:

Run: `cd software/wisp && pio run -e seeed_xiao_esp32_c6 --target compiledb && grep -c "WifiLink.cpp" .pio/build/seeed_xiao_esp32_c6/compile_commands.json || true`

If `WifiLink.cpp` isn't compiled (because nothing references it yet), defer verification to Task 5 — we'll catch any errors when main.cpp includes it.

- [ ] **Step 2.4: Commit**

```bash
git add software/wisp/src/WifiLink.h software/wisp/src/WifiLink.cpp
git commit -m "$(cat <<'EOF'
feat(wisp): WifiLink STA-mode bring-up with NVS creds

Replaces the Phase D TODO in main.cpp for WiFi station-mode wiring.
Credentials persist via Preferences ("wisp_wifi" namespace) so the
wisp reconnects on reboot. Coex with ESP-NOW channel pinning is
unchanged from Aurora's existing behavior — see docs/mesh-deployment.md.
EOF
)"
```

---

## Task 3: StageBeacon — BLE manufacturer-data advert (old-lamp auto-join)

**Files:**
- Create: `software/wisp/src/StageBeacon.h`
- Create: `software/wisp/src/StageBeacon.cpp`

**Why:** Old lamp firmware at `software/lamp-os/src/components/network/bluetooth.cpp:39-72` scans for a BLE advertisement with manufacturer ID `42007` (`BLE_STAGE_MAGIC_NUMBER`) and uses the embedded SSID/password to join WiFi. Without this, old lamps would need manual WiFi provisioning. Lifting the Gen 1 repeater's advert format makes them auto-join the wisp's WiFi.

**Reference (do not deviate):** `software/artnet-repeater/src/main.cpp:42-56`. Magic ID `42007`, non-connectable, scan-response enabled, mfg data layout `<2 bytes magic LE><ssid\0><password\0>` (max 28 bytes total).

- [ ] **Step 3.1: Write `software/wisp/src/StageBeacon.h`**

```cpp
// StageBeacon — non-connectable BLE advertisement carrying WiFi creds for
// pre-mesh lamps to discover and auto-join.
//
// Lifted byte-for-byte from software/artnet-repeater/src/main.cpp:42-56,
// which is what the lamp-side scanner at
// software/lamp-os/src/components/network/bluetooth.cpp:39-72 expects.
//
// Magic ID 42007. Payload: <2 bytes magic LE><ssid\0><password\0>.
// Max combined ssid+password length (incl. NULs) is 26 bytes — caller
// must enforce or we silently truncate to 28 total advert bytes.

#pragma once

#include <string>

namespace wisp {

class StageBeacon {
 public:
  // Initialize NimBLE if not already up. Idempotent.
  void begin(const std::string& deviceName);

  // Start advertising with the given creds. Replaces any in-flight advert.
  // No-op (and stops advertising) if either string is empty.
  void advertise(const std::string& ssid, const std::string& password);

  // Stop advertising. Safe to call before begin().
  void stop();

  bool isAdvertising() const { return advertising_; }

 private:
  bool initialized_ = false;
  bool advertising_ = false;
  std::string deviceName_;
};

}  // namespace wisp
```

- [ ] **Step 3.2: Write `software/wisp/src/StageBeacon.cpp`**

```cpp
#include "StageBeacon.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <vector>

namespace wisp {

namespace {
constexpr uint16_t kStageMagic = 42007;
constexpr size_t kMaxAdvertBytes = 28;  // matches Gen 1 repeater
}  // namespace

void StageBeacon::begin(const std::string& deviceName) {
  if (initialized_) return;
  deviceName_ = deviceName;
  NimBLEDevice::init(deviceName_);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, NimBLETxPowerType::Advertise);
  initialized_ = true;
}

void StageBeacon::advertise(const std::string& ssid,
                            const std::string& password) {
  if (!initialized_) {
    Serial.println("[stage] advertise() before begin(); ignoring");
    return;
  }
  if (ssid.empty() || password.empty()) {
    stop();
    return;
  }

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  adv->setName(deviceName_);
  adv->enableScanResponse(true);
  adv->setConnectableMode(0);  // non-connectable
  adv->setMinInterval(650);
  adv->setMaxInterval(800);

  std::vector<uint8_t> data;
  data.reserve(kMaxAdvertBytes);
  // Manufacturer ID (LE)
  data.push_back(uint8_t(kStageMagic & 0xff));
  data.push_back(uint8_t((kStageMagic >> 8) & 0xff));
  // SSID + NUL
  for (char c : ssid) data.push_back(uint8_t(c));
  data.push_back(0);
  // Password + NUL
  for (char c : password) data.push_back(uint8_t(c));
  data.push_back(0);
  // Truncate defensively — lamp scanner only reads up to 28 bytes anyway.
  if (data.size() > kMaxAdvertBytes) {
    Serial.printf("[stage] truncating mfg data %u → %u bytes\n",
                  (unsigned)data.size(), (unsigned)kMaxAdvertBytes);
    data.resize(kMaxAdvertBytes);
  }
  // NimBLE expects std::vector<unsigned char>
  std::vector<unsigned char> mfg(data.begin(), data.end());
  adv->setManufacturerData(mfg);

  adv->start();
  advertising_ = true;
  Serial.printf("[stage] advertising ssid='%s' (%u byte payload)\n",
                ssid.c_str(), (unsigned)data.size());
}

void StageBeacon::stop() {
  if (!initialized_) return;
  NimBLEDevice::getAdvertising()->stop();
  advertising_ = false;
}

}  // namespace wisp
```

- [ ] **Step 3.3: Commit**

```bash
git add software/wisp/src/StageBeacon.h software/wisp/src/StageBeacon.cpp
git commit -m "$(cat <<'EOF'
feat(wisp): StageBeacon — BLE WiFi-cred advert for old-lamp auto-join

Mirrors software/artnet-repeater/src/main.cpp:42-56 byte-for-byte so
pre-mesh lamps running software/lamp-os firmware will discover the
wisp's WiFi via the manufacturer-data scan they already do
(software/lamp-os/src/components/network/bluetooth.cpp:39-72). First
NimBLE consumer on the wisp.
EOF
)"
```

---

## Task 4: ArtnetEmitter — UDP broadcast wrapper

**Files:**
- Create: `software/wisp/src/ArtnetEmitter.h`
- Create: `software/wisp/src/ArtnetEmitter.cpp`

- [ ] **Step 4.1: Write `software/wisp/src/ArtnetEmitter.h`**

```cpp
// ArtnetEmitter — broadcasts ArtNet DMX universe-1 frames over UDP/6454
// so pre-mesh lamps running software/lamp-os firmware can be painted by
// the same Aurora palette that drives the v0x03 mesh.
//
// Mirrors PaintDistributor's lifecycle: enabled/disabled gate + palette
// change kick + low-rate backstop refresh. Pure frame construction lives
// in artnet_frame.{h,cpp}; this class just owns the AsyncUDP socket and
// decides when to emit.

#pragma once

#include <AsyncUDP.h>

#include <cstdint>

namespace wisp {

class CurrentPalette;
class WifiLink;

class ArtnetEmitter {
 public:
  void begin(CurrentPalette* palette, WifiLink* wifi);

  // External enable/disable — wired to a serial command and (later) the
  // app pane. Defaults to disabled so the wisp doesn't shout into a
  // venue's LAN unsolicited.
  void setEnabled(bool on);
  bool enabled() const { return enabled_; }

  // Called from the Aurora palette callback in main.cpp. Kicks an
  // immediate emit if enabled + connected.
  void onPaletteChanged();

  // Periodic tick from loop(). Pushes a backstop refresh frame every
  // kBackstopMs while enabled, so a lamp that booted between palette
  // changes still picks up the current colors.
  void tick(uint32_t nowMs);

 private:
  void emitNow();

  static constexpr uint16_t kArtnetPort = 6454;
  static constexpr uint32_t kBackstopMs = 1000;

  CurrentPalette* palette_ = nullptr;
  WifiLink* wifi_ = nullptr;
  AsyncUDP udp_;
  bool udpReady_ = false;
  bool enabled_ = false;
  uint8_t seq_ = 0;
  uint32_t lastEmitMs_ = 0;
};

}  // namespace wisp
```

- [ ] **Step 4.2: Write `software/wisp/src/ArtnetEmitter.cpp`**

```cpp
#include "ArtnetEmitter.h"

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFi.h>

#include "CurrentPalette.h"
#include "WifiLink.h"
#include "artnet_frame.h"

namespace wisp {

void ArtnetEmitter::begin(CurrentPalette* palette, WifiLink* wifi) {
  palette_ = palette;
  wifi_ = wifi;
  // UDP socket is opened lazily on first emit; until then we don't know
  // if WiFi is connected.
}

void ArtnetEmitter::setEnabled(bool on) {
  enabled_ = on;
  if (on) {
    lastEmitMs_ = 0;  // force first emit on next tick
    Serial.println("[artnet] enabled");
  } else {
    Serial.println("[artnet] disabled");
  }
}

void ArtnetEmitter::onPaletteChanged() {
  if (!enabled_) return;
  emitNow();
}

void ArtnetEmitter::tick(uint32_t nowMs) {
  if (!enabled_) return;
  if (nowMs - lastEmitMs_ < kBackstopMs) return;
  emitNow();
}

void ArtnetEmitter::emitNow() {
  if (!palette_ || !wifi_) return;
  if (!wifi_->isConnected()) return;

  if (!udpReady_) {
    // AsyncUDP::connect for outgoing only; no listen needed.
    // We use writeTo() with broadcast addr per call.
    udpReady_ = true;
    Serial.println("[artnet] udp ready");
  }

  uint8_t buf[kArtnetFrameSize];
  size_t n = buildArtnetDmxFrame(*palette_, seq_++, buf, sizeof(buf));
  if (n != kArtnetFrameSize) {
    Serial.println("[artnet] frame build failed");
    return;
  }

  IPAddress bcast(255, 255, 255, 255);
  size_t sent = udp_.writeTo(buf, n, bcast, kArtnetPort);
  lastEmitMs_ = millis();
  if (sent != n) {
    Serial.printf("[artnet] short write %u/%u\n",
                  (unsigned)sent, (unsigned)n);
  }
}

}  // namespace wisp
```

- [ ] **Step 4.3: Commit**

```bash
git add software/wisp/src/ArtnetEmitter.h software/wisp/src/ArtnetEmitter.cpp
git commit -m "$(cat <<'EOF'
feat(wisp): ArtnetEmitter — UDP broadcast for pre-mesh lamps

Broadcasts the universe-1 DMX frame built by artnet_frame.cpp to
255.255.255.255:6454 on palette change + a 1 s backstop. Gated behind
setEnabled() so the wisp doesn't shout unsolicited into a venue LAN.
Skips emit when WiFi STA is not associated.
EOF
)"
```

---

## Task 5: Wire it all into main.cpp + extend serial commands

**Files:**
- Modify: `software/wisp/src/main.cpp`

**What we add to setup():**
- WifiLink begin
- StageBeacon begin (no advertise yet — that's gated)
- ArtnetEmitter begin

**What we add to loop():**
- `artnetEmitter.tick(now)`

**What we add to `onAuroraPalette()`:**
- `artnetEmitter.onPaletteChanged()` after `paintDistributor.onPaletteChanged()`.

**What we add to `processSerialCommand()`:**
- `wifi:set <ssid> <pass>` — call `wifi.setCredentials(...)`. If stage beacon was on, kick a re-advertise with new creds.
- `wifi:show` — print state.
- `artnet:on` / `artnet:off` — toggle the ArtNet emitter.
- `stage:on` / `stage:off` — toggle the BLE WiFi-cred beacon.

- [ ] **Step 5.1: Add includes + globals**

Edit `software/wisp/src/main.cpp:12-31`. Add includes after the existing `#include` block. `<WiFi.h>` is needed for `WiFi.localIP()` in the `wifi:show` command — main.cpp does not currently include it directly:

```cpp
#include <WiFi.h>

#include "ArtnetEmitter.h"
#include "StageBeacon.h"
#include "WifiLink.h"
```

Add to the anonymous namespace globals (after `AuroraPaletteClient auroraClient;`):

```cpp
wisp::WifiLink wifi;
wisp::StageBeacon stageBeacon;
wisp::ArtnetEmitter artnetEmitter;
```

- [ ] **Step 5.2: Extend setup()**

In `software/wisp/src/main.cpp:160-192`, after the `mesh.onPacket` / `mesh.begin` block and before the Aurora client init, add:

```cpp
  wifi.begin();
  stageBeacon.begin(buildInstanceId().c_str());
  artnetEmitter.begin(&currentPalette, &wifi);
```

Remove the obsolete `// TODO Phase D: WiFi credentials...` block (the four commented-out lines starting at `main.cpp:172`).

Update the cmds banner at the end of setup() (the `Serial.println("[wisp] cmds: paint:on / paint:off");` line) to:

```cpp
  Serial.println("[wisp] cmds: paint:on/off  artnet:on/off  stage:on/off");
  Serial.println("[wisp] cmds: wifi:set <ssid> <pass>  wifi:show");
```

- [ ] **Step 5.3: Extend `onAuroraPalette()`**

In `software/wisp/src/main.cpp:79`, after `paintDistributor.onPaletteChanged();`, add:

```cpp
  artnetEmitter.onPaletteChanged();
```

- [ ] **Step 5.4: Extend `loop()`**

In `software/wisp/src/main.cpp:201`, after `paintDistributor.tick(now);`, add:

```cpp
  artnetEmitter.tick(now);
```

- [ ] **Step 5.5: Replace `processSerialCommand()`**

Replace the entire body of `processSerialCommand` (currently `main.cpp:84-95`) with:

```cpp
void processSerialCommand(const String& cmd) {
  if (cmd.length() == 0) return;
  if (cmd == "paint:on") {
    paintDistributor.setPaintMode(true);
    Serial.println("[wisp.cmd] paint mode ON");
  } else if (cmd == "paint:off") {
    paintDistributor.setPaintMode(false);
    Serial.println("[wisp.cmd] paint mode OFF");
  } else if (cmd == "artnet:on") {
    artnetEmitter.setEnabled(true);
  } else if (cmd == "artnet:off") {
    artnetEmitter.setEnabled(false);
  } else if (cmd == "stage:on") {
    stageBeacon.advertise(wifi.ssid(), wifi.password());
  } else if (cmd == "stage:off") {
    stageBeacon.stop();
  } else if (cmd == "wifi:show") {
    Serial.printf("[wifi] ssid='%s' connected=%d ip=%s\n",
                  wifi.ssid().c_str(), wifi.isConnected() ? 1 : 0,
                  WiFi.localIP().toString().c_str());
  } else if (cmd.startsWith("wifi:set ")) {
    // Format: "wifi:set <ssid> <pass>" — split on first space after prefix.
    int sp = cmd.indexOf(' ', 9);
    if (sp < 0 || sp == 9 || sp == (int)cmd.length() - 1) {
      Serial.println("[wisp.cmd] usage: wifi:set <ssid> <pass>");
      return;
    }
    String ssid = cmd.substring(9, sp);
    String pass = cmd.substring(sp + 1);
    wifi.setCredentials(std::string(ssid.c_str()),
                        std::string(pass.c_str()));
    Serial.println("[wisp.cmd] wifi creds saved");
    if (stageBeacon.isAdvertising()) {
      stageBeacon.advertise(wifi.ssid(), wifi.password());
    }
  } else {
    Serial.printf("[wisp.cmd] unknown command: %s\n", cmd.c_str());
  }
}
```

- [ ] **Step 5.6: Build firmware**

Run: `cd software/wisp && pio run -e seeed_xiao_esp32_c6`

Expected: clean build. If link errors mention `AsyncUDP`, verify it's coming from the ESP32 core (it should — the lamp firmware uses it via `software/lamp-os/src/components/network/artnet.cpp`).

- [ ] **Step 5.7: Verify native suite still green**

Run: `cd software/wisp && pio test -e native`

Expected: all tests pass. We have not modified any host-portable code in this task.

- [ ] **Step 5.8: Commit**

```bash
git add software/wisp/src/main.cpp
git commit -m "$(cat <<'EOF'
feat(wisp): wire WifiLink + StageBeacon + ArtnetEmitter into main

Replaces the Phase D WiFi-STA TODO with a real WifiLink instance and
adds serial commands to configure creds, toggle the BLE WiFi-cred
beacon, and toggle ArtNet emit independently of mesh paint mode.
Aurora palette changes now kick both the mesh PaintDistributor and the
ArtnetEmitter.
EOF
)"
```

---

## Task 6: Update platformio.ini native exclusions

**Files:**
- Modify: `software/wisp/platformio.ini`

**Why:** The new Arduino-coupled `.cpp` files (`ArtnetEmitter`, `WifiLink`, `StageBeacon`) must be excluded from the `[env:native]` build because they include `<Arduino.h>`, `<WiFi.h>`, `<NimBLEDevice.h>`, etc. `artnet_frame.cpp` is pure and stays included (though the test redeclares the algorithm per house style).

- [ ] **Step 6.1: Edit `software/wisp/platformio.ini`**

Find the `[env:native]` `build_src_filter` block (around the section that lists `-<main.cpp>`, `-<MeshLink.cpp>`, etc) and add three lines so the final block reads:

```ini
build_src_filter =
	+<*>
	-<main.cpp>
	-<MeshLink.cpp>
	-<LampInventory.cpp>
	-<CurrentPalette.cpp>
	-<PaintDistributor.cpp>
	-<StatusBeacon.cpp>
	-<TupleSampler.cpp>
	-<aurora/AuroraDiscovery.cpp>
	-<aurora/AuroraWsConnection.cpp>
	-<aurora/PaletteFetcher.cpp>
	-<aurora/AuroraPaletteClient.cpp>
	-<ArtnetEmitter.cpp>
	-<WifiLink.cpp>
	-<StageBeacon.cpp>
```

- [ ] **Step 6.2: Verify native suite green**

Run: `cd software/wisp && pio test -e native`

Expected: all existing tests pass + `test_artnet_frame` passes. No compile errors mentioning `WiFi.h`, `NimBLEDevice.h`, or `AsyncUDP.h`.

- [ ] **Step 6.3: Commit**

```bash
git add software/wisp/platformio.ini
git commit -m "$(cat <<'EOF'
build(wisp): exclude new Arduino-coupled sources from native env

ArtnetEmitter, WifiLink, and StageBeacon include WiFi / NimBLE /
AsyncUDP headers and don't belong in the host Unity build. artnet_frame
stays included — it's pure C++.
EOF
)"
```

---

## Task 7: Documentation refresh

**Files:**
- Modify: `docs/mesh-deployment.md`

**Why:** The deployment doc is the operational reference. The new BLE-beacon + ArtNet emit changes how operators bring the wisp online when pre-mesh lamps are in the fleet. Add a short subsection rather than rewriting the doc.

- [ ] **Step 7.1: Add an "ArtNet bridge" subsection**

Open `docs/mesh-deployment.md`. Find a sensible insertion point near the existing deployment checklist (look for an existing top-level heading like "Deployment checklist" or "Troubleshooting playbook"). Insert before the troubleshooting section:

```markdown
## ArtNet bridge (pre-mesh lamp backwards compat)

The wisp can broadcast ArtNet DMX universe 1 in parallel with the v0x03
mesh, so pre-mesh lamps still running the `software/lamp-os` ArtNet
firmware can be painted by the same Aurora palette.

**Wire-up at the venue:**

1. Set the wisp's WiFi creds via serial: `wifi:set <ssid> <pass>`. These
   persist in NVS.
2. `stage:on` to start advertising the BLE manufacturer-data beacon
   (magic ID `42007`, payload `<ssid>\0<password>\0`). Pre-mesh lamps
   scan for this and auto-join the same WiFi.
3. `artnet:on` to start broadcasting ArtNet to `255.255.255.255:6454`.
   One frame per Aurora palette change plus a 1 s backstop.

**Coex caveat:** ESP-NOW remains pinned to channel 1 at boot, but
associating to the venue AP re-tunes the radio to whatever channel the
AP advertises. Mesh reliability degrades the further the AP sits from
channel 1. This is the same constraint Aurora already imposes; the
ArtNet path piggybacks the existing trade-off, it does not introduce a
new one.

**Wire format produced** — see `software/wisp/src/artnet_frame.h`:
universe 1, 8 fixtures × 10 channels (shade RGBW, base RGBW, mode byte,
parameter byte). Lamps self-assign their fixture index randomly at
boot (`software/lamp-os/src/components/network/artnet.cpp:46`); we
broadcast all 8 slots and let them draw straws.
```

- [ ] **Step 7.2: Commit**

```bash
git add docs/mesh-deployment.md
git commit -m "$(cat <<'EOF'
docs(mesh): document wisp ArtNet bridge bring-up

Operational steps for using the wisp as a drop-in replacement for the
old artnet-repeater: wifi:set creds, stage:on for BLE auto-provisioning,
artnet:on for emit. Reiterates the coex constraint already documented
upstream so operators don't think the ArtNet path is new pain.
EOF
)"
```

---

## Task 8: Hardware verification (manual)

**Not a code task — must be done on real hardware.** Document outcomes in the PR description when promoting.

- [ ] **Step 8.1: Flash the wisp**

Run: `cd software/wisp && pio run -e seeed_xiao_esp32_c6 -t upload && pio device monitor`

Expected on boot:
```
wisp: phase B boot
[mesh] ready ch=1 mac=...
[wifi] no creds; staying offline       ← if NVS is empty
[wisp] aurora client started as wisp-XXXXXX
[wisp] paint distributor + status beacon online
[wisp] cmds: paint:on/off  artnet:on/off  stage:on/off
[wisp] cmds: wifi:set <ssid> <pass>  wifi:show
```

- [ ] **Step 8.2: Set WiFi creds and verify join**

In the serial monitor, type:
```
wifi:set MyVenueAP myvenuepass
```

Expected:
```
[wisp.cmd] wifi creds saved
[wifi] connecting to 'MyVenueAP'
[wifi] got IP: 192.168.1.42
```

- [ ] **Step 8.3: Verify stage beacon visibility**

Type `stage:on`. From a laptop, run `sudo hcitool lescan --duplicates | head -20` (Linux) or use a BLE scanner app on phone. Look for an advertisement matching the wisp's instance name (`wisp-XXXXXX`) carrying 28 bytes of manufacturer data starting with `17 a4` (= 42007 LE).

- [ ] **Step 8.4: Verify ArtNet frames on the wire**

On a laptop joined to the same WiFi, run `sudo tcpdump -i <iface> -nn 'udp port 6454' -X | head -40`. Type `artnet:on` in the wisp serial. Expected: 530-byte packets every ~1 s with `00000000  41 72 74 2d 4e 65 74 00  00 50 00 0e ...` header.

- [ ] **Step 8.5: Verify a pre-mesh lamp paints correctly**

Power on a lamp flashed with the `software/lamp-os` ArtNet-only firmware in range. It should join the wisp's WiFi via the BLE beacon, then start receiving ArtNet within seconds. Verify its colors track the active Aurora palette.

- [ ] **Step 8.6: Verify v0x03 mesh paint still works**

In the same room, power on a v0x03-mesh lamp. Type `paint:on` in the wisp serial. Verify the mesh lamp also paints from the same palette. If mesh paint is unreliable, check the venue AP's channel — if it's not channel 1, that's the expected coex degradation, not a regression in this PR.

- [ ] **Step 8.7: Coex sanity check**

Leave both paths running for 5 minutes. Verify:
- Status beacon still emits (`MSG_WISP_HELLO` every 2 s — visible in any lamp's serial log).
- ArtNet frames still emit (`tcpdump` count grows ~1/sec).
- Mesh lamp keeps the painted color.

---

## Self-review checklist (run before declaring done)

- [ ] `pio test -e native` green (existing + new `test_artnet_frame`).
- [ ] `pio run -e seeed_xiao_esp32_c6` clean (no warnings beyond baseline).
- [ ] No new lib_deps added to `platformio.ini` (only the native exclusion list).
- [ ] `artnet_frame.h/cpp` compiles in the host build (no Arduino includes).
- [ ] All four new classes used from `main.cpp` (no dead code).
- [ ] Serial command parser doesn't crash on `wifi:set` with missing args.
- [ ] BLE advert is exactly the Gen 1 payload format (magic 42007 LE, `<ssid>\0<password>\0`).
- [ ] ArtNet frame is exactly 530 bytes (test pins this).
- [ ] Universe 1, OpCode 0x5000, sequence counter rolls 0..255 (test pins this).
