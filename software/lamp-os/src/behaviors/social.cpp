#include "social.hpp"

#include <Arduino.h>

#include "components/network/nearby_lamps.hpp"
#include "components/personality/personality_engine.hpp"
#include "config/config.hpp"
#include "util/color.hpp"
#include "util/fade.hpp"

namespace lamp {

namespace {

// Darken `c` toward zero by `strength`/255. strength=0 returns c;
// strength=255 returns black. Used by the Extrovert greeting's
// "subtle pulse-back" phase — gives the held color a brief dip before
// resettling. Per-channel; preserves the hue.
Color darken(const Color& c, uint8_t strength) {
  const uint16_t keep = static_cast<uint16_t>(255 - strength);
  return Color(static_cast<uint8_t>(c.r * keep / 255),
               static_cast<uint8_t>(c.g * keep / 255),
               static_cast<uint8_t>(c.b * keep / 255),
               static_cast<uint8_t>(c.w * keep / 255));
}

}  // namespace

void SocialBehavior::draw() {
  // Piecewise waveform: ease-in (to foundLampColor) → hold (optionally
  // with a subtle "pulse-back" dip during the first two-thirds) → ease-out
  // (back to whatever the underlying expression has drawn this frame).
  // pulseBackStrength == 0 reduces this to a plain ease-in / hold / ease-out
  // and matches the pre-Phase-D shape SocialBehavior used.
  const uint32_t easeIn  = easeInFrames;
  const uint32_t hold    = holdFrames;
  const uint32_t fadeOut = fadeOutFrames;

  for (int i = 0; i < fb->pixelCount; i++) {
    const Color buf = fb->buffer[i];
    Color out;
    if (easeIn > 0 && frame < easeIn) {
      // Phase 1 — ease in toward foundLampColor.
      out = fade(buf, foundLampColor, easeIn - 1, frame);
    } else if (frame < easeIn + hold) {
      const uint32_t holdFrame = frame - easeIn;
      const uint32_t pulseSpan = (pulseBackStrength > 0) ? (hold * 2 / 3) : 0;
      if (pulseSpan > 0 && holdFrame < pulseSpan) {
        const Color dimmed = darken(foundLampColor, pulseBackStrength);
        const uint32_t halfPulse = (pulseSpan > 0) ? (pulseSpan / 2) : 0;
        if (halfPulse > 0 && holdFrame < halfPulse) {
          // Pulse down: foundLampColor → dimmed.
          out = fade(foundLampColor, dimmed, halfPulse - 1, holdFrame);
        } else if (halfPulse > 0) {
          // Pulse up: dimmed → foundLampColor.
          out = fade(dimmed, foundLampColor, halfPulse - 1, holdFrame - halfPulse);
        } else {
          out = foundLampColor;
        }
      } else {
        // Plain hold.
        out = foundLampColor;
      }
    } else if (fadeOut > 0 && frame < easeIn + hold + fadeOut) {
      // Phase 3 — ease out back to the underlying expression's pixel.
      const uint32_t fadeFrame = frame - (easeIn + hold);
      out = fade(foundLampColor, buf, fadeOut - 1, fadeFrame);
    } else {
      // Past the explicit window — leave buffer alone (playOnce will stop
      // us at `frames` regardless).
      out = buf;
    }
    fb->buffer[i] = out;
  }

  nextFrame();
};

void SocialBehavior::control() {
  if (animationState != STOPPED) return;
  const uint32_t now = millis();
  // Wraparound-safe time comparison (millis() rolls over at ~49 days).
  // The re-greet check below uses the same idiom for consistency.
  if (static_cast<int32_t>(now - nextAcknowledgeTimeMs) < 0) return;

  const SocialMode mode = config_ ? config_->lamp.socialMode : SocialMode::Ambivert;

  // Ambivert fatigue gate — if we burnt out recently, take a breather.
  if (mode == SocialMode::Ambivert &&
      static_cast<int32_t>(now - tiredUntilMs_) < 0) {
    return;
  }

  uint32_t regreetWindowMs = 0;
  switch (mode) {
    case SocialMode::Extrovert: regreetWindowMs = 0; break;
    case SocialMode::Ambivert:  regreetWindowMs = AMBIVERT_REGREET_WINDOW_MS; break;
    case SocialMode::Introvert: regreetWindowMs = INTROVERT_REGREET_WINDOW_MS; break;
  }

  // Snapshot taken under nearbyLamps' mutex so iterating is safe against
  // the NimBLE scan task and the ESP-NOW recv task both writing.
  std::vector<NearbyLamp> foundLamps =
      nearbyLamps.getReachableViaBle(LAMP_PRUNE_TIME_MS);

  for (auto it = foundLamps.rbegin(); it != foundLamps.rend(); ++it) {
    // Re-greet window: even if the NearbyLamp's `acknowledged` flag was
    // reset (peer pruned + returned), enforce our own per-peer cooldown.
    if (regreetWindowMs > 0) {
      auto last = lastGreetedAtMs_.find(it->name);
      if (last != lastGreetedAtMs_.end() && now - last->second < regreetWindowMs) {
        continue;
      }
    }
    // Skip already-acknowledged (within the NearbyLamp lifetime) so we
    // don't greet the same peer twice in a single sighting.
    if (it->acknowledged) continue;

    // PersonalityEngine gates per-peer greeting via the (SocialMode ×
    // Disposition) matrix. Salty in every mode returns skip — the lamp's
    // only on-meet signal for a Salty peer is the arrival side-eye dim
    // (fired separately from PersonalityEngine, not here). We still mark
    // the peer acknowledged + record lastGreetedAtMs so we don't keep
    // re-evaluating them every loop iteration.
    const GreetingTuning tuning = personalityEngine.greetingFor(it->name);
    if (tuning.skip) {
#ifdef LAMP_DEBUG
      Serial.printf("[social] skip %s (Salty)\n", it->name.c_str());
#endif
      nearbyLamps.acknowledge(it->name);
      lastGreetedAtMs_[it->name] = now;
      continue;  // keep scanning for a non-Salty peer below
    }

#ifdef LAMP_DEBUG
    Serial.printf("[social] greet %s (mode=%u disp=%u frames=%u pulse=%u)\n",
                  it->name.c_str(), (unsigned)mode,
                  (unsigned)tuning.totalFrames,
                  (unsigned)tuning.pulseBackStrength);
#endif
    nearbyLamps.acknowledge(it->name);
    foundLampColor = it->baseColor;

    // Copy the engine's waveform into our draw-side fields. AnimatedBehavior's
    // `frames` drives playOnce / nextFrame — keep it in lockstep with totalFrames.
    easeInFrames      = tuning.easeInFrames;
    holdFrames        = tuning.holdFrames;
    fadeOutFrames     = tuning.fadeOutFrames;
    pulseBackStrength = tuning.pulseBackStrength;
    frames            = tuning.totalFrames;

    // Record into our persistent (in-memory) greeting log.
    lastGreetedAtMs_[it->name] = now;
    if (lastGreetedAtMs_.size() > MAX_GREETED_TRACKED) {
      // LRU eviction — drop the entry with the smallest timestamp.
      auto oldest = lastGreetedAtMs_.begin();
      for (auto i = lastGreetedAtMs_.begin(); i != lastGreetedAtMs_.end(); ++i) {
        if (i->second < oldest->second) oldest = i;
      }
      lastGreetedAtMs_.erase(oldest);
    }
    recentGreetMs_.push_back(now);

    // Per-mode cooldown.
    uint32_t cooldown = 0;
    switch (mode) {
      case SocialMode::Extrovert:
        cooldown = EXTROVERT_COOLDOWN_MS;
        break;
      case SocialMode::Ambivert:
        cooldown = LAMP_TIME_BETWEEN_ACKNOWLEDGEMENT_MS;
        break;
      case SocialMode::Introvert: {
        const uint32_t span =
            INTROVERT_RECHARGE_MAX_MS - INTROVERT_RECHARGE_MIN_MS;
        const uint32_t recharge =
            INTROVERT_RECHARGE_MIN_MS + (esp_random() % span);
        cooldown = INTROVERT_BASE_COOLDOWN_MS + recharge;
        break;
      }
    }
    nextAcknowledgeTimeMs = now + cooldown;

    // Ambivert: trim the fatigue window, enter "tired" if we've burned
    // through too many greetings recently.
    if (mode == SocialMode::Ambivert) {
      while (!recentGreetMs_.empty() &&
             now - recentGreetMs_.front() > AMBIVERT_FATIGUE_WINDOW_MS) {
        recentGreetMs_.erase(recentGreetMs_.begin());
      }
      if (recentGreetMs_.size() >= AMBIVERT_FATIGUE_COUNT) {
        tiredUntilMs_ = now + AMBIVERT_TIRED_DURATION_MS;
        recentGreetMs_.clear();
#ifdef LAMP_DEBUG
        Serial.printf("[social] ambivert tired until +%u ms\n",
                      (unsigned)AMBIVERT_TIRED_DURATION_MS);
#endif
      }
    }

    playOnce();
    break;
  }
};

}  // namespace lamp
