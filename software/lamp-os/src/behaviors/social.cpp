#include "social.hpp"

#include <Arduino.h>

#include "components/network/nearby_lamps.hpp"
#include "config/config.hpp"
#include "util/color.hpp"
#include "util/fade.hpp"

namespace lamp {

void SocialBehavior::draw() {
  for (int i = 0; i < fb->pixelCount; i++) {
    if (frame < easeFrames) {
      fb->buffer[i] = fade(fb->buffer[i], foundLampColor, easeFrames - 1, frame);
    } else if (frame > (frames - easeFrames)) {
      fb->buffer[i] = fade(foundLampColor, fb->buffer[i], easeFrames - 1, frame % easeFrames);
    } else {
      fb->buffer[i] = foundLampColor;
    }
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

#ifdef LAMP_DEBUG
    Serial.printf("[social] greet %s (mode=%u)\n", it->name.c_str(),
                  (unsigned)mode);
#endif
    nearbyLamps.acknowledge(it->name);
    foundLampColor = it->baseColor;

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
