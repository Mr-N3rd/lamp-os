#include "components/personality/personality_engine.hpp"

#include <algorithm>
#include <cmath>

#include "components/network/show_receiver.hpp"
#include "config/config.hpp"
#include "expressions/expression_invocation.hpp"
#include "expressions/expression_manager.hpp"

namespace lamp {

PersonalityEngine personalityEngine;

void PersonalityEngine::begin(Config* config,
                              ExpressionManager* expressionManager,
                              ShowReceiver* showReceiver) {
  config_ = config;
  expressionManager_ = expressionManager;
  showReceiver_ = showReceiver;
  if (config_) {
    lastSocialMode_ = config_->lamp.socialMode;
  }
}

void PersonalityEngine::tick(uint32_t nowMs) {
  if (!config_) return;

  // Mode transition trip: if SocialMode changed AND the new mode isn't
  // Introvert, force one re-apply so the dim cleanly releases. If the new
  // mode IS Introvert, the next sample tick will pick up the crowd-driven
  // factor and apply it.
  const SocialMode currentMode = config_->lamp.socialMode;
  if (currentMode != lastSocialMode_) {
    lastSocialMode_ = currentMode;
    if (currentMode != SocialMode::Introvert && crowdDimFactor_ < 1.0f) {
      crowdDimFactor_ = 1.0f;
      lastCommittedLevel_ = 100;
      pendingApply_ = true;
    }
    // Reset the smoother so a fresh Introvert period starts clean.
    sampleCount_ = 0;
    emaSeeded_ = false;
  }

  // Per-tick work that doesn't gate on the 1 Hz sample cadence —
  // arrival detection should be reactive (a Smitten peer walking in
  // shouldn't wait up to 1s for the pulse). Cheap when no peers changed
  // because the snapshot + map lookup is small.
  const std::vector<NearbyLamp> peers = snapshotBlePeers_();
  tickArrivals_(nowMs, peers);
  tickClosestSmittenPulse_(nowMs, peers);
  tickFollowUpPulses_(nowMs);
  tickSaltyRestore_(nowMs);
  tickBleed_(nowMs, peers);

  // 1 Hz sample cadence — crowd-dim sampling lives here so the median
  // window is uniformly-spaced regardless of loop jitter. Reuses the
  // `peers` snapshot from above instead of re-calling snapshotBlePeers_()
  // — saves a vector copy + RSSI sort on every sample tick.
  if (nowMs - lastSampleMs_ >= kSamplePeriodMs || lastSampleMs_ == 0) {
    lastSampleMs_ = nowMs;
    sampleAndSmoothCrowd_(nowMs, peers);
  }
}

uint8_t PersonalityEngine::applyCrowdDim(uint8_t baseline) const {
  if (!config_ || config_->lamp.socialMode != SocialMode::Introvert) {
    return baseline;
  }
  if (crowdDimFactor_ >= 0.999f) return baseline;
  const float scaled = static_cast<float>(baseline) * crowdDimFactor_;
  // Round + floor at 1 — never let personality blank the lamp.
  uint8_t out = static_cast<uint8_t>(scaled + 0.5f);
  if (out < 1) out = 1;
  return out;
}

uint8_t PersonalityEngine::applySaltyDim(uint8_t baseline) const {
  if (!saltyActive_) return baseline;
  const float f = saltyFactorAt_(millis());
  if (f >= 0.999f) return baseline;
  const float scaled = static_cast<float>(baseline) * f;
  uint8_t out = static_cast<uint8_t>(scaled + 0.5f);
  if (out < 1) out = 1;
  return out;
}

float PersonalityEngine::saltyFactorAt_(uint32_t nowMs) const {
  if (!saltyActive_) return 1.0f;
  // Wraparound-safe elapsed time since arrival.
  const int32_t elapsed = static_cast<int32_t>(nowMs - saltyArrivalMs_);
  if (elapsed < 0) return 1.0f;
  const uint32_t e = static_cast<uint32_t>(elapsed);
  const uint32_t fadeIn  = kSaltySideEyeFadeInMs;
  const uint32_t hold    = kSaltySideEyeHoldMs;
  const uint32_t fadeOut = kSaltySideEyeFadeOutMs;
  if (e < fadeIn) {
    // 1.0 → kSaltySideEyeFactor linearly across fadeIn.
    const float t = static_cast<float>(e) / static_cast<float>(fadeIn);
    return 1.0f - (1.0f - kSaltySideEyeFactor) * t;
  }
  if (e < fadeIn + hold) {
    return kSaltySideEyeFactor;
  }
  if (e < fadeIn + hold + fadeOut) {
    const float t = static_cast<float>(e - fadeIn - hold) /
                    static_cast<float>(fadeOut);
    return kSaltySideEyeFactor + (1.0f - kSaltySideEyeFactor) * t;
  }
  return 1.0f;
}

namespace {

// Waveform profiles in (total, easeIn, hold, fadeOut, pulseBack) order.
// All frame counts assume a ~60 fps draw loop. pulseBack > 0 means the
// hold section opens with a brief dip toward darkness (then snaps back)
// before settling — the "subtle pulse-back into hold" the spec calls for.
struct Profile {
  uint32_t total;
  uint32_t easeIn;
  uint32_t hold;
  uint32_t fadeOut;
  uint8_t  pulseBack;
};

constexpr Profile kProfileSkip     = {0,   0,   0,   0,   0};
constexpr Profile kProfileMinimal  = {60,  12,  24,  24,  0};
constexpr Profile kProfileQuick    = {90,  18,  30,  42,  0};
constexpr Profile kProfileGentle   = {120, 30,  36,  54,  0};
constexpr Profile kProfileStandard = {150, 36,  48,  66,  0};
// pulseBack=100 → ~60% retention (40% dip). Originally 160 (37%
// retention), which audit flagged as too aggressive — at 60fps the
// down/up swing landed in glitch territory rather than the "subtle
// breath" the spec intended.
constexpr Profile kProfileWarm     = {180, 42,  60,  78,  100};
constexpr Profile kProfileEnthused = {240, 54,  96,  90,  100};
constexpr Profile kProfileFullShow = {300, 60,  120, 120, 100};

GreetingTuning toTuning(const Profile& p) {
  GreetingTuning t;
  t.totalFrames       = p.total;
  t.easeInFrames      = p.easeIn;
  t.holdFrames        = p.hold;
  t.fadeOutFrames     = p.fadeOut;
  t.pulseBackStrength = p.pulseBack;
  t.skip              = (p.total == 0);
  return t;
}

// (SocialMode × Disposition) → Profile. Salty is skip in every row —
// the lamp's only on-meet signal for Salty is the arrival side-eye dim
// (PersonalityEngine fires that directly; SocialBehavior never sees a
// non-skip tuning for Salty).
const Profile& profileFor(lamp::SocialMode mode, uint8_t disposition) {
  switch (disposition) {
    case 1:  // Salty
      return kProfileSkip;
    case 2:  // Wary
      switch (mode) {
        case lamp::SocialMode::Introvert: return kProfileMinimal;
        case lamp::SocialMode::Ambivert:  return kProfileMinimal;
        case lamp::SocialMode::Extrovert: return kProfileQuick;
      }
      return kProfileMinimal;
    case 4:  // Fond
      switch (mode) {
        case lamp::SocialMode::Introvert: return kProfileGentle;
        case lamp::SocialMode::Ambivert:  return kProfileWarm;
        case lamp::SocialMode::Extrovert: return kProfileEnthused;
      }
      return kProfileWarm;
    case 5:  // Smitten
      switch (mode) {
        case lamp::SocialMode::Introvert: return kProfileWarm;
        case lamp::SocialMode::Ambivert:  return kProfileEnthused;
        case lamp::SocialMode::Extrovert: return kProfileFullShow;
      }
      return kProfileEnthused;
    case 3:  // Neutral (also the default for unknown peers — see getDisposition)
    default:
      switch (mode) {
        case lamp::SocialMode::Introvert: return kProfileMinimal;
        case lamp::SocialMode::Ambivert:  return kProfileStandard;
        case lamp::SocialMode::Extrovert: return kProfileStandard;
      }
      return kProfileStandard;
  }
}

}  // namespace

GreetingTuning PersonalityEngine::greetingFor(const std::string& peerName) const {
  if (!config_) return toTuning(kProfileStandard);
  const uint8_t disp = config_->getDisposition(peerName);  // unknown → 3
  const SocialMode mode = config_->lamp.socialMode;
  return toTuning(profileFor(mode, disp));
}

#if defined(LAMP_TEST) || defined(LAMP_DEBUG)
void PersonalityEngine::setNearbyOverride(std::vector<NearbyLamp> peers) {
  nearbyOverride_ = std::move(peers);
  nearbyOverrideActive_ = true;
}

void PersonalityEngine::clearNearbyOverride() {
  nearbyOverride_.clear();
  nearbyOverrideActive_ = false;
}
#endif

// --- Crowd-dim internals -------------------------------------------------

float PersonalityEngine::weightForDisposition_(uint8_t d) const {
  switch (d) {
    case 1: return 2.0f;   // Salty — crowds you more
    case 2: return 1.5f;   // Wary
    case 3: return 1.0f;   // Neutral (also the default when peer is unknown)
    case 4: return 0.5f;   // Fond
    case 5: return 0.0f;   // Smitten — doesn't crowd you at all
    default: return 1.0f;  // out-of-range defensively maps to Neutral
  }
}

float PersonalityEngine::computeWeightedCount_(const std::vector<NearbyLamp>& peers) const {
  if (!config_) return 0.0f;
  float w = 0.0f;
  for (const auto& p : peers) {
    if (p.name.empty()) continue;
    const uint8_t d = config_->getDisposition(p.name);
    w += weightForDisposition_(d);
  }
  return w;
}

float PersonalityEngine::dimFactorForCount_(float weightedCount) const {
  if (weightedCount <= 0.0f) return 1.0f;
  // factor = max(kDimFloor, 1 - (1-kDimFloor) * log10(1+W) / log10(1+kCurveScaleW))
  //
  // kCurveScaleW is constexpr (= 10.0f), so log10(1+kCurveScaleW) = log10(11)
  // is also constant. Precompute the denominator at compile time so the only
  // float work left at runtime is one log10, one divide, and a few mul/adds.
  // Sample tick runs at 1 Hz so the savings are small, but free.
  static constexpr float kLog10OnePlusCurveScale = 1.0413927f;  // log10(11)
  const float numer = std::log10(1.0f + weightedCount);
  const float drop  = (1.0f - kDimFloor) * (numer / kLog10OnePlusCurveScale);
  float factor = 1.0f - drop;
  if (factor < kDimFloor) factor = kDimFloor;
  if (factor > 1.0f)      factor = 1.0f;
  return factor;
}

std::vector<NearbyLamp> PersonalityEngine::snapshotBlePeers_() const {
  // Gate must match setNearbyOverride() in the header — that method
  // compiles under (LAMP_TEST || LAMP_DEBUG), so the read side has to
  // honor the override in both build flavors. Without LAMP_DEBUG here,
  // the inject_nearby BLE test-action accepts the payload but the
  // engine silently keeps reading live BLE.
#if defined(LAMP_TEST) || defined(LAMP_DEBUG)
  if (nearbyOverrideActive_) return nearbyOverride_;
#endif
  return nearbyLamps.getReachableViaBle(LAMP_PRUNE_TIME_MS);
}

void PersonalityEngine::sampleAndSmoothCrowd_(
    uint32_t /*nowMs*/, const std::vector<NearbyLamp>& peers) {
  if (!config_) return;
  const float rawW = computeWeightedCount_(peers);

  // Insert into rolling buffer.
  sampleBuf_[sampleHead_] = rawW;
  sampleHead_ = (sampleHead_ + 1) % kSampleWindow;
  if (sampleCount_ < kSampleWindow) sampleCount_++;

  // Median of the active window.
  float sorted[kSampleWindow];
  for (size_t i = 0; i < sampleCount_; ++i) sorted[i] = sampleBuf_[i];
  std::sort(sorted, sorted + sampleCount_);
  const float median = (sampleCount_ % 2 == 1)
      ? sorted[sampleCount_ / 2]
      : 0.5f * (sorted[sampleCount_ / 2 - 1] + sorted[sampleCount_ / 2]);

  // EMA on top of the median.
  if (!emaSeeded_) {
    smoothedW_ = median;
    emaSeeded_ = true;
  } else {
    smoothedW_ = kEmaAlpha * median + (1.0f - kEmaAlpha) * smoothedW_;
  }

  // Compute target factor + target level at a nominal baseline of 100.
  const float targetFactor = dimFactorForCount_(smoothedW_);
  const uint8_t targetLevel = static_cast<uint8_t>(targetFactor * 100.0f + 0.5f);

  // Only commit when the change crosses the deadband AND the lamp is
  // Introvert (otherwise applyCrowdDim returns identity anyway, but we
  // want to keep the pendingApply flag honest).
  const int delta = static_cast<int>(targetLevel) - static_cast<int>(lastCommittedLevel_);
  const int absDelta = delta < 0 ? -delta : delta;
  if (absDelta >= kDeadbandLevels) {
    crowdDimFactor_ = targetFactor;
    lastCommittedLevel_ = targetLevel;
    if (config_->lamp.socialMode == SocialMode::Introvert) {
      pendingApply_ = true;
    }
  }
}

// --- Arrival / follow-up / closest cycle (Step 5) ----------------------

void PersonalityEngine::firePulse_(const Color& color) {
  if (!expressionManager_ || !showReceiver_) return;
  ExpressionInvocation inv;
  inv.type = "pulse";
  inv.colors = {color};
  inv.target = 3;  // BOTH
  inv.parameters["cycles"] = 2;
  uint8_t myMac[6] = {0};
  showReceiver_->getMyMac(myMac);
  (void)expressionManager_->triggerInvocation(inv, myMac);
}

void PersonalityEngine::tickArrivals_(uint32_t nowMs,
                                       const std::vector<NearbyLamp>& peers) {
  if (!config_) return;
  for (const auto& p : peers) {
    if (p.name.empty()) continue;
    bool freshArrival = true;
    auto it = lastSeenBleMs_.find(p.name);
    if (it != lastSeenBleMs_.end()) {
      // Wraparound-safe: cast the unsigned delta to int32 so a stale
      // entry from before a millis() wrap (~49 days) shows up as a
      // negative gap rather than a large positive (which would
      // incorrectly mark the peer as "still here" and suppress arrival).
      const int32_t gap = static_cast<int32_t>(nowMs - it->second);
      if (gap >= 0 && gap <= static_cast<int32_t>(kArrivalGapMs)) {
        freshArrival = false;
      }
    }
    // Update lastSeenBleMs_ unconditionally; bound the map.
    if (it != lastSeenBleMs_.end()) {
      it->second = nowMs;
    } else {
      if (lastSeenBleMs_.size() >= kMaxTrackedPeers) {
        // Evict the oldest entry — bounded scan is fine at this size.
        auto oldest = lastSeenBleMs_.begin();
        for (auto i = lastSeenBleMs_.begin(); i != lastSeenBleMs_.end(); ++i) {
          if (i->second < oldest->second) oldest = i;
        }
        lastSeenBleMs_.erase(oldest);
      }
      lastSeenBleMs_[p.name] = nowMs;
    }
    if (!freshArrival) continue;

    const uint8_t disp = config_->getDisposition(p.name);
    switch (disp) {
      case 5: {  // Smitten — seed 5-pulse follow-up queue.
        // No unconditional arrival pulse: per spec the Smitten "pulse" is
        // either the closest-peer cycle (45s recurring when this peer is
        // closest) OR the 5 follow-up pulses after the arrival greeting.
        // The greeting itself fires via SocialBehavior using the matrix.
        // tickClosestSmittenPulse_ later this tick will erase this queue
        // entry if the peer becomes the closest — that branch handles all
        // pulse delivery in the closest case.
        if (followUpQueue_.size() >= kFollowUpQueueCap &&
            followUpQueue_.find(p.name) == followUpQueue_.end()) {
          auto oldest = followUpQueue_.begin();
          for (auto i = followUpQueue_.begin(); i != followUpQueue_.end(); ++i) {
            if (i->second.nextFireMs < oldest->second.nextFireMs) oldest = i;
          }
          followUpQueue_.erase(oldest);
        }
        FollowUpPulseState st;
        st.remaining = kFollowUpCount;
        st.nextFireMs = nowMs + kGreetingMaxFramesMs;
        st.color = p.baseColor;
        followUpQueue_[p.name] = st;
        break;
      }
      case 4: {  // Fond — single arrival pulse
        firePulse_(p.baseColor);
        break;
      }
      case 1: {  // Salty — side-eye dim envelope, no greeting
        saltyArrivalMs_ = nowMs;
        saltyActive_    = true;
        lastSaltyLevel_ = 100;
        pendingApply_ = true;
        break;
      }
      default:  // 2 Wary / 3 Neutral — no arrival side effect
        break;
    }
  }
}

void PersonalityEngine::tickFollowUpPulses_(uint32_t nowMs) {
  for (auto it = followUpQueue_.begin(); it != followUpQueue_.end();) {
    if (static_cast<int32_t>(nowMs - it->second.nextFireMs) >= 0) {
      firePulse_(it->second.color);
      if (it->second.remaining > 0) it->second.remaining--;
      if (it->second.remaining == 0) {
        it = followUpQueue_.erase(it);
        continue;
      }
      it->second.nextFireMs = nowMs + kFollowUpIntervalMs;
    }
    ++it;
  }
}

void PersonalityEngine::tickClosestSmittenPulse_(uint32_t nowMs,
                                                  const std::vector<NearbyLamp>& peers) {
  if (!config_) return;
  // peers from getReachableViaBle() are sorted by lastRssi descending —
  // closest is front. Filter for non-empty names.
  const NearbyLamp* closest = nullptr;
  for (const auto& p : peers) {
    if (!p.name.empty()) { closest = &p; break; }
  }
  if (!closest) {
    // No BLE peers at all — release closest state, including the cadence
    // clock. If a Smitten peer re-becomes closest later, the transition
    // path should start a fresh cadence from THAT moment, not inherit a
    // stale clock that could fire a pulse moments after the new transition.
    closestSmittenName_.clear();
    lastClosestPulseMs_ = 0;
    return;
  }
  const uint8_t disp = config_->getDisposition(closest->name);
  if (disp != 5) {
    // Closest exists but isn't Smitten (or this peer's disposition just got
    // demoted). Same reset rationale as above — don't carry a stale cadence
    // clock through a Smitten ↔ non-Smitten flip.
    closestSmittenName_.clear();
    lastClosestPulseMs_ = 0;
    return;
  }
  // Transition (different closest name OR first time): fire immediately,
  // BUT only after a hysteresis check. Without hysteresis, two Smitten
  // peers with similar RSSI flap closest on dBm noise — each flip fires
  // a pulse, producing a strobe-light artifact at ~60Hz. Require the new
  // closest's RSSI to beat the previous closest's by ≥ kRssiHysteresisDb
  // before swapping. The previous closest is found in the current
  // snapshot (it may have moved further away, in which case its RSSI is
  // fresh); if it's no longer visible at all, transition without
  // hysteresis (the prev peer is gone).
  if (closest->name != closestSmittenName_) {
    if (!closestSmittenName_.empty()) {
      const NearbyLamp* prev = nullptr;
      for (const auto& p : peers) {
        if (p.name == closestSmittenName_) { prev = &p; break; }
      }
      if (prev != nullptr) {
        const int delta = static_cast<int>(closest->lastRssi) -
                          static_cast<int>(prev->lastRssi);
        if (delta < static_cast<int>(kRssiHysteresisDb)) {
          // New "closest" isn't decisively closer than the one we already
          // committed to. Keep the previous closest. Do NOT touch
          // lastClosestPulseMs_ — the cadence keeps running against the
          // previous transition.
          return;
        }
      }
    }
    closestSmittenName_ = closest->name;
    lastClosestPulseMs_ = nowMs;
    // Mutually exclusive with the after-arrival 5-pulse queue — closest
    // status takes over. Erase any pending entry for this peer so we don't
    // double-fire from both code paths.
    followUpQueue_.erase(closest->name);
    firePulse_(closest->baseColor);
    return;
  }
  // Same closest — sustained pulse cadence. Use the wraparound-safe
  // gap idiom consistent with tickArrivals_ / tickColorBleed_.
  const int32_t sinceLast = static_cast<int32_t>(nowMs - lastClosestPulseMs_);
  if (sinceLast >= static_cast<int32_t>(kClosestPulsePeriodMs)) {
    lastClosestPulseMs_ = nowMs;
    firePulse_(closest->baseColor);
  }
}

void PersonalityEngine::tickSaltyRestore_(uint32_t nowMs) {
  if (!saltyActive_) return;
  // Trip pendingApply_ when the rendered level (against a nominal 100
  // baseline) crosses a ≥1-step integer boundary so the loop re-publishes
  // brightness smoothly during the fade. Cheap: one mul + round per tick.
  const float f = saltyFactorAt_(nowMs);
  const uint8_t level = static_cast<uint8_t>(100.0f * f + 0.5f);
  if (level != lastSaltyLevel_) {
    lastSaltyLevel_ = level;
    pendingApply_   = true;
  }
  // Full window elapsed (fade-in + hold + fade-out) → release.
  const int32_t endOffset = static_cast<int32_t>(
      nowMs - (saltyArrivalMs_ + kSaltySideEyeFadeInMs +
               kSaltySideEyeHoldMs + kSaltySideEyeFadeOutMs));
  if (endOffset >= 0) {
    saltyActive_    = false;
    saltyArrivalMs_ = 0;
    lastSaltyLevel_ = 100;
    pendingApply_   = true;
  }
}

void PersonalityEngine::tickBleed_(uint32_t nowMs,
                                    const std::vector<NearbyLamp>& peers) {
  if (!config_) return;

  // 1. For every currently-Fond peer in `peers`: insert / refresh /
  //    clear pending fade-out. Bounded by kMaxTrackedPeers with LRU
  //    eviction (oldest lastSeenMs).
  for (const auto& p : peers) {
    if (p.name.empty()) continue;
    // Fond (4) and Smitten (5) both bleed — Smitten is escalation-of-Fond
    // semantically, so it does everything Fond does plus the closest-peer
    // pulse cycle. Lower dispositions don't contribute.
    if (config_->getDisposition(p.name) < 4) continue;

    auto it = bleedFadeStates_.find(p.name);
    if (it == bleedFadeStates_.end()) {
      // Evict oldest if at cap (LRU by lastSeenMs).
      if (bleedFadeStates_.size() >= kMaxTrackedPeers) {
        auto oldest = bleedFadeStates_.begin();
        for (auto i = bleedFadeStates_.begin();
             i != bleedFadeStates_.end(); ++i) {
          if (i->second.lastSeenMs < oldest->second.lastSeenMs) oldest = i;
        }
        bleedFadeStates_.erase(oldest);
      }
      BleedFadeState st;
      st.color          = p.baseColor;
      st.startMs        = nowMs;
      st.lastSeenMs     = nowMs;
      st.fadeOutStartMs = 0;
      bleedFadeStates_[p.name] = st;
    } else {
      it->second.lastSeenMs     = nowMs;
      it->second.fadeOutStartMs = 0;   // peer back / still here → cancel any fade-out
    }
  }

  // 2. Drive fade-out for entries whose peer is absent (or hasn't been
  //    refreshed this tick). The presence check piggybacks on the
  //    lastSeenMs update above — anything with lastSeenMs < nowMs is
  //    "missed this tick" → arm or continue fade-out. Once the
  //    fade-out completes, erase.
  for (auto it = bleedFadeStates_.begin(); it != bleedFadeStates_.end();) {
    if (it->second.lastSeenMs != nowMs) {
      if (it->second.fadeOutStartMs == 0) {
        it->second.fadeOutStartMs = nowMs;
      } else {
        const int32_t elapsed =
            static_cast<int32_t>(nowMs - it->second.fadeOutStartMs);
        if (elapsed >= static_cast<int32_t>(kBleedFadeOutMs)) {
          it = bleedFadeStates_.erase(it);
          continue;
        }
      }
    }
    ++it;
  }
}

std::vector<PersonalityEngine::BleedContribution>
PersonalityEngine::getBleedContributions(uint32_t nowMs) const {
  std::vector<BleedContribution> out;
  if (bleedFadeStates_.empty()) return out;

  // 1. Compute raw alpha per entry from the fade state.
  out.reserve(bleedFadeStates_.size());
  float totalAlpha = 0.0f;
  for (const auto& kv : bleedFadeStates_) {
    const BleedFadeState& s = kv.second;
    float frac = 0.0f;
    if (s.fadeOutStartMs == 0) {
      // Fade-in toward kBleedAlphaPerPeer.
      const int32_t elapsed = static_cast<int32_t>(nowMs - s.startMs);
      if (elapsed <= 0) {
        frac = 0.0f;
      } else if (elapsed >= static_cast<int32_t>(kBleedFadeInMs)) {
        frac = 1.0f;
      } else {
        frac = static_cast<float>(elapsed) /
               static_cast<float>(kBleedFadeInMs);
      }
    } else {
      // Fade-out from the last fade-in fraction we'd reached when the
      // peer dropped. (Tracking the dropped-at fraction explicitly is
      // overkill — approximate via "had they finished fade-in?".)
      const int32_t elapsedSinceFadeOut =
          static_cast<int32_t>(nowMs - s.fadeOutStartMs);
      const int32_t fadeInElapsed =
          static_cast<int32_t>(s.fadeOutStartMs - s.startMs);
      const float fadeInFrac =
          (fadeInElapsed <= 0)
              ? 0.0f
              : (fadeInElapsed >= static_cast<int32_t>(kBleedFadeInMs)
                     ? 1.0f
                     : static_cast<float>(fadeInElapsed) /
                           static_cast<float>(kBleedFadeInMs));
      if (elapsedSinceFadeOut <= 0) {
        frac = fadeInFrac;
      } else if (elapsedSinceFadeOut >=
                 static_cast<int32_t>(kBleedFadeOutMs)) {
        frac = 0.0f;
      } else {
        frac = fadeInFrac *
               (1.0f - static_cast<float>(elapsedSinceFadeOut) /
                           static_cast<float>(kBleedFadeOutMs));
      }
    }
    const float alpha = kBleedAlphaPerPeer * frac;
    if (alpha < 0.005f) continue;  // sub-perceptible — skip
    out.push_back({s.color, alpha});
    totalAlpha += alpha;
  }

  // 2. Clamp the summed alpha to kBleedMaxAlpha. When over cap, scale
  //    every contribution proportionally so the perceptual mix stays.
  if (totalAlpha > kBleedMaxAlpha && totalAlpha > 0.0f) {
    const float scale = kBleedMaxAlpha / totalAlpha;
    for (auto& c : out) c.alpha *= scale;
  }
  return out;
}

}  // namespace lamp
