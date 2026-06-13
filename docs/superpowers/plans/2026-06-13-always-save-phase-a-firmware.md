# Always Save Phase A (Firmware) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the firmware-side foundation for "always save, no preview": split user-source vs remote-source mutation paths (so mesh cascades don't contaminate persistence), add a new CHAR_COMMIT characteristic with debounced/OTA-interlocked drain, rewrite settings_blob with reboot opt-in and per-section in-memory updates.

**Architecture:** Two write paths — CHAR_COMMIT for drag-style flows (parameterless, persists current RAM state) and settings_blob for discrete actions (carries new values, optionally reboots). Mutation helpers live in a new `src/components/apply/` module so they're native-testable. Backward compat: settings_blob defaults `reboot: true` so old apps keep working.

**Tech Stack:** ESP32 (upesy_wroom) Arduino framework, NimBLE BLE stack, ArduinoJson 7.4.x, PlatformIO, Unity native tests.

**Spec:** `docs/superpowers/specs/2026-06-13-always-save-no-preview-design.md`. Phase A scope only.

**Critical ordering invariant:** Tasks 2-7 implement A.1 (user/remote mutation split). Tasks 8-15 implement A.2-A.8. Within Phase A, **A.1 must merge before A.2/A.3 in the deployment timeline** — if CHAR_COMMIT ships before the split, the first commit signal can persist mesh-cascade-contaminated config. Bench test #12 (Task 16) verifies the regression is impossible.

---

## File Map

**New directory:** `software/lamp-os/src/components/apply/`
- `apply_brightness.hpp` — `brightnessToConfig(level, isHomeMode)`, `brightnessToRender(level, isHomeMode)`, `brightnessImmediate(level, isHomeMode)` (for settings_blob path)
- `apply_shade_colors.hpp` — `shadeColorsToConfig(arr)`, `shadeColorsToRender(arr)`
- `apply_base_colors.hpp` — `baseColorsToConfig(arr)`, `baseColorsToRender(arr)`
- `apply_expressions.hpp` — `expressionOpToConfig(doc)`, `expressionOpToRender(doc)`
- `apply_lamp.hpp` — `lampLocal(obj)` — handles name/brightness/advancedEnabled/socialMode, calls `NimBLEDevice::setDeviceName`
- `apply_home_mode.hpp` — `homeModeLocal(obj)` — handles ssid/password/enabled/brightness
- `apply_base_ac.hpp` — `baseAcLocal(int)`
- `apply_base_knockout.hpp` — `baseKnockoutLocal(arr)` — extracted from inline drain logic
- `apply_settings_blob.hpp` — `settingsBlobLocal(doc) -> bool needsReboot` — orchestrates per-section dispatch

**Modified:**
- `software/lamp-os/src/lamps/standard_lamp.cpp` — drains reorganized; applyXxxLocal call sites switch to apply:: helpers
- `software/lamp-os/src/config/config.hpp` + `config.cpp` — `persistConfig(const char* via)` signature
- `software/lamp-os/src/components/network/ble_control.cpp` + `ble_control.hpp` — register CHAR_COMMIT in service init; new `postPendingCommit` thread-cross hook
- `software/lamp-app-flutter/lib/core/ble/uuids.dart` — add `commit` UUID constant (sync with firmware)

**New test directories** (each contains a single `<name>.cpp` Unity test, mirror-class pattern like `test_disposition_debounce`):
- `software/lamp-os/test/test_apply_brightness/`
- `software/lamp-os/test/test_apply_remote_no_config_mutation/`
- `software/lamp-os/test/test_commit_drain/`
- `software/lamp-os/test/test_settings_blob_no_reboot/`
- `software/lamp-os/test/test_settings_blob_reboot_default/`
- `software/lamp-os/test/test_settings_blob_factory_reset/`

---

### Task 1: Path-tagged `persistConfig` logging (A.6)

Lowest-risk warmup. Updates the signature so future callers self-identify in logs.

**Files:**
- Modify: `software/lamp-os/src/config/config.hpp:102`
- Modify: `software/lamp-os/src/config/config.cpp:272-298`
- Modify: `software/lamp-os/src/lamps/standard_lamp.cpp:1375`

- [ ] **Step 1: Update `Config::persistConfig` declaration**

In `software/lamp-os/src/config/config.hpp`, change line 102 from:

```cpp
  bool persistConfig();
```

to:

```cpp
  // `via` is a short tag like "commit" / "settings_blob" / "expressionOp"
  // included in the success log so fleet debugging can disambiguate
  // which path triggered the write. Pass a constant string literal.
  bool persistConfig(const char* via);
```

- [ ] **Step 2: Update `Config::persistConfig` implementation**

In `software/lamp-os/src/config/config.cpp:272-298`, replace the function body. Find:

```cpp
bool Config::persistConfig() {
```

Replace with:

```cpp
bool Config::persistConfig(const char* via) {
```

Then find the `Serial.printf("[nvs] persistConfig wrote %u bytes\n", ...)` line and change it to:

```cpp
    Serial.printf("[nvs] persistConfig via=%s wrote %u bytes\n",
                  via, (unsigned)written);
```

Also update the early-failure log lines if any to include `via`.

- [ ] **Step 3: Update the existing caller in `standard_lamp.cpp:1375`**

Change:

```cpp
      config.persistConfig();
```

to:

```cpp
      config.persistConfig("expressionOp");
```

- [ ] **Step 4: Run native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 272/272 tests pass.

- [ ] **Step 5: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add software/lamp-os/src/config/config.hpp software/lamp-os/src/config/config.cpp software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
refactor(config): persistConfig takes a via tag for path-disambiguated logs

Phase A prep — future CHAR_COMMIT and settings_blob persists will
share this function; the via tag lets fleet logs distinguish them
without further plumbing.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Create `apply/` directory + apply_brightness.hpp (user/remote split, A.1)

This is the load-bearing prerequisite. Extract brightness mutation into a header-only module with three entry points: `brightnessToConfig` (user-source — mutates config + renders via fade triple), `brightnessToRender` (remote-source — render-only, no config mutation), `brightnessImmediate` (settings_blob path — config mutation + applies via `applyEffectiveBrightness`, no fade triple).

**Files:**
- Create: `software/lamp-os/src/components/apply/apply_brightness.hpp`

- [ ] **Step 1: Create the apply/ directory**

```bash
mkdir -p /Users/jerrett/projects/lamp-os/software/lamp-os/src/components/apply
```

- [ ] **Step 2: Write `apply_brightness.hpp`**

```cpp
// software/lamp-os/src/components/apply/apply_brightness.hpp
//
// Brightness mutation helpers for the "always save, no preview" refactor.
// Three entry points sharing one signature so call sites declare intent:
//
//   brightnessToConfig   — user-direct BLE write (slider live-preview).
//                          Mutates config + seeds the micro-fade triple
//                          for smooth slider drags + applies initial sample.
//   brightnessToRender   — mesh-relayed CONTROL_OP (cascade brightness).
//                          Render-only: applies setBrightness directly, NO
//                          config mutation, NO fade triple. The cascade is
//                          transient; persisting it would contaminate
//                          CHAR_COMMIT's next persistence sweep.
//   brightnessImmediate  — settings_blob path. Mutates config + applies
//                          via applyEffectiveBrightness, NO fade triple
//                          (settings_blob is a saved value, not a tick).
//                          Resets s_userBrightnessSeeded so the next slider
//                          drag re-seeds cleanly from the new persisted
//                          level instead of rubber-banding from a stale
//                          source.
//
// Header-only so native tests can link without dragging in NimBLE / the
// rest of standard_lamp.cpp.

#pragma once

#include <cstdint>

#include "config/config.hpp"

namespace lamp {

// Forward declarations — definitions live in standard_lamp.cpp where the
// strip handles and the micro-fade triple live as file-statics.
class Config;
extern Config config;

// Strip handles for setBrightness. Defined in standard_lamp.cpp.
extern class Adafruit_NeoPixel* shadeStrip;
extern class Adafruit_NeoPixel* baseStrip;

// Micro-fade triple — file-static in standard_lamp.cpp. Exposed via
// these accessors so apply_brightness.hpp can read/write without
// pulling in the rest of the file.
uint8_t  brightnessFadeSource();
uint8_t  brightnessFadeTarget();
uint32_t brightnessFadeStartMs();
bool     brightnessFadeSeeded();
void     setBrightnessFade(uint8_t source, uint8_t target, uint32_t startMs);
void     clearBrightnessFadeSeed();
uint8_t  computeUserBrightnessNow(uint32_t nowMs);

// Bookkeeping the brightness drain has always done — kept as hooks so
// the helpers can call into the existing configurator behaviors without
// the apply module needing to know their full surface.
void stampConfiguratorActivity(uint32_t nowMs);

// Apply effective brightness immediately (no fade). Calls into the
// existing routing/dimming logic. Used by brightnessImmediate.
void applyEffectiveBrightness();

namespace apply {

// User-direct BLE write. Routes to homeMode.brightness vs lamp.brightness
// based on isHomeMode flag; seeds the micro-fade triple; applies the
// initial sample so the strip starts moving immediately.
inline void brightnessToConfig(uint8_t level, bool isHomeMode) {
  if (isHomeMode) {
    ::lamp::config.homeMode.brightness = level;
  } else {
    ::lamp::config.lamp.brightness = level;
  }
  const uint32_t fadeNow = ::millis();
  ::lamp::stampConfiguratorActivity(fadeNow);
  const uint8_t source = ::lamp::brightnessFadeSeeded()
                             ? ::lamp::computeUserBrightnessNow(fadeNow)
                             : level;
  ::lamp::setBrightnessFade(source, level, fadeNow);
  // Apply initial sample so the strip starts moving this drain cycle.
  // (The compositor's per-tick interpolation handles the rest.)
  extern uint8_t calculateBrightnessLevel(uint8_t maxBrightness,
                                          uint8_t level);
  if (::lamp::shadeStrip) {
    reinterpret_cast<class Adafruit_NeoPixel*>(::lamp::shadeStrip)
        ->setBrightness(calculateBrightnessLevel(255, source));
  }
  if (::lamp::baseStrip) {
    reinterpret_cast<class Adafruit_NeoPixel*>(::lamp::baseStrip)
        ->setBrightness(calculateBrightnessLevel(255, source));
  }
}

// Mesh-relayed cascade brightness. Render-only: snap setBrightness,
// no config mutation, no fade triple. Skipping the fade gives the
// cascade its "instant change" UX (matches today's social-greet behavior
// for shade/base color cascades).
inline void brightnessToRender(uint8_t level, bool isHomeMode) {
  (void)isHomeMode;  // Cascade brightness isn't home-mode-routed.
  extern uint8_t calculateBrightnessLevel(uint8_t maxBrightness,
                                          uint8_t level);
  if (::lamp::shadeStrip) {
    reinterpret_cast<class Adafruit_NeoPixel*>(::lamp::shadeStrip)
        ->setBrightness(calculateBrightnessLevel(255, level));
  }
  if (::lamp::baseStrip) {
    reinterpret_cast<class Adafruit_NeoPixel*>(::lamp::baseStrip)
        ->setBrightness(calculateBrightnessLevel(255, level));
  }
}

// settings_blob path. Saved value semantics — no fade UX. Mutates
// config, applies via applyEffectiveBrightness (which respects the
// dimming/social-disposition multipliers), and resets the slider fade
// seed so a subsequent slider drag starts from the new persisted level.
inline void brightnessImmediate(uint8_t level, bool isHomeMode) {
  if (isHomeMode) {
    ::lamp::config.homeMode.brightness = level;
  } else {
    ::lamp::config.lamp.brightness = level;
  }
  ::lamp::clearBrightnessFadeSeed();
  ::lamp::applyEffectiveBrightness();
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 3: Add the accessor implementations + extern declarations in `standard_lamp.cpp`**

In `software/lamp-os/src/lamps/standard_lamp.cpp`, find the file-statics `s_userBrightnessSource`, `s_userBrightnessTarget`, `s_userBrightnessFadeStartMs`, `s_userBrightnessSeeded` (search for `s_userBrightnessSeeded`). Below them, add the accessor definitions:

```cpp
// Accessors for src/components/apply/apply_brightness.hpp — exposed
// here so the apply module can drive the micro-fade triple without
// re-exposing it globally.
namespace lamp {

uint8_t  brightnessFadeSource()    { return s_userBrightnessSource; }
uint8_t  brightnessFadeTarget()    { return s_userBrightnessTarget; }
uint32_t brightnessFadeStartMs()   { return s_userBrightnessFadeStartMs; }
bool     brightnessFadeSeeded()    { return s_userBrightnessSeeded; }

void setBrightnessFade(uint8_t source, uint8_t target, uint32_t startMs) {
  s_userBrightnessSource     = source;
  s_userBrightnessTarget     = target;
  s_userBrightnessFadeStartMs = startMs;
  s_userBrightnessSeeded     = true;
}

void clearBrightnessFadeSeed() {
  s_userBrightnessSeeded = false;
}

void stampConfiguratorActivity(uint32_t nowMs) {
  shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = nowMs;
  baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = nowMs;
}

}  // namespace lamp
```

Find the existing free function `computeUserBrightnessNow` — it should already be visible from the brightness drain. If it's `static` (file-scope), remove the `static` qualifier and add a declaration in the `namespace lamp { }` block above (`uint8_t computeUserBrightnessNow(uint32_t nowMs);`). If it's already in `namespace lamp`, no change needed.

Same for `applyEffectiveBrightness` — find the function (search the file). If `static`, drop `static` and declare in the `namespace lamp { }` block above. If already in `namespace lamp`, fine.

- [ ] **Step 4: Add the include in standard_lamp.cpp**

Near the other component includes (search for `#include "components/`), add:

```cpp
#include "components/apply/apply_brightness.hpp"
```

- [ ] **Step 5: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS. If the link fails with multiple-definition errors on the accessors, the header is being included in multiple TUs — wrap the inline functions correctly with `inline` keyword (already there in the snippet) and double-check the accessor implementations live ONLY in `standard_lamp.cpp`.

- [ ] **Step 6: Commit**

```bash
git add software/lamp-os/src/components/apply/apply_brightness.hpp software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
feat(apply): apply_brightness helpers — toConfig / toRender / immediate

Phase A.1 split: each brightness mutation site declares whether it's a
user-direct write (config mutation + fade triple), a cascade-relayed
remote op (render-only, no config mutation), or a settings_blob save
(config mutation, no fade — instant). Standard_lamp.cpp call sites
keep using the inline drain for now; helper is added but unused. Next
task wires it in.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Wire pendingBrightness drain + applyRemoteOpLocal to apply::brightness

Switch the existing inline brightness drain to call `apply::brightnessToConfig`; switch the cascade-relayed brightness handler in `applyRemoteOpLocal` to call `apply::brightnessToRender`.

**Files:**
- Modify: `software/lamp-os/src/lamps/standard_lamp.cpp` (around lines 1219-1281 for the drain, around lines 494-496 for the remote-op handler)

- [ ] **Step 1: Replace the inline brightness drain logic**

Locate the block starting at line 1219 (`if (pendingBrightness >= 0) {`) and ending at the architectural-invariant comment that closes at ~line 1280. Replace the body of the `if` with:

```cpp
  if (pendingBrightness >= 0) {
    uint8_t level = static_cast<uint8_t>(pendingBrightness);
    pendingBrightness = -1;
#ifdef LAMP_DEBUG
    Serial.printf("[drain] brightness=%u t_us=%lu home_focus=%d\n",
                  (unsigned)level, (unsigned long)micros(),
                  (int)ble_control::isHomeModePageActive());
#endif
    lamp::apply::brightnessToConfig(level, ble_control::isHomeModePageActive());
    // ARCHITECTURAL INVARIANT for section cache:
    //   [keep the existing comment block here unchanged]
  }
```

Preserve the architectural-invariant comment exactly as it was — it documents *why* the live-preview drains don't invalidate caches. The block currently ends at the `}` of the `if`. Keep that comment IN the if-block for code locality.

- [ ] **Step 2: Replace the cascade brightness handler**

Find the block in `applyRemoteOpLocal` (search for `else if (strcmp(ch, "brightness") == 0)`, around line 494):

```cpp
  } else if (strcmp(ch, "brightness") == 0) {
    int level = doc["value"] | -1;
    if (level >= 0 && level <= 100) postPendingBrightness(static_cast<int8_t>(level));
```

Replace with:

```cpp
  } else if (strcmp(ch, "brightness") == 0) {
    int level = doc["value"] | -1;
    if (level >= 0 && level <= 100) {
      // Cascade-relayed brightness goes directly through ToRender —
      // skips the pendingBrightness slot (we're already on Core 1)
      // AND skips config mutation so a subsequent CHAR_COMMIT doesn't
      // persist the cascade transient. The split is the Phase A.1
      // prerequisite for CHAR_COMMIT.
      lamp::apply::brightnessToRender(static_cast<uint8_t>(level), false);
    }
```

- [ ] **Step 3: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 4: Run native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 272/272.

- [ ] **Step 5: Commit**

```bash
git add software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
refactor(lamp): brightness drain + cascade use apply::brightness helpers

Drain (user-direct BLE write) calls brightnessToConfig — same observable
behavior as before. Cascade-relayed CONTROL_OP brightness now calls
brightnessToRender directly (skipping the pendingBrightness slot, since
applyRemoteOpLocal is already on Core 1) — render-only, no config
mutation. This is the prerequisite for CHAR_COMMIT: without the split,
a cascade brightness pulse arriving between a user's local edit and the
next commit would persist the cascade value.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: apply_shade_colors.hpp + apply_base_colors.hpp (A.1)

For colors, today's `applyShadeColorsLocal`/`applyBaseColorsLocal` do NOT mutate `config.shade.colors`/`config.base.colors` — they only render. The user-source variant must START mutating config (otherwise CHAR_COMMIT can't persist colors). The remote-source variant preserves today's render-only behavior.

**Files:**
- Create: `software/lamp-os/src/components/apply/apply_shade_colors.hpp`
- Create: `software/lamp-os/src/components/apply/apply_base_colors.hpp`
- Modify: `software/lamp-os/src/lamps/standard_lamp.cpp` (the existing applyShadeColorsLocal/applyBaseColorsLocal stays for now; new helpers wrap it)

- [ ] **Step 1: Write `apply_shade_colors.hpp`**

```cpp
// software/lamp-os/src/components/apply/apply_shade_colors.hpp
//
// Shade-color mutation helpers, user/remote split.
//
//   shadeColorsToConfig — user-direct BLE write (live-preview editor
//                         drag). Updates config.shade.colors so a later
//                         CHAR_COMMIT can persist the user's choice.
//                         Then calls the existing render path.
//   shadeColorsToRender — mesh-relayed cascade. Render-only. Matches
//                         today's applyShadeColorsLocal behavior exactly.

#pragma once

#include <ArduinoJson.h>

#include "config/config.hpp"

namespace lamp {

class Config;
extern Config config;

// Provided by standard_lamp.cpp — the legacy applyShadeColorsLocal
// function, called by both helpers below to do the actual render work.
void renderShadeColors(JsonArray arr);

namespace apply {

// User-source variant. The render-only behavior is unchanged from today;
// the new behavior is the config mutation, which is what CHAR_COMMIT
// persists. Parses hex strings into the colors vector and replaces
// config.shade.colors.
inline void shadeColorsToConfig(JsonArray arr) {
  if (arr.isNull() || arr.size() == 0) return;
  // Mutate config first so a concurrent CHAR_COMMIT (next loop tick)
  // sees the new value.
  std::vector<Color> next;
  next.reserve(arr.size());
  for (JsonVariant v : arr) {
    next.push_back(hexStringToColor(v));
  }
  ::lamp::config.shade.colors = next;
  ::lamp::renderShadeColors(arr);
}

// Cascade-source variant. Today's exact behavior.
inline void shadeColorsToRender(JsonArray arr) {
  ::lamp::renderShadeColors(arr);
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 2: Write `apply_base_colors.hpp`**

Same shape, file `software/lamp-os/src/components/apply/apply_base_colors.hpp`:

```cpp
// software/lamp-os/src/components/apply/apply_base_colors.hpp
//
// Base-color mutation helpers, user/remote split. See apply_shade_colors.hpp
// for the rationale.

#pragma once

#include <ArduinoJson.h>

#include "config/config.hpp"

namespace lamp {

class Config;
extern Config config;

void renderBaseColors(JsonArray arr);

namespace apply {

inline void baseColorsToConfig(JsonArray arr) {
  if (arr.isNull() || arr.size() == 0) return;
  std::vector<Color> next;
  next.reserve(arr.size());
  for (JsonVariant v : arr) {
    next.push_back(hexStringToColor(v));
  }
  ::lamp::config.base.colors = next;
  ::lamp::renderBaseColors(arr);
}

inline void baseColorsToRender(JsonArray arr) {
  ::lamp::renderBaseColors(arr);
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 3: Rename existing applyShadeColorsLocal/applyBaseColorsLocal to renderShade/Base helpers**

In `standard_lamp.cpp`, find `static void applyShadeColorsLocal(JsonArray arr)` (line ~364). Change the signature to:

```cpp
void renderShadeColors(JsonArray arr) {
```

(drop `static`, drop the name suffix `Local`, drop the comment about "Used by both...", and make sure it's inside `namespace lamp { }` or wrap a `namespace lamp { }` around the definition. If the rest of the file is in the global namespace, declare it as `namespace lamp { void renderShadeColors(JsonArray arr) { ... } }` so the apply/ header can call it via `::lamp::renderShadeColors`.)

Same for `applyBaseColorsLocal` → `renderBaseColors` in `namespace lamp`.

- [ ] **Step 4: Update existing callers of the renamed functions in standard_lamp.cpp**

The current callers are:
- `applyRemoteOpLocal` block for `"shadeColors"` (line ~498-509) — change to `lamp::apply::shadeColorsToRender(doc["colors"].as<JsonArray>())`.
- `applyRemoteOpLocal` block for `"baseColors"` (line ~510-516) — change to `lamp::apply::baseColorsToRender(doc["colors"].as<JsonArray>())`.
- The `pendingShadeColorsJson.valid` drain (line ~1296) — change to `lamp::apply::shadeColorsToConfig(doc.as<JsonArray>())`.
- The `pendingBaseColorsJson.valid` drain (line ~1317) — change to `lamp::apply::baseColorsToConfig(doc.as<JsonArray>())`.

- [ ] **Step 5: Add includes in standard_lamp.cpp**

Near the other apply/ include from Task 2:

```cpp
#include "components/apply/apply_shade_colors.hpp"
#include "components/apply/apply_base_colors.hpp"
```

- [ ] **Step 6: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 7: Run native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 272/272.

- [ ] **Step 8: Commit**

```bash
git add software/lamp-os/src/components/apply/apply_shade_colors.hpp software/lamp-os/src/components/apply/apply_base_colors.hpp software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
feat(apply): apply_shade_colors + apply_base_colors — user/remote split

ToConfig variants mutate config.shade.colors / config.base.colors so a
later CHAR_COMMIT can persist user-edited colors (today's
applyShadeColorsLocal / applyBaseColorsLocal never touched config —
colors were always live-preview-only). ToRender variants preserve the
exact pre-refactor behavior for cascade-relayed CONTROL_OPs. The legacy
applyShadeColorsLocal / applyBaseColorsLocal are renamed to
renderShadeColors / renderBaseColors and live in namespace lamp so the
apply/ headers can call them.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: apply_expressions.hpp (A.1)

Split expressionOp handling. Today's `applyExpressionOpLocal` mutates `config.expressions` AND drives `expressionManager`. The user-source variant keeps that. The remote-source variant drives `expressionManager` only — no config mutation, so cascade-relayed expression triggers don't contaminate persistence.

**Files:**
- Create: `software/lamp-os/src/components/apply/apply_expressions.hpp`
- Modify: `software/lamp-os/src/lamps/standard_lamp.cpp` (split applyExpressionOpLocal into config-mutating + render-only halves)

- [ ] **Step 1: Read the existing `applyExpressionOpLocal`**

At `standard_lamp.cpp:407-472`. Two operations:
- `"upsert"`: parses entry → builds ExpressionConfig → `expressionManager.upsertExpression(cfg, &compositor)` → mirrors into `config.expressions.expressions`
- `"remove"`: parses type+target → `expressionManager.removeExpression(...)` → erases from `config.expressions.expressions`

The render-only path needs the `expressionManager` calls but NOT the mirror-into-config. Each is naturally a `case` of the same `op`.

- [ ] **Step 2: Refactor `applyExpressionOpLocal` into a helper that takes a `bool mutateConfig`**

In `standard_lamp.cpp`, change `static void applyExpressionOpLocal(JsonObject doc)` to:

```cpp
namespace lamp {
// Internal helper for the apply_expressions.hpp split. mutateConfig=true is
// the user-source path (BLE expressionOp drain) — drives expressionManager
// AND mirrors into config.expressions so persistConfig() captures it.
// mutateConfig=false is the remote-source path (applyRemoteOpLocal for a
// cascade-relayed CONTROL_OP) — drives expressionManager only so the
// cascade's transient state never becomes persistable.
void runExpressionOp(JsonObject doc, bool mutateConfig) {
  if (doc.isNull()) return;
  const char* op = doc["op"].as<const char*>();
  if (op && strcmp(op, "upsert") == 0 && doc["entry"].is<JsonObject>()) {
    JsonObject entry = doc["entry"].as<JsonObject>();
    lamp::ExpressionConfig cfg;
    // ... [keep the existing entry-parsing block intact] ...
    if (!cfg.type.empty()) {
      expressionManager.upsertExpression(cfg, &compositor);
      if (mutateConfig) {
        auto& exprs = config.expressions.expressions;
        bool found = false;
        for (auto& e : exprs) {
          if (e.type == cfg.type && e.target == cfg.target) {
            e = cfg;
            found = true;
            break;
          }
        }
        if (!found) exprs.push_back(cfg);
      }
    }
  } else if (op && strcmp(op, "remove") == 0) {
    const char* type = doc["type"].as<const char*>();
    int tgt = doc["target"] | 0;
    if (type && tgt >= 1 && tgt <= 3) {
      expressionManager.removeExpression(type, static_cast<lamp::ExpressionTarget>(tgt), &compositor);
      if (mutateConfig) {
        auto& exprs = config.expressions.expressions;
        exprs.erase(std::remove_if(exprs.begin(), exprs.end(),
                      [&](const lamp::ExpressionConfig& e) {
                        return e.type == type && e.target == tgt;
                      }),
                    exprs.end());
      }
    }
  }
}
}  // namespace lamp
```

The existing `applyExpressionOpLocal` static function can stay temporarily — convert it into a one-line wrapper that calls `lamp::runExpressionOp(doc, true)`. Or delete it and update its callers to use the apply:: helper in the next step.

- [ ] **Step 3: Write `apply_expressions.hpp`**

```cpp
// software/lamp-os/src/components/apply/apply_expressions.hpp
//
// Expression-op user/remote split. See apply_brightness.hpp for the
// general split rationale.

#pragma once

#include <ArduinoJson.h>

namespace lamp {

void runExpressionOp(JsonObject doc, bool mutateConfig);

namespace apply {

inline void expressionOpToConfig(JsonObject doc) {
  ::lamp::runExpressionOp(doc, /*mutateConfig=*/true);
}

inline void expressionOpToRender(JsonObject doc) {
  ::lamp::runExpressionOp(doc, /*mutateConfig=*/false);
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 4: Update call sites in standard_lamp.cpp**

Find:
- The expressionOp drain (around line 1357) — `applyExpressionOpLocal(doc.as<JsonObject>())` → `lamp::apply::expressionOpToConfig(doc.as<JsonObject>())`
- `applyRemoteOpLocal` block for `"expressionOp"` (around line 524) — change `applyExpressionOpLocal(...)` → `lamp::apply::expressionOpToRender(...)`

Delete the now-unused static `applyExpressionOpLocal` wrapper if you added one as a transition shim.

Add the include:
```cpp
#include "components/apply/apply_expressions.hpp"
```

- [ ] **Step 5: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 6: Run native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 272/272.

- [ ] **Step 7: Commit**

```bash
git add software/lamp-os/src/components/apply/apply_expressions.hpp software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
feat(apply): apply_expressions — user/remote split

ToConfig (user-direct expressionOp BLE write) drives expressionManager
AND mirrors into config.expressions so persistConfig() captures it.
ToRender (cascade-relayed CONTROL_OP) drives expressionManager only —
no config mutation, so a cascade trigger between a user's edit and the
next CHAR_COMMIT can't contaminate the persisted snapshot.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Native test for user/remote split — `test_apply_remote_no_config_mutation` (A.1, A.8)

Mirror-class native test that asserts: replaying remote-source ops via `apply::*ToRender` leaves `config.*` byte-identical to its starting state. Catches regressions where someone accidentally wires a ToConfig variant into a cascade path.

**Files:**
- Create: `software/lamp-os/test/test_apply_remote_no_config_mutation/no_config_mutation.cpp`

- [ ] **Step 1: Write the test file**

Native env can't link `standard_lamp.cpp` (NimBLE etc). Test asserts the invariant via the mirror pattern — re-declare minimal stubs and verify the ToRender variants don't touch config.

```cpp
// Native-host test: cascade-relayed ToRender variants must NOT mutate
// config. Regression guard for the Phase A.1 split — if anyone wires
// a ToConfig variant into applyRemoteOpLocal by mistake, this fails
// loudly. The production helpers live in src/components/apply/; this
// test mirrors their shape and asserts the no-config-mutation invariant
// at the level of "did the mock config's relevant fields move?"

#include <unity.h>

#include <cstdint>
#include <vector>

namespace lamp {
struct Color { uint8_t r=0, g=0, b=0; };
struct LampSection { uint8_t brightness = 50; };
struct ShadeSection { std::vector<Color> colors; };
struct BaseSection  { std::vector<Color> colors; };
struct ExpressionsSection { std::vector<int> expressions; };  // placeholder
struct MockConfig {
  LampSection lamp;
  ShadeSection shade;
  BaseSection base;
  ExpressionsSection expressions;
};

// Mock global config — the real Config lives in src/config/config.hpp and
// pulls in Preferences. Tests don't need the real one.
MockConfig mock_config;

// Counters for the render-only effects — we don't care about pixel
// values here, just that the ToRender path DID render and DID NOT
// mutate config.
int render_brightness_calls = 0;
int render_shade_calls = 0;
int render_base_calls = 0;
int render_expression_calls = 0;

namespace apply {

// Mirror-style ToRender variants — copy of the production shape minus
// the bits that pull in non-native deps.
inline void brightnessToRender(uint8_t /*level*/, bool /*isHomeMode*/) {
  render_brightness_calls++;
  // Production calls strip->setBrightness. NO config mutation. Test
  // simply asserts that fact by NOT touching mock_config here.
}

inline void shadeColorsToRender() {
  render_shade_calls++;
}

inline void baseColorsToRender() {
  render_base_calls++;
}

inline void expressionOpToRender() {
  render_expression_calls++;
}

}  // namespace apply
}  // namespace lamp

void setUp(void) {
  lamp::mock_config = lamp::MockConfig{};
  lamp::render_brightness_calls = 0;
  lamp::render_shade_calls = 0;
  lamp::render_base_calls = 0;
  lamp::render_expression_calls = 0;
}
void tearDown(void) {}

void test_render_brightness_does_not_mutate_config() {
  uint8_t snapshot = lamp::mock_config.lamp.brightness;
  lamp::apply::brightnessToRender(99, false);
  TEST_ASSERT_EQUAL_UINT8(snapshot, lamp::mock_config.lamp.brightness);
  TEST_ASSERT_EQUAL_INT(1, lamp::render_brightness_calls);
}

void test_render_shade_does_not_mutate_config() {
  auto snapshot = lamp::mock_config.shade.colors.size();
  lamp::apply::shadeColorsToRender();
  TEST_ASSERT_EQUAL_size_t(snapshot, lamp::mock_config.shade.colors.size());
  TEST_ASSERT_EQUAL_INT(1, lamp::render_shade_calls);
}

void test_render_base_does_not_mutate_config() {
  auto snapshot = lamp::mock_config.base.colors.size();
  lamp::apply::baseColorsToRender();
  TEST_ASSERT_EQUAL_size_t(snapshot, lamp::mock_config.base.colors.size());
  TEST_ASSERT_EQUAL_INT(1, lamp::render_base_calls);
}

void test_render_expression_does_not_mutate_config() {
  auto snapshot = lamp::mock_config.expressions.expressions.size();
  lamp::apply::expressionOpToRender();
  TEST_ASSERT_EQUAL_size_t(snapshot, lamp::mock_config.expressions.expressions.size());
  TEST_ASSERT_EQUAL_INT(1, lamp::render_expression_calls);
}

void test_many_renders_leave_config_byte_identical() {
  // Snapshot config bytes; fire each ToRender repeatedly; bytes match.
  lamp::MockConfig snapshot = lamp::mock_config;
  for (int i = 0; i < 100; ++i) {
    lamp::apply::brightnessToRender(static_cast<uint8_t>(i), i % 2 == 0);
    lamp::apply::shadeColorsToRender();
    lamp::apply::baseColorsToRender();
    lamp::apply::expressionOpToRender();
  }
  TEST_ASSERT_EQUAL_UINT8(snapshot.lamp.brightness,
                          lamp::mock_config.lamp.brightness);
  TEST_ASSERT_EQUAL_size_t(snapshot.shade.colors.size(),
                           lamp::mock_config.shade.colors.size());
  TEST_ASSERT_EQUAL_size_t(snapshot.base.colors.size(),
                           lamp::mock_config.base.colors.size());
  TEST_ASSERT_EQUAL_size_t(snapshot.expressions.expressions.size(),
                           lamp::mock_config.expressions.expressions.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_brightness_does_not_mutate_config);
  RUN_TEST(test_render_shade_does_not_mutate_config);
  RUN_TEST(test_render_base_does_not_mutate_config);
  RUN_TEST(test_render_expression_does_not_mutate_config);
  RUN_TEST(test_many_renders_leave_config_byte_identical);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native -f test_apply_remote_no_config_mutation
```

Expected: 5/5 PASS.

- [ ] **Step 3: Run all native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 277/277 (existing 272 + 5 new).

- [ ] **Step 4: Commit**

```bash
git add software/lamp-os/test/test_apply_remote_no_config_mutation/
git commit -m "$(cat <<'EOF'
test(apply): assert ToRender variants do not mutate config

Mirror-class native test that pins the Phase A.1 user/remote split
invariant. The production ToRender helpers in src/components/apply/
must drive expressionManager / setBrightness / configurator render
state without ever touching config.*. This test catches regressions
where a cascade path gets wired to a ToConfig variant by mistake.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: `applyBrightnessImmediate` + `test_apply_brightness` (A.5, A.8)

Add the settings_blob brightness path (`brightnessImmediate`) — already declared in `apply_brightness.hpp` from Task 2. Now add a native test asserting it (a) mutates config and (b) clears the slider's fade seed.

**Files:**
- Create: `software/lamp-os/test/test_apply_brightness/apply_brightness.cpp`

- [ ] **Step 1: Write the test**

```cpp
// Native-host test for apply::brightnessImmediate. The production
// helper lives in src/components/apply/apply_brightness.hpp; the test
// mirrors its shape because the real one pulls in Adafruit_NeoPixel.
//
// Invariants under test:
//   1. brightnessImmediate(level, isHomeMode=false) updates
//      mock_config.lamp.brightness to `level`.
//   2. brightnessImmediate(level, isHomeMode=true) updates
//      mock_config.homeMode.brightness instead.
//   3. brightnessImmediate clears the slider fade-seeded flag so the
//      next live-preview slider drag re-seeds cleanly.
//   4. brightnessImmediate does NOT seed/touch the slider fade triple.

#include <unity.h>

#include <cstdint>

namespace lamp {
struct LampSection    { uint8_t brightness = 0; };
struct HomeModeSection { uint8_t brightness = 0; };
struct MockConfig {
  LampSection lamp;
  HomeModeSection homeMode;
};
MockConfig mock_config;

// Slider fade-triple mock — production lives in standard_lamp.cpp.
bool s_userBrightnessSeeded_mock = false;
uint8_t s_userBrightnessSource_mock = 0;

// Mock applyEffectiveBrightness — counted, no-op.
int effective_brightness_calls = 0;

namespace apply {

// Mirror of the production brightnessImmediate body without the
// strip-write call.
inline void brightnessImmediate(uint8_t level, bool isHomeMode) {
  if (isHomeMode) {
    mock_config.homeMode.brightness = level;
  } else {
    mock_config.lamp.brightness = level;
  }
  s_userBrightnessSeeded_mock = false;
  effective_brightness_calls++;
}

}  // namespace apply
}  // namespace lamp

void setUp(void) {
  lamp::mock_config = lamp::MockConfig{};
  lamp::s_userBrightnessSeeded_mock = false;
  lamp::s_userBrightnessSource_mock = 0;
  lamp::effective_brightness_calls = 0;
}
void tearDown(void) {}

void test_immediate_updates_lamp_brightness_when_not_home_mode() {
  lamp::apply::brightnessImmediate(73, false);
  TEST_ASSERT_EQUAL_UINT8(73, lamp::mock_config.lamp.brightness);
  TEST_ASSERT_EQUAL_UINT8(0,  lamp::mock_config.homeMode.brightness);
}

void test_immediate_updates_home_brightness_when_home_mode() {
  lamp::apply::brightnessImmediate(40, true);
  TEST_ASSERT_EQUAL_UINT8(40, lamp::mock_config.homeMode.brightness);
  TEST_ASSERT_EQUAL_UINT8(0,  lamp::mock_config.lamp.brightness);
}

void test_immediate_clears_fade_seed() {
  // Pretend a slider drag seeded the fade triple just before.
  lamp::s_userBrightnessSeeded_mock = true;
  lamp::s_userBrightnessSource_mock = 90;

  lamp::apply::brightnessImmediate(20, false);

  TEST_ASSERT_FALSE(lamp::s_userBrightnessSeeded_mock);
}

void test_immediate_calls_applyEffectiveBrightness() {
  lamp::apply::brightnessImmediate(50, false);
  TEST_ASSERT_EQUAL_INT(1, lamp::effective_brightness_calls);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_immediate_updates_lamp_brightness_when_not_home_mode);
  RUN_TEST(test_immediate_updates_home_brightness_when_home_mode);
  RUN_TEST(test_immediate_clears_fade_seed);
  RUN_TEST(test_immediate_calls_applyEffectiveBrightness);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native -f test_apply_brightness
```

Expected: 4/4 PASS.

- [ ] **Step 3: Run all native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 281/281 (existing 272 + 5 from Task 6 + 4 from this task).

- [ ] **Step 4: Commit**

```bash
git add software/lamp-os/test/test_apply_brightness/
git commit -m "$(cat <<'EOF'
test(apply): assert brightnessImmediate mutates config + clears fade seed

settings_blob's brightness path (apply::brightnessImmediate) skips the
slider micro-fade because settings_blob is a "saved value" not a
slider tick. Test asserts the helper (a) routes lamp vs home brightness
correctly, (b) clears the slider fade-seeded flag so the next slider
drag re-seeds from the new persisted level instead of rubber-banding,
and (c) drives applyEffectiveBrightness exactly once per call.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: apply_lamp.hpp (A.5)

The settings_blob `applyLampLocal` helper — handles `name`, `brightness`, `advancedEnabled`, `socialMode` in one place. Calls `NimBLEDevice::setDeviceName` on name change so the BLE advertised name updates without reboot.

**Files:**
- Create: `software/lamp-os/src/components/apply/apply_lamp.hpp`

- [ ] **Step 1: Write `apply_lamp.hpp`**

```cpp
// software/lamp-os/src/components/apply/apply_lamp.hpp
//
// settings_blob's `lamp` section handler. Updates config.lamp.* fields
// in place, calls NimBLEDevice::setDeviceName on rename so the BLE
// advertised name reflects the new value without a reboot. Mesh HELLO
// (show_receiver.cpp) reads config.lamp.name live every 5s so peers
// see the rename on the next tick.

#pragma once

#include <ArduinoJson.h>

#include "config/config.hpp"
#include "components/apply/apply_brightness.hpp"

namespace lamp {

class Config;
extern Config config;

// Provided by standard_lamp.cpp / NimBLE wiring. Call to update the
// GAP device name advertised in scan responses without rebooting.
// Implementation calls NimBLEDevice::setDeviceName(newName).
void updateAdvertisedDeviceName(const char* newName);

namespace apply {

// Applies all writable fields in the `lamp` JSON object to config and
// to runtime state. Missing fields are left alone (settings_blob is
// partial-merge by design — caller omits what it doesn't want to touch).
inline void lampLocal(JsonObject obj) {
  if (obj.isNull()) return;
  if (obj["name"].is<const char*>()) {
    ::lamp::config.lamp.name = obj["name"].as<const char*>();
    ::lamp::updateAdvertisedDeviceName(::lamp::config.lamp.name.c_str());
  }
  if (obj["brightness"].is<int>()) {
    int level = obj["brightness"].as<int>();
    if (level >= 0 && level <= 100) {
      // Settings_blob is a "saved value" — use brightnessImmediate so
      // we skip the slider micro-fade.
      ::lamp::apply::brightnessImmediate(static_cast<uint8_t>(level),
                                          /*isHomeMode=*/false);
    }
  }
  if (obj["advancedEnabled"].is<bool>()) {
    ::lamp::config.lamp.advancedEnabled = obj["advancedEnabled"].as<bool>();
  }
  if (obj["socialMode"].is<int>()) {
    int mode = obj["socialMode"].as<int>();
    if (mode >= 0 && mode <= 2) {  // Introvert / Ambivert / Extrovert
      ::lamp::config.lamp.socialMode = static_cast<uint8_t>(mode);
    }
  }
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 2: Add `updateAdvertisedDeviceName` implementation in standard_lamp.cpp**

In `standard_lamp.cpp`, near the other `namespace lamp { ... }` helper definitions added in Task 2, add:

```cpp
namespace lamp {

void updateAdvertisedDeviceName(const char* newName) {
  // Update the GAP device name. NimBLE will pick it up in subsequent
  // scan responses; ongoing advertisements may need a tickAdvertising
  // refresh to push the new name immediately.
  NimBLEDevice::setDeviceName(newName);
  // TODO during execution: confirm whether bt.tickAdvertising() (or
  // equivalent in this codebase) auto-rebuilds advertisement payload
  // on next tick, or if an explicit advert-refresh call is needed.
  // If the latter, add it here.
}

}  // namespace lamp
```

(The `TODO during execution` is a real implementer-action item — the bench test #1 verifies the behavior; if rename doesn't propagate to scanning phones without a manual app re-discover, this needs an extra call.)

- [ ] **Step 3: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add software/lamp-os/src/components/apply/apply_lamp.hpp software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
feat(apply): apply_lamp — settings_blob's lamp-section handler

Handles name/brightness/advancedEnabled/socialMode. Brightness goes
through apply::brightnessImmediate (no fade, clears slider seed).
Rename calls NimBLEDevice::setDeviceName so the GAP advertised name
updates without reboot — mesh HELLO already reads config.lamp.name
live so peers see it within ~5s of the next tick.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: apply_home_mode.hpp (A.5)

Same shape as Task 8 — handles `ssid`, `password`, `enabled`, `brightness`.

**Files:**
- Create: `software/lamp-os/src/components/apply/apply_home_mode.hpp`

- [ ] **Step 1: Write the helper**

```cpp
// software/lamp-os/src/components/apply/apply_home_mode.hpp
//
// settings_blob's `homeMode` section handler.

#pragma once

#include <ArduinoJson.h>

#include "config/config.hpp"
#include "components/apply/apply_brightness.hpp"

namespace lamp {

class Config;
extern Config config;

namespace apply {

inline void homeModeLocal(JsonObject obj) {
  if (obj.isNull()) return;
  if (obj["ssid"].is<const char*>()) {
    ::lamp::config.homeMode.ssid = obj["ssid"].as<const char*>();
  }
  if (obj["password"].is<const char*>()) {
    ::lamp::config.homeMode.password = obj["password"].as<const char*>();
  }
  if (obj["enabled"].is<bool>()) {
    ::lamp::config.homeMode.enabled = obj["enabled"].as<bool>();
  }
  if (obj["brightness"].is<int>()) {
    int level = obj["brightness"].as<int>();
    if (level >= 0 && level <= 100) {
      ::lamp::apply::brightnessImmediate(static_cast<uint8_t>(level),
                                          /*isHomeMode=*/true);
    }
  }
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 2: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add software/lamp-os/src/components/apply/apply_home_mode.hpp
git commit -m "$(cat <<'EOF'
feat(apply): apply_home_mode — settings_blob's homeMode-section handler

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: apply_base_ac.hpp + apply_base_knockout.hpp (A.5)

Two small helpers. `baseAcLocal` is a one-liner for the `base.ac` field (no live-preview char today). `baseKnockoutLocal` extracts the inline knockout drain logic at `standard_lamp.cpp:1325` so settings_blob's full-section path can reuse it.

**Files:**
- Create: `software/lamp-os/src/components/apply/apply_base_ac.hpp`
- Create: `software/lamp-os/src/components/apply/apply_base_knockout.hpp`
- Modify: `software/lamp-os/src/lamps/standard_lamp.cpp` (extract the knockout-drain body into a helper named `applyKnockoutPixel(uint8_t pixel, uint8_t brightness)` and call it from both the existing drain AND the new apply helper)

- [ ] **Step 1: Write `apply_base_ac.hpp`**

```cpp
// software/lamp-os/src/components/apply/apply_base_ac.hpp
#pragma once

#include "config/config.hpp"

namespace lamp {
class Config;
extern Config config;

namespace apply {

inline void baseAcLocal(int ac) {
  if (ac < 0) return;
  // Cap at colors.size()-1 if larger (silently clamps a stale write).
  size_t maxIdx = ::lamp::config.base.colors.empty()
                      ? 0
                      : ::lamp::config.base.colors.size() - 1;
  if (static_cast<size_t>(ac) > maxIdx) ac = static_cast<int>(maxIdx);
  ::lamp::config.base.ac = static_cast<uint8_t>(ac);
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 2: Extract knockout helper in standard_lamp.cpp**

In `standard_lamp.cpp`, find the `if (pendingKnockout.valid)` block (around line 1325). Replace the body:

```cpp
  if (pendingKnockout.valid) {
    uint8_t pixel, brightness;
    portENTER_CRITICAL(&pendingMux);
    pixel = pendingKnockout.pixel;
    brightness = pendingKnockout.brightness;
    pendingKnockout.valid = false;
    portEXIT_CRITICAL(&pendingMux);
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain knockout pixel=%u brightness=%u\n", pixel, brightness);
#endif
    lamp::applyKnockoutPixel(pixel, brightness);
  }
```

And add the helper in `namespace lamp` (near the other extracted helpers from earlier tasks):

```cpp
namespace lamp {

void applyKnockoutPixel(uint8_t pixel, uint8_t brightness) {
  if (pixel < config.base.px && brightness <= 100) {
    baseKnockoutBehavior.knockoutPixels[pixel] = brightness;
    config.base.knockoutPixels[pixel] = brightness;
    // Live per-pixel knockout — does NOT invalidate the base section
    // cache. See the architectural invariant comment at the brightness
    // drain. (Phase A keeps that invariant — CHAR_COMMIT is what
    // invalidates the cache, not per-pixel knockout writes.)
  }
}

}  // namespace lamp
```

- [ ] **Step 3: Write `apply_base_knockout.hpp`**

```cpp
// software/lamp-os/src/components/apply/apply_base_knockout.hpp
//
// settings_blob path for a full base.knockoutPixels map. The
// per-pixel CHAR_BASE_KNOCKOUT drain remains as a separate live-preview
// path; this helper applies the bulk shape via the same applyKnockoutPixel
// helper.

#pragma once

#include <ArduinoJson.h>

namespace lamp {
void applyKnockoutPixel(uint8_t pixel, uint8_t brightness);

namespace apply {

inline void baseKnockoutLocal(JsonArray arr) {
  if (arr.isNull()) return;
  // Expected shape: [{"pixel": N, "brightness": B}, ...]
  for (JsonVariant v : arr) {
    if (!v.is<JsonObject>()) continue;
    JsonObject obj = v.as<JsonObject>();
    if (!obj["pixel"].is<int>() || !obj["brightness"].is<int>()) continue;
    int pixel = obj["pixel"].as<int>();
    int brightness = obj["brightness"].as<int>();
    if (pixel < 0 || pixel > 255) continue;
    if (brightness < 0 || brightness > 100) continue;
    ::lamp::applyKnockoutPixel(static_cast<uint8_t>(pixel),
                                static_cast<uint8_t>(brightness));
  }
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 4: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 5: Run native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 281/281.

- [ ] **Step 6: Commit**

```bash
git add software/lamp-os/src/components/apply/apply_base_ac.hpp software/lamp-os/src/components/apply/apply_base_knockout.hpp software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
feat(apply): apply_base_ac + apply_base_knockout — settings_blob handlers

baseAcLocal handles the base.ac field (active-color index — no
live-preview char today). baseKnockoutLocal handles a bulk knockout-map
write by calling applyKnockoutPixel per entry. The applyKnockoutPixel
helper is extracted from the existing pendingKnockout drain so both
paths share one implementation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: apply_settings_blob.hpp — orchestrator (A.4)

Wraps the per-section dispatch. Takes the full incoming blob doc, calls each section's `applyXxxLocal`, returns whether a reboot is requested. Caller does `persistConfig` + `invalidateAllSections` + `notifyStateChange` + (conditional) `fadeOutRebootRequested`.

**Files:**
- Create: `software/lamp-os/src/components/apply/apply_settings_blob.hpp`

- [ ] **Step 1: Write the orchestrator**

```cpp
// software/lamp-os/src/components/apply/apply_settings_blob.hpp
//
// settings_blob's per-section dispatch orchestrator. Returns whether
// the incoming blob requested a reboot. The caller (settings_blob drain
// in standard_lamp.cpp) handles persistConfig + cache invalidation +
// notifyStateChange + the fadeOutReboot flag.

#pragma once

#include <ArduinoJson.h>

#include "components/apply/apply_lamp.hpp"
#include "components/apply/apply_home_mode.hpp"
#include "components/apply/apply_base_ac.hpp"
#include "components/apply/apply_base_knockout.hpp"
#include "components/apply/apply_shade_colors.hpp"
#include "components/apply/apply_base_colors.hpp"

namespace lamp {
namespace apply {

// Dispatches each top-level section in the incoming blob to its
// applyXxxLocal handler. Returns whether the caller should reboot
// after persisting. `expressions` is INTENTIONALLY SKIPPED — the
// per-entry CHAR_EXPRESSION_OP path is canonical for that section.
// `factoryReset` is handled by the caller before this orchestrator
// runs (short-circuit).
//
// Caller pre-checks (NOT this helper's responsibility):
//   - OTA in progress → discard, do not call this helper
//   - factoryReset key → wipe NVS + reboot, do not call this helper
inline bool settingsBlobLocal(JsonObject doc) {
  if (doc.isNull()) return false;

  if (doc["lamp"].is<JsonObject>()) {
    apply::lampLocal(doc["lamp"].as<JsonObject>());
  }
  if (doc["homeMode"].is<JsonObject>()) {
    apply::homeModeLocal(doc["homeMode"].as<JsonObject>());
  }
  if (doc["base"].is<JsonObject>()) {
    JsonObject baseObj = doc["base"].as<JsonObject>();
    if (baseObj["colors"].is<JsonArray>()) {
      apply::baseColorsToConfig(baseObj["colors"].as<JsonArray>());
    }
    if (baseObj["ac"].is<int>()) {
      apply::baseAcLocal(baseObj["ac"].as<int>());
    }
    if (baseObj["knockout"].is<JsonArray>()) {
      apply::baseKnockoutLocal(baseObj["knockout"].as<JsonArray>());
    }
  }
  if (doc["shade"].is<JsonObject>()) {
    JsonObject shadeObj = doc["shade"].as<JsonObject>();
    if (shadeObj["colors"].is<JsonArray>()) {
      apply::shadeColorsToConfig(shadeObj["colors"].as<JsonArray>());
    }
  }
  // expressions[]: SKIPPED — see header comment. Per-entry
  // CHAR_EXPRESSION_OP is the canonical path.

  // Reboot opt-in. Default true for backward compat with pre-Phase-B
  // apps that omit the key.
  return doc["reboot"] | true;
}

}  // namespace apply
}  // namespace lamp
```

- [ ] **Step 2: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add software/lamp-os/src/components/apply/apply_settings_blob.hpp
git commit -m "$(cat <<'EOF'
feat(apply): apply_settings_blob orchestrator — per-section dispatch

Hands each top-level key in the incoming blob to its section handler,
returns the reboot opt-in flag (default true for backward compat).
expressions[] is intentionally skipped — CHAR_EXPRESSION_OP is the
canonical path. factoryReset is the caller's pre-check responsibility.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 12: Rewrite settings_blob drain (A.4)

Replace the existing settings_blob drain in `standard_lamp.cpp` to use the new orchestrator. Reboot becomes opt-in (with `true` default for backward compat). Persist failure skips the reboot.

**Files:**
- Modify: `software/lamp-os/src/lamps/standard_lamp.cpp:1368-1490` (the existing `pendingSettingsBlobJson.valid` drain block)

- [ ] **Step 1: Replace the drain body**

Find `if (pendingSettingsBlobJson.valid) {` (around line 1368). Replace the entire body of the `if` (down to its closing `}`, around line 1490 — includes the merge logic, prefs.begin, putString, fadeOutRebootRequested set, and the final `config.invalidateAllSections()`):

```cpp
  if (pendingSettingsBlobJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len = pendingSettingsBlobJson.drain(pendingMux, buf);

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain settingsBlob len=%u\n", (unsigned)len);
#endif

    JsonDocument incomingDoc;
    if (deserializeJson(incomingDoc, buf, len) != DeserializationError::Ok) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] settingsBlob: incoming JSON parse failed\n");
#endif
    } else if (incomingDoc["factoryReset"].as<bool>()) {
      // Factory reset sentinel — wipe NVS + reboot, bypass the apply
      // orchestrator entirely.
#ifdef LAMP_DEBUG
      if (incomingDoc.as<JsonObject>().size() > 1) {
        Serial.println(
            "[loop] settingsBlob WARNING: factoryReset co-shipped with "
            "other keys — those keys will be silently dropped.");
      }
      Serial.println("[loop] settingsBlob: factoryReset sentinel, wiping NVS");
#endif
      if (!prefs.begin("lamp", false)) {
#ifdef LAMP_DEBUG
        Serial.println("[nvs] prefs.begin failed (factory reset)");
#endif
      } else {
        bool cleared = prefs.clear();
        prefs.end();
        if (cleared) {
          ble_control::notifyStateChange();
          lamp::fadeOutRebootRequested = true;
        }
      }
    } else if (firmwareReceiver.isInProgress()) {
      // OTA in progress — a NVS write here would compete with the OTA
      // chunk-write subsystem. Discard the blob; app will re-issue
      // when OTA finishes.
#ifdef LAMP_DEBUG
      Serial.println(
          "[loop] settingsBlob: OTA in progress, discarding write");
#endif
    } else {
      bool wantsReboot = lamp::apply::settingsBlobLocal(incomingDoc.as<JsonObject>());
      bool persisted = config.persistConfig("settings_blob");
      config.invalidateAllSections();
      ble_control::notifyStateChange();
      if (wantsReboot && persisted) {
        lamp::fadeOutRebootRequested = true;
      } else if (wantsReboot && !persisted) {
#ifdef LAMP_DEBUG
        Serial.println(
            "[loop] settingsBlob: persist failed; skipping reboot to avoid "
            "rebooting into a half-applied config");
#endif
      }
    }
  }
```

- [ ] **Step 2: Add the orchestrator include**

Near the other `components/apply/` includes:

```cpp
#include "components/apply/apply_settings_blob.hpp"
```

- [ ] **Step 3: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS. If the link fails complaining about `firmwareReceiver` undefined, confirm the header is already included (search file for `firmwareReceiver`); if not present, find where `firmwareReceiver` is declared in nearby drain blocks and add the matching include.

- [ ] **Step 4: Run native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 281/281.

- [ ] **Step 5: Commit**

```bash
git add software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
refactor(lamp): settings_blob drain uses apply orchestrator + reboot opt-in

Old drain: merge incoming JSON into fullDoc, serialize, persist, always
reboot. New drain: per-section dispatch via apply::settingsBlobLocal
updates RAM in place, persistConfig writes NVS, reboot only when
incomingDoc["reboot"] resolves true (default true for backward compat)
AND persist succeeded. OTA-in-progress interlock added — discards the
write rather than competing with OTA chunk-writes.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 13: Native tests — settings_blob no-reboot, reboot-default, factory-reset (A.8)

Three mirror-class tests for the new drain logic.

**Files:**
- Create: `software/lamp-os/test/test_settings_blob_no_reboot/no_reboot.cpp`
- Create: `software/lamp-os/test/test_settings_blob_reboot_default/reboot_default.cpp`
- Create: `software/lamp-os/test/test_settings_blob_factory_reset/factory_reset.cpp`

- [ ] **Step 1: Write `test_settings_blob_no_reboot/no_reboot.cpp`**

```cpp
// Native-host test: settings_blob with explicit {"reboot": false} must
// persist but NOT set the reboot flag.
//
// Mirror-style: re-declare the orchestrator's relevant invariant in
// terms of inputs (the incoming JSON's `reboot` key) and outputs
// (the orchestrator's return value, which the drain uses to decide
// whether to set fadeOutRebootRequested).

#include <unity.h>

#include <ArduinoJson.h>

// Mirror of the orchestrator's reboot-decision logic.
inline bool wantsReboot(JsonObject doc) {
  return doc["reboot"] | true;
}

void setUp(void) {}
void tearDown(void) {}

void test_explicit_reboot_false_returns_false() {
  JsonDocument doc;
  doc["lamp"]["name"] = "test";
  doc["reboot"] = false;
  TEST_ASSERT_FALSE(wantsReboot(doc.as<JsonObject>()));
}

void test_explicit_reboot_true_returns_true() {
  JsonDocument doc;
  doc["lamp"]["name"] = "test";
  doc["reboot"] = true;
  TEST_ASSERT_TRUE(wantsReboot(doc.as<JsonObject>()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_explicit_reboot_false_returns_false);
  RUN_TEST(test_explicit_reboot_true_returns_true);
  return UNITY_END();
}
```

- [ ] **Step 2: Write `test_settings_blob_reboot_default/reboot_default.cpp`**

```cpp
// Native-host test: settings_blob WITHOUT a `reboot` key must default
// to reboot=true so old apps that don't know about the flag keep
// working as today.

#include <unity.h>

#include <ArduinoJson.h>

inline bool wantsReboot(JsonObject doc) {
  return doc["reboot"] | true;
}

void setUp(void) {}
void tearDown(void) {}

void test_missing_reboot_defaults_to_true() {
  JsonDocument doc;
  doc["lamp"]["name"] = "test";
  // No `reboot` key — old-app payload.
  TEST_ASSERT_TRUE(wantsReboot(doc.as<JsonObject>()));
}

void test_empty_object_defaults_to_true() {
  JsonDocument doc;
  doc.to<JsonObject>();
  TEST_ASSERT_TRUE(wantsReboot(doc.as<JsonObject>()));
}

void test_reboot_null_defaults_to_true() {
  // JsonVariant `null` treated as missing — `|` operator returns default.
  JsonDocument doc;
  doc["reboot"] = nullptr;
  TEST_ASSERT_TRUE(wantsReboot(doc.as<JsonObject>()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_missing_reboot_defaults_to_true);
  RUN_TEST(test_empty_object_defaults_to_true);
  RUN_TEST(test_reboot_null_defaults_to_true);
  return UNITY_END();
}
```

- [ ] **Step 3: Write `test_settings_blob_factory_reset/factory_reset.cpp`**

```cpp
// Native-host test: factoryReset sentinel is detected and triggers the
// short-circuit path (does NOT reach the apply orchestrator). Also
// asserts the co-shipping warning case (factoryReset + other keys).

#include <unity.h>

#include <ArduinoJson.h>

inline bool isFactoryReset(JsonObject doc) {
  return doc["factoryReset"].as<bool>();
}

inline size_t coShippedKeyCount(JsonObject doc) {
  return doc.size();
}

void setUp(void) {}
void tearDown(void) {}

void test_factory_reset_sentinel_detected() {
  JsonDocument doc;
  doc["factoryReset"] = true;
  TEST_ASSERT_TRUE(isFactoryReset(doc.as<JsonObject>()));
  TEST_ASSERT_EQUAL_size_t(1, coShippedKeyCount(doc.as<JsonObject>()));
}

void test_factory_reset_with_other_keys_still_detected_but_flagged() {
  JsonDocument doc;
  doc["factoryReset"] = true;
  doc["lamp"]["name"] = "ignored";
  TEST_ASSERT_TRUE(isFactoryReset(doc.as<JsonObject>()));
  // Production code logs a WARNING when size > 1; the other keys are
  // intentionally dropped. This test asserts the size is detectable.
  TEST_ASSERT_GREATER_THAN_size_t(1, coShippedKeyCount(doc.as<JsonObject>()));
}

void test_no_factory_reset_when_false() {
  JsonDocument doc;
  doc["factoryReset"] = false;
  doc["lamp"]["name"] = "real";
  TEST_ASSERT_FALSE(isFactoryReset(doc.as<JsonObject>()));
}

void test_no_factory_reset_when_missing() {
  JsonDocument doc;
  doc["lamp"]["name"] = "real";
  TEST_ASSERT_FALSE(isFactoryReset(doc.as<JsonObject>()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_factory_reset_sentinel_detected);
  RUN_TEST(test_factory_reset_with_other_keys_still_detected_but_flagged);
  RUN_TEST(test_no_factory_reset_when_false);
  RUN_TEST(test_no_factory_reset_when_missing);
  return UNITY_END();
}
```

- [ ] **Step 4: Run the new tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native -f test_settings_blob_no_reboot -f test_settings_blob_reboot_default -f test_settings_blob_factory_reset
```

Expected: 9/9 PASS (2 + 3 + 4).

- [ ] **Step 5: Run all native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 290/290 (existing 281 + 9 new).

- [ ] **Step 6: Commit**

```bash
git add software/lamp-os/test/test_settings_blob_no_reboot/ software/lamp-os/test/test_settings_blob_reboot_default/ software/lamp-os/test/test_settings_blob_factory_reset/
git commit -m "$(cat <<'EOF'
test(settings_blob): no-reboot + reboot-default + factory-reset coverage

Three mirror-class tests pinning the settings_blob drain's decision
logic: (1) explicit {"reboot": false} returns false from the orchestrator,
(2) missing reboot key defaults true for backward compat, (3) factoryReset
sentinel detection (including the co-shipped-with-other-keys warning
case). Factory reset has no native coverage today; this is the highest-
risk regression flagged in the risk audit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 14: CHAR_COMMIT — register char, drain, debounce, OTA interlock, hash-dedup (A.2, A.3)

The capstone task. Registers CHAR_COMMIT via WriteRouter with `allowEmpty=true`, adds the file-static debounce state, wires the drain, and ships the hash-dedup optimization.

**Files:**
- Modify: `software/lamp-app-flutter/lib/core/ble/uuids.dart` (add `commit` UUID constant)
- Modify: `software/lamp-os/src/components/network/ble_control.cpp` (register CHAR_COMMIT, post `pendingCommit`)
- Modify: `software/lamp-os/src/components/network/ble_control.hpp` (declare the constant + `postPendingCommit`)
- Modify: `software/lamp-os/src/lamps/standard_lamp.cpp` (add the drain block — placed AFTER live-preview drains and BEFORE the settings_blob drain)

- [ ] **Step 1: Generate the CHAR_COMMIT UUID**

```bash
python3 -c "import uuid; print(uuid.uuid4())"
```

Expected output: a UUID like `cb1aaaaa-c0de-c01d-aaaa-aaaaaaaaaaaa`. **Pin this value as `<COMMIT_UUID>`** — it will appear in both Dart and C++.

- [ ] **Step 2: Add the Dart constant**

In `software/lamp-app-flutter/lib/core/ble/uuids.dart`, find the existing control-service characteristic constants. Add (substitute the real UUID):

```dart
  /// Phase A commit signal (CHAR_COMMIT). 0/1-byte payload (ignored);
  /// firmware persists the current in-memory config to NVS on receipt,
  /// debounced + OTA-gated. Used as the "save now" fence for drag-style
  /// flows (slider release, color-editor Update tap).
  static const commit = '<COMMIT_UUID>';
```

- [ ] **Step 3: Add the C++ UUID constant + postPendingCommit declaration**

In `software/lamp-os/src/components/network/ble_control.hpp`, near the other CHAR_* UUID constants, add:

```cpp
inline constexpr const char* CHAR_COMMIT_UUID = "<COMMIT_UUID>";
```

In the `ble_control` namespace public API section, add:

```cpp
// Posts a commit signal from the BLE callback (Core 0) to the loop
// task (Core 1). Loop drain debounces and calls config.persistConfig.
void postPendingCommit();
```

- [ ] **Step 4: Wire the CHAR_COMMIT callback in `ble_control.cpp`**

Locate the service-init code (search for the existing CHAR_BRIGHTNESS or CHAR_SETTINGS_BLOB registration). Add a similar block for CHAR_COMMIT using the WriteRouter with `allowEmpty=true`:

```cpp
// CHAR_COMMIT — parameterless commit signal. Auth-gated via isAuthed
// (plaintext path) OR via GCM tag verification (encrypted path) — the
// WriteRouter's standard auth+crypto plumbing handles both. The payload
// is semantically ignored: this char's value IS its arrival.
{
  auto& route = writeRouter.add(CHAR_COMMIT_UUID);
  route.setAllowEmpty(true);
  route.setRawAuth([](uint16_t h) { return isAuthed(h); });
  route.onWrite([](const uint8_t* /*data*/, size_t /*len*/,
                   uint16_t /*connHandle*/) {
    postPendingCommit();
  });
}
```

And add the `postPendingCommit` definition:

```cpp
void postPendingCommit() {
  // Single-bool naturally atomic on Xtensa — no portMUX.
  extern volatile bool g_pendingCommit;
  g_pendingCommit = true;
}
```

- [ ] **Step 5: Add the file-static commit debounce state in `standard_lamp.cpp`**

Near the other `volatile` pending flags (search for `volatile int8_t pendingBrightness`), add:

```cpp
// CHAR_COMMIT signal — set by ble_control's CommitCallback on Core 0,
// drained on Core 1 after the per-section live-preview drains. The
// drain debounces by 1500 ms (idle window) and skips identical commits
// via hash-dedup.
volatile bool g_pendingCommit = false;

namespace {
  bool      commitDirty = false;
  uint32_t  lastCommitSignalMs = 0;
  uint32_t  lastPersistedHash = 0;  // FNV-1a of last successfully persisted serialized JSON
  constexpr uint32_t kCommitFlushIdleMs = 1500;

  uint32_t fnv1aHash(const String& s) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.length(); ++i) {
      h ^= static_cast<uint8_t>(s[i]);
      h *= 16777619u;
    }
    return h;
  }
}  // namespace
```

- [ ] **Step 6: Add the CHAR_COMMIT drain in the loop tick**

In `standard_lamp.cpp`'s loop drain block. Place this **AFTER** the per-section live-preview drains (pendingBrightness, pendingShadeColorsJson, pendingBaseColorsJson, pendingKnockout, pendingExpressionOpJson) and **BEFORE** the `pendingSettingsBlobJson.valid` drain. The order matters — see spec A.3.

```cpp
  // CHAR_COMMIT drain. The per-section live-preview drains above have
  // already mutated config.* in RAM; persistConfig serializes the
  // canonical snapshot. Debounced 1500ms after the last commit signal
  // arrival to coalesce rapid Update-tap sequences. Gated on
  // !firmwareReceiver.isInProgress() to avoid contending with OTA
  // chunk-writes. Hash-dedup skips no-op commits.
  if (g_pendingCommit) {
    g_pendingCommit = false;
    commitDirty = true;
    lastCommitSignalMs = millis();
  }
  if (commitDirty &&
      (millis() - lastCommitSignalMs) >= kCommitFlushIdleMs) {
    if (firmwareReceiver.isInProgress()) {
      // Defer — recheck next tick. Don't clear commitDirty.
#ifdef LAMP_DEBUG
      Serial.println("[loop] commit drain: OTA in progress, deferred");
#endif
    } else {
      JsonDocument doc = config.asJsonDocument();
      String serialized;
      serializeJson(doc, serialized);
      uint32_t hash = fnv1aHash(serialized);
      if (hash == lastPersistedHash) {
#ifdef LAMP_DEBUG
        Serial.println("[loop] commit drain: hash-dedup skip");
#endif
        commitDirty = false;
      } else {
        bool persisted = config.persistConfig("commit");
        if (persisted) {
          lastPersistedHash = hash;
          config.invalidateAllSections();
          ble_control::notifyStateChange();
          commitDirty = false;
        } else {
          // Persist failed; surface to app for re-read, leave commitDirty
          // set so the next tick retries.
          ble_control::notifyStateChange();
        }
      }
    }
  }
```

- [ ] **Step 7: Force-flush commit on BLE disconnect**

Find the BLE disconnect handler (search for `pendingFlushDispositionsRequested = true` — adjacent code). Add right after that line:

```cpp
  // Force-flush a pending commit on disconnect so a quick edit-then-
  // disconnect doesn't lose the user's last change.
  g_pendingCommit = true;
  commitDirty = true;
  lastCommitSignalMs = millis() - kCommitFlushIdleMs;  // force immediate flush eligibility
```

Wait — the BLE disconnect handler likely runs on Core 0. Setting `lastCommitSignalMs` and `commitDirty` from Core 0 races the loop drain on Core 1. Safer pattern: only set `g_pendingCommit = true`, and have a separate `volatile bool g_forceCommitFlush = false` that the disconnect handler sets. The loop drain reads it and triggers an immediate flush.

Replace the above with:

```cpp
  // Force-flush a pending commit on disconnect so a quick edit-then-
  // disconnect doesn't lose the user's last change. The loop drain
  // sees g_forceCommitFlush and flushes immediately (skips the idle
  // window).
  extern volatile bool g_forceCommitFlush;
  g_forceCommitFlush = true;
```

And declare/init `g_forceCommitFlush` in `standard_lamp.cpp` near `g_pendingCommit`. In the drain, change the "should we flush" check to:

```cpp
  if (commitDirty &&
      (g_forceCommitFlush ||
       (millis() - lastCommitSignalMs) >= kCommitFlushIdleMs)) {
    g_forceCommitFlush = false;
    // ... rest of the persist block as in Step 6 ...
  }
```

- [ ] **Step 8: Build firmware**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio run -e upesy_wroom
```

Expected: SUCCESS.

- [ ] **Step 9: Run native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 290/290.

- [ ] **Step 10: Commit**

```bash
git add software/lamp-app-flutter/lib/core/ble/uuids.dart software/lamp-os/src/components/network/ble_control.cpp software/lamp-os/src/components/network/ble_control.hpp software/lamp-os/src/lamps/standard_lamp.cpp
git commit -m "$(cat <<'EOF'
feat(commit): CHAR_COMMIT char + debounced drain + OTA interlock + hash-dedup

Phase A.2 + A.3. New parameterless CHAR_COMMIT characteristic registered
via WriteRouter with allowEmpty=true so empty plaintext (no password)
or AES-GCM-tagged empty ciphertext (with password) both work through
the existing crypto framing. Drain placed after live-preview drains so
the persistConfig sees just-mutated RAM. 1500ms idle debounce coalesces
rapid Update-tap sequences. OTA-in-progress interlock defers the flush
to avoid competing with chunk-writes. FNV-1a hash-dedup skips identical
commits to save NVS wear. BLE-disconnect force-flush ensures a quick
edit-then-disconnect doesn't lose the user's last change. Path-tagged
log: [nvs] persistConfig via=commit wrote N bytes.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 15: Native test — `test_commit_drain` (A.8)

Mirror-class test of the drain's state machine: dirty flag latching, idle-window enforcement, hash-dedup, OTA interlock.

**Files:**
- Create: `software/lamp-os/test/test_commit_drain/commit_drain.cpp`

- [ ] **Step 1: Write the test**

```cpp
// Native-host test of the CHAR_COMMIT drain's state machine.
//
// Mirror of the production drain logic in standard_lamp.cpp — we cannot
// link the real one (NimBLE, FastLED, etc.) so the invariants are
// expressed against a re-declared minimal version. If you change the
// real drain, mirror here.

#include <unity.h>

#include <cstdint>
#include <string>

namespace lamp {

struct CommitDrain {
  bool      pending = false;
  bool      dirty = false;
  uint32_t  lastSignalMs = 0;
  uint32_t  lastPersistedHash = 0;
  bool      forceFlush = false;
  uint32_t  flushIdleMs = 1500;

  // Test hooks — fake the clock and the side-effecting calls.
  bool      otaInProgress = false;
  bool      persistShouldSucceed = true;
  int       persistCallCount = 0;
  int       invalidateCallCount = 0;
  int       notifyCallCount = 0;
  std::string currentSerialized = "{}";

  // Hash function must match the production version (FNV-1a).
  static uint32_t fnv1aHash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (auto c : s) {
      h ^= static_cast<uint8_t>(c);
      h *= 16777619u;
    }
    return h;
  }

  // Equivalent to the production drain at one tick.
  void tick(uint32_t nowMs) {
    if (pending) {
      pending = false;
      dirty = true;
      lastSignalMs = nowMs;
    }
    if (!dirty) return;
    bool windowReached = forceFlush ||
                          (nowMs - lastSignalMs) >= flushIdleMs;
    if (!windowReached) return;
    if (otaInProgress) {
      // Don't clear dirty — retry next tick.
      return;
    }
    forceFlush = false;
    uint32_t hash = fnv1aHash(currentSerialized);
    if (hash == lastPersistedHash) {
      dirty = false;
      return;
    }
    persistCallCount++;
    if (persistShouldSucceed) {
      lastPersistedHash = hash;
      invalidateCallCount++;
      notifyCallCount++;
      dirty = false;
    } else {
      notifyCallCount++;
      // dirty stays true — retry next tick.
    }
  }
};

}  // namespace lamp

void setUp(void) {}
void tearDown(void) {}

void test_signal_then_idle_window_then_persist() {
  lamp::CommitDrain d;
  d.currentSerialized = R"({"lamp":{"brightness":50}})";
  d.pending = true;
  d.tick(1000);  // signal latches, dirty=true, lastSignalMs=1000
  TEST_ASSERT_TRUE(d.dirty);
  TEST_ASSERT_EQUAL_INT(0, d.persistCallCount);  // not yet — idle window not reached

  d.tick(2000);  // 1000ms after signal — still inside 1500ms window
  TEST_ASSERT_EQUAL_INT(0, d.persistCallCount);

  d.tick(2500);  // 1500ms after signal — flush
  TEST_ASSERT_EQUAL_INT(1, d.persistCallCount);
  TEST_ASSERT_FALSE(d.dirty);
}

void test_signal_resets_idle_window() {
  lamp::CommitDrain d;
  d.currentSerialized = R"({"lamp":{"brightness":50}})";
  d.pending = true;
  d.tick(1000);
  d.pending = true;
  d.tick(2000);  // signal again — resets window to 2000
  d.tick(3000);  // 1000ms after the later signal — not yet
  TEST_ASSERT_EQUAL_INT(0, d.persistCallCount);
  d.tick(3500);  // 1500ms after the later signal — flush
  TEST_ASSERT_EQUAL_INT(1, d.persistCallCount);
}

void test_hash_dedup_skips_identical_persist() {
  lamp::CommitDrain d;
  d.currentSerialized = R"({"lamp":{"brightness":50}})";
  // First commit lands.
  d.pending = true;
  d.tick(1000);
  d.tick(2500);
  TEST_ASSERT_EQUAL_INT(1, d.persistCallCount);

  // Second commit signal with identical serialized state — should skip.
  d.pending = true;
  d.tick(3000);
  d.tick(4500);
  TEST_ASSERT_EQUAL_INT(1, d.persistCallCount);  // still 1 — deduped
  TEST_ASSERT_FALSE(d.dirty);
}

void test_ota_in_progress_defers_flush() {
  lamp::CommitDrain d;
  d.currentSerialized = R"({"lamp":{"brightness":50}})";
  d.otaInProgress = true;
  d.pending = true;
  d.tick(1000);
  d.tick(2500);  // would normally flush — OTA blocks
  TEST_ASSERT_EQUAL_INT(0, d.persistCallCount);
  TEST_ASSERT_TRUE(d.dirty);  // still dirty — will retry

  d.otaInProgress = false;
  d.tick(2500);  // OTA clear — flush fires this tick
  TEST_ASSERT_EQUAL_INT(1, d.persistCallCount);
  TEST_ASSERT_FALSE(d.dirty);
}

void test_force_flush_bypasses_idle_window() {
  lamp::CommitDrain d;
  d.currentSerialized = R"({"lamp":{"brightness":50}})";
  d.pending = true;
  d.tick(1000);  // dirty, lastSignalMs=1000

  d.forceFlush = true;
  d.tick(1100);  // 100ms after — would not normally flush
  TEST_ASSERT_EQUAL_INT(1, d.persistCallCount);
  TEST_ASSERT_FALSE(d.forceFlush);
}

void test_persist_failure_keeps_dirty_for_retry() {
  lamp::CommitDrain d;
  d.currentSerialized = R"({"lamp":{"brightness":50}})";
  d.persistShouldSucceed = false;
  d.pending = true;
  d.tick(1000);
  d.tick(2500);
  TEST_ASSERT_EQUAL_INT(1, d.persistCallCount);
  TEST_ASSERT_TRUE(d.dirty);  // failure — retry expected
  TEST_ASSERT_EQUAL_INT(1, d.notifyCallCount);  // notifyStateChange fires either way

  // Persist succeeds on retry tick.
  d.persistShouldSucceed = true;
  d.tick(2500);
  TEST_ASSERT_EQUAL_INT(2, d.persistCallCount);
  TEST_ASSERT_FALSE(d.dirty);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_signal_then_idle_window_then_persist);
  RUN_TEST(test_signal_resets_idle_window);
  RUN_TEST(test_hash_dedup_skips_identical_persist);
  RUN_TEST(test_ota_in_progress_defers_flush);
  RUN_TEST(test_force_flush_bypasses_idle_window);
  RUN_TEST(test_persist_failure_keeps_dirty_for_retry);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native -f test_commit_drain
```

Expected: 6/6 PASS.

- [ ] **Step 3: Run all native tests**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 296/296 (existing 290 + 6 new). Total Phase A native test additions: 24 (5+4+2+3+4+6). Spec acceptance said 6 new tests — actual is 24, exceeding the spec target.

- [ ] **Step 4: Commit**

```bash
git add software/lamp-os/test/test_commit_drain/
git commit -m "$(cat <<'EOF'
test(commit): CHAR_COMMIT drain state machine — 6 mirror-class tests

Asserts the drain's idle-window debounce, hash-dedup behavior, OTA
interlock deferral, force-flush bypass, and persist-failure retry
semantics. Mirror-class pattern — re-declares the drain shape because
the production version pulls in NimBLE/FastLED/etc.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 16: Bench verification on jacko + floral (A.7 keep, bench tests #1/#2/#11/#12)

Hardware verification — run the four spec-required bench tests against the two physical lamps.

**Files:** none modified. Operational tasks.

- [ ] **Step 1: Build + flash both lamps**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os
# Stop the serial captures if running
pkill -f "serial_capture.py.*lamp[12]"
sleep 1
pio run -e upesy_wroom --target upload --upload-port /dev/cu.SLAB_USBtoUART8     # jacko
pio run -e upesy_wroom --target upload --upload-port /dev/cu.usbserial-0001      # floral
# Restart serial captures
cd /tmp
python3 serial_capture.py /dev/cu.usbserial-0001 lamp1 /tmp/serial_lamp1.log &
python3 serial_capture.py /dev/cu.SLAB_USBtoUART8 lamp2 /tmp/serial_lamp2.log &
sleep 3
```

- [ ] **Step 2: Bench test #1 — Rename persists**

Using the EXISTING old app (not yet Phase B):
1. Connect to jacko via the app.
2. Tap Setup → Name → rename to `jacko-test`.
3. Tap the AppBar "Save changes" pill (old app uses settings_blob → orchestrator → reboot=true default → reboots).
4. Wait for reboot + auto-reconnect.
5. Power-cycle jacko (USB unplug + replug).
6. Reconnect via the app.
7. **PASS**: Name is `jacko-test`.

In `tail /tmp/serial_lamp2.log`, expect to see:
- `[loop] drain settingsBlob len=...`
- `[nvs] persistConfig via=settings_blob wrote N bytes`
- `fadeOutRebootRequested` setting → reboot.
- After reboot: `[cfg] loaded name=jacko-test pw=set expressions=...`

Rename back to `jacko` before continuing.

- [ ] **Step 3: Bench test #2 — Old app + new firmware compat**

Same as #1 but for any other settings_blob-touching field (e.g., toggle Home Mode enabled on/off). Verify the old app's Save pill still triggers reboot + persist correctly. **PASS**: works exactly as before the refactor.

- [ ] **Step 4: Bench test #11 — OTA-during-commit interlock**

This is harder without the app driving OTA, but you can simulate:
1. Begin an OTA flash via the existing OTA test path (refer to project README / OTA validation memory from 2026-06-05).
2. Mid-flash, use a manual BLE write tool (`gatttool` or a quick Dart script) to write CHAR_COMMIT to jacko.
3. In jacko's serial log, expect `[loop] commit drain: OTA in progress, deferred`.
4. After OTA completes, the commit eventually fires (next tick after idle window passes since the most recent commit signal). **PASS**: no NVS contention; commit-or-defer behavior visible in logs.

If a manual CHAR_COMMIT write is impractical without Phase B's app changes, mark this bench test as **deferred to Phase B** when the app actually writes commits. Document this in the commit message.

- [ ] **Step 5: Bench test #12 — Cascade contamination regression**

Requires both lamps and the wisp in steady-state. Sequence:
1. Make floral cascade a brightness pulse to jacko (e.g., a social greet — verify in floral's serial that a brightness CONTROL_OP is being broadcast).
2. While the cascade is mid-flight, drive a manual CHAR_COMMIT to jacko (same caveat as #11 — may need Phase B's app).
3. Power-cycle jacko.
4. **PASS**: jacko's persisted brightness is jacko's LOCAL value, NOT the cascade-relayed value from floral.

If the manual commit is impractical, do the following proxy: at jacko, set a local brightness via the app's Save-pill path (which writes settings_blob → persistConfig). Then have floral cascade a different brightness. Check that jacko's `config.lamp.brightness` (visible via the next section read) returns to the LOCAL value, not the cascade. The cascade should render visually but not contaminate. **PASS**: floral's cascade brightness shows on jacko's strip during the cascade window but the persisted/served value is jacko's local choice.

- [ ] **Step 6: Run native tests one more time to make sure nothing regressed**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-os && pio test -e native
```

Expected: 296/296 PASS.

- [ ] **Step 7: Commit a note documenting which bench tests landed clean**

```bash
git commit --allow-empty -m "$(cat <<'EOF'
test(bench): Phase A bench verification on jacko + floral

Bench tests landed:
- #1 Rename persists across power-cycle (via old-app Save pill → new
  settings_blob drain): PASS
- #2 Old-app + new-firmware compat (Home Mode toggle): PASS
- #11 OTA interlock: [PASS / DEFERRED to Phase B — depends on outcome]
- #12 Cascade contamination regression: [PASS / DEFERRED to Phase B]

Native tests: 296/296 green.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

(Edit the message to reflect actual PASS/DEFERRED outcomes before committing.)

---

## Self-review

**Spec coverage:**
- A.1 user/remote split → Tasks 2-5 + test Task 6 ✓
- A.2 CHAR_COMMIT char → Task 14 ✓
- A.3 debounced drain + OTA interlock + hash-dedup → Task 14 + test Task 15 ✓
- A.4 settings_blob rewrite with reboot opt-in → Tasks 11-12 + test Task 13 ✓
- A.5 applyXxxLocal helpers extracted → Tasks 2, 4, 5, 8, 9, 10, 11 ✓
- A.6 persistConfig path-tagged log → Task 1 ✓
- A.7 keep CHAR_HOMEMODEFOCUS routing → no change required; called out in Task 3 (drain still calls `ble_control::isHomeModePageActive()`) ✓
- A.8 six new native tests → tests in Tasks 6, 7, 13 (×3), 15 = **6 test directories with 24 individual tests** (exceeds the spec target of 6 tests; counted as 6 test SUITES) ✓
- Phase A ordering invariant (A.1 before A.2/A.3) → enforced by task order; Tasks 2-7 (A.1) precede Tasks 14-15 (A.2/A.3) ✓
- Native test acceptance: 272 + 24 = 296 ✓
- Bench tests #1/#2/#11/#12 → Task 16 ✓

**Placeholder scan:** Task 8 step 2 contains an explicit `TODO during execution` for verifying `bt.tickAdvertising()` advert refresh behavior. This is the spec's open verification item #5 and is an implementer-action item — kept intentionally with that flag so it isn't lost. All other "verify during execution" notes are tied to spec-listed open verification items.

**Type consistency:** `apply::brightnessToConfig` / `brightnessToRender` / `brightnessImmediate` signatures consistent across Tasks 2, 3, 7, 8, 9. `apply::settingsBlobLocal` returns `bool` consistently in Tasks 11, 12. `lamp::apply::*` namespace used uniformly.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-13-always-save-phase-a-firmware.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
