#pragma once

#include "core/animated_behavior.hpp"

namespace lamp {

/**
 * @brief Additive personality bleed layer.
 *
 * PersonalityBehavior is the visual counterpart to PersonalityEngine's
 * Fond color-bleed. It contributes a subtle per-pixel tint toward each
 * currently-Fond peer's color, alpha-blended on top of whatever lower
 * layers have already drawn this frame.
 *
 * Architectural shape:
 *   - Registered AFTER the configurator (so the bleed overlays the
 *     configurator's base/painted scene rather than getting wiped).
 *   - Registered BEFORE the fade-out so reboot animation stays on top.
 *   - Continuously in PLAYING state — control() flips STOPPED→PLAYING
 *     once at boot and stays there. draw() is a no-op when the engine
 *     reports zero contributions (no Fond peers in flight).
 *
 * Per-peer fade-in / fade-out state lives in PersonalityEngine;
 * PersonalityBehavior is a pure consumer of getBleedContributions().
 */
class PersonalityBehavior : public AnimatedBehavior {
  using AnimatedBehavior::AnimatedBehavior;

 public:
  void draw() override;
  void control() override;
};

}  // namespace lamp
