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
//
// Dependencies pulled in by the include chain:
//   util/levels.hpp        — lamp::calculateBrightnessLevel
//   standard_lamp.hpp      — LAMP_MAX_BRIGHTNESS, strip handles
//   config/config.hpp      — lamp::Config (config lives at ::config in standard_lamp.cpp)

#pragma once

#include <Adafruit_NeoPixel.h>
#include <cstdint>

#include "config/config.hpp"
#include "lamps/standard_lamp.hpp"
#include "util/levels.hpp"

// Strip handles and config defined in standard_lamp.cpp at global (non-lamp)
// scope.
extern Adafruit_NeoPixel* shadeStrip;
extern Adafruit_NeoPixel* baseStrip;
// config is defined as `lamp::Config config;` at file scope in
// standard_lamp.cpp — i.e., it lives at ::config, not ::lamp::config.
extern lamp::Config config;

namespace lamp {

// Micro-fade triple — file-static in standard_lamp.cpp. Exposed via
// these accessors so apply_brightness.hpp can read/write without
// pulling in the rest of the file. Definitions live inside
// `namespace lamp { ... }` in standard_lamp.cpp (Task 2 Step 3).
uint8_t  brightnessFadeSource();
uint8_t  brightnessFadeTarget();
uint32_t brightnessFadeStartMs();
bool     brightnessFadeSeeded();
void     setBrightnessFade(uint8_t source, uint8_t target, uint32_t startMs);
void     clearBrightnessFadeSeed();
uint8_t  computeUserBrightnessNow(uint32_t nowMs);

// Bookkeeping the brightness drain has always done.
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
    ::config.homeMode.brightness = level;
  } else {
    ::config.lamp.brightness = level;
  }
  const uint32_t fadeNow = ::millis();
  ::lamp::stampConfiguratorActivity(fadeNow);
  const uint8_t source = ::lamp::brightnessFadeSeeded()
                             ? ::lamp::computeUserBrightnessNow(fadeNow)
                             : level;
  ::lamp::setBrightnessFade(source, level, fadeNow);
  // Apply initial sample so the strip starts moving this drain cycle.
  // (The compositor's per-tick interpolation handles the rest.)
  if (::shadeStrip) {
    ::shadeStrip->setBrightness(
        ::lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, source));
  }
  if (::baseStrip) {
    ::baseStrip->setBrightness(
        ::lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, source));
  }
}

// Mesh-relayed cascade brightness. Render-only: snap setBrightness,
// no config mutation, no fade triple. Skipping the fade gives the
// cascade its "instant change" UX (matches today's social-greet behavior
// for shade/base color cascades).
inline void brightnessToRender(uint8_t level, bool isHomeMode) {
  (void)isHomeMode;  // Cascade brightness isn't home-mode-routed.
  if (::shadeStrip) {
    ::shadeStrip->setBrightness(
        ::lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
  }
  if (::baseStrip) {
    ::baseStrip->setBrightness(
        ::lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
  }
}

// settings_blob path. Saved value semantics — no fade UX. Mutates
// config, applies via applyEffectiveBrightness (which respects the
// dimming/social-disposition multipliers), and resets the slider fade
// seed so a subsequent slider drag starts from the new persisted level.
inline void brightnessImmediate(uint8_t level, bool isHomeMode) {
  if (isHomeMode) {
    ::config.homeMode.brightness = level;
  } else {
    ::config.lamp.brightness = level;
  }
  ::lamp::clearBrightnessFadeSeed();
  ::lamp::applyEffectiveBrightness();
}

}  // namespace apply
}  // namespace lamp
