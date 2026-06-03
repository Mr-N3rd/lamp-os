#include "configurator.hpp"

#include <Arduino.h>

#include <cstdint>

#include "util/color.hpp"
#include "util/fade.hpp"

namespace lamp {

bool ConfiguratorBehavior::fadeActive(uint32_t nowMs) const {
  if (fadeDurationMs_ == 0) return false;
  return (nowMs - fadeStartMs_) < fadeDurationMs_;
}

void ConfiguratorBehavior::beginFade(const std::vector<Color>& targetColors,
                                     uint16_t fadeDurationMs) {
  // Snapshot the CURRENT buffer state — works for both a fresh-fade
  // and a mid-fade interrupt (the interrupt case picks up wherever
  // the interpolation has landed so far, so the override transition
  // doesn't snap back to the pre-fade baseline).
  const size_t n = fb ? static_cast<size_t>(fb->pixelCount) : 0;
  fadeFromColors_.assign(n, Color());
  if (fb && n > 0) {
    for (size_t i = 0; i < n; ++i) fadeFromColors_[i] = fb->buffer[i];
  }
  // Assign the target colors. Callers (ColorOverride::apply, the BLE
  // colors drain) have already done the gradient expansion to pixelCount
  // length, so we just copy.
  colors = targetColors;
  fadeStartMs_ = millis();
  fadeDurationMs_ = fadeDurationMs;
}

void ConfiguratorBehavior::draw() {
  // Phase C: duration-controlled fade. When fadeDurationMs_ is 0 (set by
  // legacy callers that never went through beginFade — i.e. boot-time
  // initBehaviors which assigns colors directly) or the window has
  // elapsed, write target colors directly. Otherwise per-pixel lerp
  // between fadeFromColors_[i] and colors[i] using the existing quad
  // ease-in-out LUT (via fade()).
  const uint32_t now = millis();
  const bool active = fadeActive(now);
  if (!active || fadeFromColors_.size() != colors.size()) {
    for (int i = 0; i < fb->pixelCount; i++) {
      fb->buffer[i] = colors[i];
    }
  } else {
    // 16-bit math is fine — fadeDurationMs_ ≤ 65535 and elapsed is bounded
    // by that. The fade() helper takes (start, end, steps, currentStep),
    // matches the existing easeFrames-based call site.
    const uint32_t elapsed = now - fadeStartMs_;
    const uint32_t duration = fadeDurationMs_;
    for (int i = 0; i < fb->pixelCount; i++) {
      fb->buffer[i] = fade(fadeFromColors_[i], colors[i], duration, elapsed);
    }
  }

  nextFrame();
};

void ConfiguratorBehavior::control() {
  // If disabled, fade out if needed then stop
  if (disabled) {
    if (animationState == PAUSED) {
      // If we're paused (showing preview), start fading out
      playOnce();
    } else if (animationState == PLAYING_ONCE && frame >= frames) {
      // Fade out complete, now stop
      stop();
    }
    // Don't process normal timeout logic when disabled
    return;
  }

  uint32_t now = millis();
  if (animationState == STOPPED) {
    if (lastWebSocketUpdateTimeMs > 0 &&
        now < lastWebSocketUpdateTimeMs + CONFIGURATOR_WEBSOCKET_TIMEOUT_MS) {
      playOnce();
    }
  }
  if (animationState == PLAYING_ONCE && frame == easeFrames) {
    pause();
  }
  if (animationState == PAUSED && now > lastWebSocketUpdateTimeMs + CONFIGURATOR_WEBSOCKET_TIMEOUT_MS) {
    playOnce();
  }
};
}  // namespace lamp