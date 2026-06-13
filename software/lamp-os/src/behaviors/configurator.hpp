#pragma once

#include <cstdint>
#include <vector>

#include "config/config_types.hpp"
#include "core/animated_behavior.hpp"
#include "util/color.hpp"

#define CONFIGURATOR_WEBSOCKET_TIMEOUT_MS 60000

/**
 * @brief a layer to preview realtime changes from the web
 *        configuration tool
 */
namespace lamp {

// Phase C: duration-controlled fade replaces the old frame-counted ease.
// kDefaultFadeMs is the value the configurator uses when nothing has called
// beginFade() since the last colors-write — picked to match the pre-Phase-C
// "easeFrames=60 @ ~60fps ≈ 1s, perceptually a tad sharper than that under
// the quad ease curve" UX so BLE color picker drags look the same. The
// override path overrides this per-call (the wisp typically asks for 100ms
// snap-in, peer-swap might ask for several hundred ms breathe).
constexpr uint16_t kDefaultFadeMs = 250;

class ConfiguratorBehavior : public AnimatedBehavior {
  using AnimatedBehavior::AnimatedBehavior;

 public:
  // Legacy frame-count knob preserved for compat with the existing
  // control() state-machine timing; the actual per-pixel interpolation now
  // uses fadeStartMs_/fadeDurationMs_.
  uint32_t easeFrames = 60;
  uint8_t brightness = 100;
  std::vector<Color> colors;
  unsigned long lastWebSocketUpdateTimeMs = 0;
  bool disabled = false;  // Can be disabled during expression previews

  void draw() override;

  void control() override;

  // Phase C: drive a duration-controlled fade from the current buffer state
  // to `targetColors`. Snapshots the current buffer into fadeFromColors_,
  // assigns `targetColors` as the new `colors` (which is what draw() will
  // eventually write directly once the fade window elapses), records the
  // start timestamp and duration. Calling beginFade() while a fade is in
  // progress snapshots the CURRENT interpolated buffer as the new "from"
  // so the transition stays smooth (no rubber-banding back to the old
  // start). When `fadeDurationMs == 0` (or `targetColors` is empty) this
  // is effectively an instant set.
  void beginFade(const std::vector<Color>& targetColors,
                 uint16_t fadeDurationMs);

  // Phase C audit: ColorOverride/BLE color writes need to read back the
  // fade-tracking state to decide whether the configurator is mid-fade.
  // Exposed publicly so the override modules can drive the transition
  // without owning their own per-pixel interpolation.
  uint32_t fadeStartMs() const { return fadeStartMs_; }
  uint16_t fadeDurationMs() const { return fadeDurationMs_; }
  bool fadeActive(uint32_t nowMs) const;

 private:
  // The buffer snapshot taken at beginFade() time. Allocated once per
  // beginFade(); per-pixel interp in draw() walks it alongside `colors`.
  // Sized to pixelCount on first beginFade() and resized only when the
  // pixel count changes (rare — only on configuration boot).
  std::vector<Color> fadeFromColors_;
  uint32_t fadeStartMs_ = 0;
  uint16_t fadeDurationMs_ = 0;
};

}  // namespace lamp
