#include "personality.hpp"

#include <Arduino.h>

#include <vector>

#include "components/personality/personality_engine.hpp"
#include "util/color.hpp"

namespace lamp {

namespace {

// Alpha-blend `out` toward `c` by `alpha` in [0, 1]. Per-channel saturating
// arithmetic — alpha is small (<= kBleedMaxAlpha = 0.6) so the blend nudges
// the underlying pixel toward the peer color without washing it out.
inline void blendInto(Color& out, const Color& c, float alpha) {
  const float keep = 1.0f - alpha;
  const float r = static_cast<float>(out.r) * keep + static_cast<float>(c.r) * alpha;
  const float g = static_cast<float>(out.g) * keep + static_cast<float>(c.g) * alpha;
  const float b = static_cast<float>(out.b) * keep + static_cast<float>(c.b) * alpha;
  const float w = static_cast<float>(out.w) * keep + static_cast<float>(c.w) * alpha;
  out.r = static_cast<uint8_t>(r > 255.0f ? 255.0f : (r < 0.0f ? 0.0f : r));
  out.g = static_cast<uint8_t>(g > 255.0f ? 255.0f : (g < 0.0f ? 0.0f : g));
  out.b = static_cast<uint8_t>(b > 255.0f ? 255.0f : (b < 0.0f ? 0.0f : b));
  out.w = static_cast<uint8_t>(w > 255.0f ? 255.0f : (w < 0.0f ? 0.0f : w));
}

}  // namespace

void PersonalityBehavior::draw() {
  const auto contributions =
      personalityEngine.getBleedContributions(millis());
  if (!contributions.empty()) {
    for (int i = 0; i < fb->pixelCount; ++i) {
      Color out = fb->buffer[i];
      for (const auto& c : contributions) {
        blendInto(out, c.color, c.alpha);
      }
      fb->buffer[i] = out;
    }
  }
  nextFrame();
}

void PersonalityBehavior::control() {
  // Perpetually-playing: once draw() starts running each frame the engine
  // gates the visual output by emitting zero contributions when no Fond
  // peer is present. Keep the behavior in PLAYING state so the compositor
  // calls draw() every frame regardless.
  if (animationState == STOPPED) {
    play();
  }
}

}  // namespace lamp
