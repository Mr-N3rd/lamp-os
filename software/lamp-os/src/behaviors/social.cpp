#include "social.hpp"

#include <Arduino.h>
#include <cstring>

#include "components/network/nearby_lamps.hpp"
#include "components/personality/personality_engine.hpp"
#include "components/firmware/firmware_distributor.hpp"
#include "components/firmware/firmware_receiver.hpp"
#include "config/config.hpp"
#include "util/color.hpp"
#include "util/fade.hpp"
#include "version.hpp"

// Defined in standard_lamp.cpp at global scope (the
// `lamp::firmwareReceiver` pattern isn't used because firmware_receiver.hpp
// doesn't expose an extern decl). Used by otaPulseMultiplier to detect
// receiver-role pulse cadence.
extern lamp::FirmwareReceiver firmwareReceiver;

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
    } else if (otaHoldActive_ &&
               firmwareDistributor.isDistributingTo(otaHoldPeerMac_)) {
      // Phase 4 (OTA-hold) — past the normal fadeOut window AND we're
      // mid-OTA to the peer we just greeted. Hold the peer's color so
      // the brightness pulse modulates on a meaningful hue rather than
      // snapping back to the underlying expression mid-update. The
      // distributor's session naturally terminates (success / fail /
      // pivot) — `isDistributingTo` flips false at that point and we
      // fall through to the else below, returning the shade to the
      // underlying buffer.
      out = foundLampColor;
    } else {
      // Past the explicit window — leave buffer alone (playOnce will stop
      // us at `frames` regardless). Also clear the OTA-hold flag here so
      // the next greeting starts with a fresh state regardless of how
      // this session ended.
      if (otaHoldActive_) otaHoldActive_ = false;
      out = buf;
    }
    fb->buffer[i] = out;
  }

  nextFrame();
};

void SocialBehavior::control() {
  if (animationState != STOPPED) return;
  const uint32_t now = millis();

  // -- Gossip OTA tick (Phase 5b'.4) -------------------------------------
  // Fires INDEPENDENTLY of the social cooldowns/dispositions below. The
  // user's intent (locked in the design): "personality controls the
  // visual greeting, not whether firmware propagates. Version updates
  // outrank social preference." So this loop runs even when:
  //   - The greeting cooldown gate (just below) would skip greeting
  //   - The Ambivert fatigue window blocks greeting
  //   - A peer's disposition is Salty (skipped for greeting elsewhere)
  //
  // The distributor's per-peer backoff (10 min after a failure, idempotent
  // on Idle re-trigger) handles dedup so calling considerPeerForOta on
  // every social tick is safe.
  //
  // Two throttles to keep this cheap:
  //   (1) Skip entirely while distributor.isInProgress() — the single-
  //       source mutex blocks concurrent sessions, so scanning during
  //       OTA finds nothing actionable.
  //   (2) Throttle the ESP-NOW vector snapshot to kOtaScanIntervalMs
  //       (500 ms). Lamps emit HELLO every 1-2s, so this still catches
  //       every fresh sighting while collapsing the 60 Hz tick rate.
  //
  // Iterates ESP-NOW-reachable peers (not BLE) because firmwareVersion is
  // only populated by ESP-NOW HELLO; BLE-only sightings carry no version.
  if (!firmwareDistributor.isInProgress() &&
      (lastOtaScanMs_ == 0 ||
       static_cast<int32_t>(now - lastOtaScanMs_) >=
           static_cast<int32_t>(kOtaScanIntervalMs))) {
    lastOtaScanMs_ = now;
    std::vector<NearbyLamp> espNowPeers =
        nearbyLamps.getReachableViaEspNow(LAMP_PRUNE_TIME_MS);
    for (const auto& p : espNowPeers) {
      if (!p.hasMac) continue;
      if (p.firmwareVersion == 0) continue;
      if (p.firmwareVersion >= lamp::FIRMWARE_VERSION) continue;
      firmwareDistributor.considerPeerForOta(p.mac, p.firmwareVersion, now);
    }
  }

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

    // OTA-hold: if we're mid-OTA to the peer we're about to greet, extend
    // the animation lifetime to give the OTA-hold draw branch room to
    // run. Distributor session lifetime is ~15-45s for a 1.5 MB image;
    // kOtaHoldFrameBudget (~60s at 60fps) covers that with slack.
    if (it->hasMac && firmwareDistributor.isDistributingTo(it->mac)) {
      otaHoldActive_ = true;
      std::memcpy(otaHoldPeerMac_, it->mac, 6);
      frames += kOtaHoldFrameBudget;
    } else {
      // Defensive: clear any stale flag from a prior greeting whose OTA
      // already finished but where the next greeting fired before the
      // draw-side clear ran (e.g. greeting cooldown is short).
      otaHoldActive_ = false;
    }

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

// Returns the brightness multiplier from a SINGLE-pulse cadence.
// Pattern repeats: dip → hold → return → pause. Falls and rises are
// linear; the perceptible "easing" comes from the explicit hold beat
// at the minimum, not from a smoothed curve. Tested looks fine on
// hardware at the configured tunables — bumping to a parabolic ease
// would gain little visual difference for the multiply-per-frame cost.
static uint8_t singlePulseMul(uint32_t phaseMs) {
  const uint32_t period =
      SocialBehavior::kOtaPulseDipMs +
      SocialBehavior::kOtaPulseHoldMs +
      SocialBehavior::kOtaPulseReturnMs +
      SocialBehavior::kOtaPulsePauseMs;
  const uint32_t t = phaseMs % period;
  // Slice the period into the four phases.
  if (t < SocialBehavior::kOtaPulseDipMs) {
    // Linear fall 100 → kOtaPulseMinPct over kOtaPulseDipMs.
    const float f = static_cast<float>(t) / SocialBehavior::kOtaPulseDipMs;
    const float dip = (100 - SocialBehavior::kOtaPulseMinPct) * f;
    return static_cast<uint8_t>(100.0f - dip);
  }
  uint32_t off = SocialBehavior::kOtaPulseDipMs;
  if (t < off + SocialBehavior::kOtaPulseHoldMs) {
    return SocialBehavior::kOtaPulseMinPct;
  }
  off += SocialBehavior::kOtaPulseHoldMs;
  if (t < off + SocialBehavior::kOtaPulseReturnMs) {
    // Linear rise kOtaPulseMinPct → 100 over kOtaPulseReturnMs.
    const float f = static_cast<float>(t - off) / SocialBehavior::kOtaPulseReturnMs;
    const float rise = (100 - SocialBehavior::kOtaPulseMinPct) * f;
    return static_cast<uint8_t>(SocialBehavior::kOtaPulseMinPct + rise);
  }
  // Pause — return at full brightness.
  return 100;
}

// Returns the brightness multiplier from a DOUBLE-pulse cadence.
// Pattern: dip → return → short gap → dip → return → long pause.
static uint8_t doublePulseMul(uint32_t phaseMs) {
  // Combine two short pulses (shorter dip + return) followed by a longer
  // pause. A "short" pulse uses half the single-pulse dip/return for
  // tighter rhythm so the doubles read as a distinct pattern.
  const uint32_t shortDip    = SocialBehavior::kOtaPulseDipMs / 2;
  const uint32_t shortReturn = SocialBehavior::kOtaPulseReturnMs / 2;
  const uint32_t period =
      (shortDip + shortReturn) * 2 +
      SocialBehavior::kOtaPulseDoubleGapMs +
      SocialBehavior::kOtaPulseDoublePauseMs;
  const uint32_t t = phaseMs % period;
  uint32_t off = 0;

  auto dipPhase = [&](uint32_t windowStart) -> int {
    // -1 → not in this dip; 0..100 → multiplier inside the dip+return.
    if (t < windowStart || t >= windowStart + shortDip + shortReturn) return -1;
    const uint32_t rel = t - windowStart;
    if (rel < shortDip) {
      const float f = static_cast<float>(rel) / shortDip;
      const float dip = (100 - SocialBehavior::kOtaPulseMinPct) * f;
      return static_cast<int>(100.0f - dip);
    }
    const float f = static_cast<float>(rel - shortDip) / shortReturn;
    const float rise = (100 - SocialBehavior::kOtaPulseMinPct) * f;
    return static_cast<int>(SocialBehavior::kOtaPulseMinPct + rise);
  };

  int v = dipPhase(off);
  if (v >= 0) return static_cast<uint8_t>(v);
  off += shortDip + shortReturn + SocialBehavior::kOtaPulseDoubleGapMs;

  v = dipPhase(off);
  if (v >= 0) return static_cast<uint8_t>(v);

  // Long pause.
  return 100;
}

uint8_t SocialBehavior::otaPulseMultiplier(uint32_t nowMs) const {
  // Distributor sending = single-pulse. Receiver receiving = double-pulse.
  // If both somehow report in-progress (shouldn't happen with the
  // single-source mutex), prefer the receiver pattern — the lamp is the
  // one being updated, more important to surface.
  if (firmwareReceiver.isInProgress()) {
    return doublePulseMul(nowMs);
  }
  if (firmwareDistributor.isInProgress()) {
    return singlePulseMul(nowMs);
  }
  return 100;
}

}  // namespace lamp
