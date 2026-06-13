#pragma once

// PersonalityEngine — Phase D personality runtime. Owns crowd-aware
// dimming, disposition-driven greeting tuning, color bleed, and arrival
// expression dispatch in a single ticked component. Replaces the scattered
// "if (socialMode == X)" checks SocialBehavior used to do alone.
//
// Read-side surface:
//   - applyCrowdDim(baseline)  → pure value transform; multiplies the
//     baseline brightness by the smoothed crowd-dim factor (Introvert
//     only; identity otherwise). Called from effectiveBrightness().
//   - greetingFor(name)        → returns a GreetingTuning describing the
//     waveform SocialBehavior should play for this peer. (Step 4.)
//
// Write-side surface:
//   - tick(nowMs) is called once per loop iteration. Samples nearbyLamps
//     at 1 Hz, recomputes the crowd-dim factor with hysteresis, dispatches
//     arrival expressions and color bleed (Steps 5+6).
//
// Test injection: in LAMP_TEST builds, setNearbyOverride() replaces the
// live nearbyLamps snapshot with a caller-supplied vector. Production
// builds compile this method out via #ifdef.

#include <Arduino.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "components/network/nearby_lamps.hpp"
#include "config/config_types.hpp"
#include "util/color.hpp"

namespace lamp {

class Config;            // fwd-decl
class ExpressionManager; // fwd-decl (used Step 5/6)
class ShowReceiver;      // fwd-decl (used Step 5 for myMac)

// A peer's greeting waveform parameters. Returned by greetingFor() and
// consumed by SocialBehavior's draw()/playOnce(). totalFrames == 0 means
// "skip this greeting entirely" — Salty in all SocialModes uses this.
struct GreetingTuning {
  uint32_t totalFrames     = 0;
  uint32_t easeInFrames    = 0;
  uint32_t holdFrames      = 0;
  uint32_t fadeOutFrames   = 0;
  uint8_t  pulseBackStrength = 0;
  bool     skip            = true;
};

class PersonalityEngine {
 public:
  // Wire dependencies. Call once during boot, after the wired modules
  // have themselves been begin()'d. ExpressionManager + ShowReceiver
  // power the arrival pulse / closest-Smitten cycle (Step 5). Salty
  // side-eye is a pure brightness multiplier (applySaltyDim); Fond
  // color bleed is an additive contribution exposed via
  // getBleedContributions() and drawn by PersonalityBehavior — neither
  // routes through ColorOverride / BrightnessOverride anymore.
  void begin(Config* config,
             ExpressionManager* expressionManager = nullptr,
             ShowReceiver* showReceiver = nullptr);

  // Drive the engine. Called every loop iteration on Core 1. Most internal
  // work is gated behind a 1 Hz sample tick.
  void tick(uint32_t nowMs);

  // Pure value transform for effectiveBrightness(). Returns `baseline`
  // when the lamp isn't Introvert or no crowd is detected. Always returns
  // ≥ 1 (we never blank a lamp from personality alone).
  uint8_t applyCrowdDim(uint8_t baseline) const;

  // Pure value transform for effectiveBrightness(). Applies the Salty
  // side-eye fade (1.0 → 0.70 → 0.70 → 1.0 across fade-in / hold /
  // fade-out) when a Salty peer has arrived recently. Identity when no
  // Salty arrival is in flight. Always returns ≥ 1. Composes with
  // applyCrowdDim — both are multipliers in (0,1].
  uint8_t applySaltyDim(uint8_t baseline) const;

  // Per-peer greeting tuning. Returned at SocialBehavior::control() time
  // so the waveform is read in lockstep with the cooldown gate the
  // behavior already does. Pure read. (Step 4 fills in the full matrix.)
  GreetingTuning greetingFor(const std::string& peerName) const;

  // Additive shade-tint contribution emitted by the Fond color bleed.
  // PersonalityBehavior::draw() consumes these to alpha-blend each
  // pixel toward the contributing peer color. `alpha` is in [0, 1].
  struct BleedContribution {
    Color color;
    float alpha;
  };

  // Snapshot the active Fond-bleed contributions at `nowMs`. Returns
  // empty when no Fond peer is in the fade-in / hold / fade-out window.
  // Total alpha across all returned entries is clamped to
  // kBleedMaxAlpha — proportional scaling when the cap is hit so no
  // single contribution dominates. Pure read; safe for the draw loop.
  std::vector<BleedContribution> getBleedContributions(uint32_t nowMs) const;

  // Was an Introvert mode → other-mode flip witnessed since last tick? Used
  // by the wiring in standard_lamp.cpp to trip pendingApplyEffectiveBrightness
  // once on the transition out of Introvert (so the dim cleanly releases).
  bool consumePendingApply() {
    const bool was = pendingApply_;
    pendingApply_ = false;
    return was;
  }

  // Curve parameters — exposed as constexpr so tests can reference them
  // without re-deriving the numbers.
  static constexpr float kDimFloor = 0.5f;       // never below 50%
  static constexpr float kCurveScaleW = 10.0f;   // ~W=10 hits the floor
  static constexpr uint32_t kSamplePeriodMs = 1000;
  static constexpr size_t kSampleWindow = 4;
  static constexpr float kEmaAlpha = 0.15f;
  static constexpr uint8_t kDeadbandLevels = 2;  // ≥ 2/100 change to commit

  // Arrival detection & dispatch constants (Step 5).
  static constexpr uint32_t kArrivalGapMs = 30000;     // "wasn't here for 30s"
  static constexpr size_t   kMaxTrackedPeers = 32;     // lastSeenBleMs_ cap

  // Smitten follow-up pulse sequence (after arrival greeting).
  static constexpr uint8_t  kFollowUpCount      = 5;
  static constexpr uint32_t kFollowUpIntervalMs = 5000;
  // ~5s upper bound on the Extrovert full-show greeting (300 frames @ 60fps).
  // The first follow-up pulse is scheduled this far AFTER arrival so it
  // doesn't overlap the greeting waveform.
  static constexpr uint32_t kGreetingMaxFramesMs = 5000;
  static constexpr size_t   kFollowUpQueueCap   = 8;

  // Smitten closest-peer recurring pulse.
  static constexpr uint32_t kClosestPulsePeriodMs = 45000;
  // Hysteresis margin (dBm) for closest-peer swaps. RSSI on ESP-NOW
  // typically wobbles ±3–5 dB even with a stationary peer; without this
  // guard, two Smitten peers with similar signal would flap "closest"
  // every tick and fire a pulse on each flip.
  static constexpr uint8_t  kRssiHysteresisDb     = 3;

  // Salty side-eye dim parameters. applySaltyDim() multiplies the
  // baseline by a piecewise-linear envelope: 1.0 → kSaltySideEyeFactor
  // over kSaltySideEyeFadeInMs, holds for kSaltySideEyeHoldMs, then
  // returns to 1.0 over kSaltySideEyeFadeOutMs.
  static constexpr float    kSaltySideEyeFactor    = 0.70f;
  static constexpr uint16_t kSaltySideEyeFadeInMs  = 200;
  static constexpr uint16_t kSaltySideEyeHoldMs    = 400;
  static constexpr uint16_t kSaltySideEyeFadeOutMs = 200;

  // Color bleed (Fond + Smitten) — additive per-pixel tint contributed by
  // PersonalityBehavior every frame, presence-driven. Per-peer fade-in
  // over kBleedFadeInMs once a Fond peer is reachable; hold while
  // present; fade-out over kBleedFadeOutMs once they drop. Total
  // alpha across all contributing peers is capped at kBleedMaxAlpha
  // so multiple Fond peers don't wash the shade out toward white.
  static constexpr uint16_t kBleedFadeInMs       = 4000;
  static constexpr uint16_t kBleedFadeOutMs      = 3000;
  static constexpr float    kBleedAlphaPerPeer   = 0.20f;
  static constexpr float    kBleedMaxAlpha       = 0.60f;

  // Available in both LAMP_TEST and LAMP_DEBUG builds. Replaces the live
  // nearbyLamps.getReachableViaBle() snapshot used by the engine — lets
  // the developer simulate a crowd from one lamp + the Flutter app's
  // test-action button. Pass {} to drop back to live data.
#if defined(LAMP_TEST) || defined(LAMP_DEBUG)
  void setNearbyOverride(std::vector<NearbyLamp> peers);
  void clearNearbyOverride();
#endif

 private:
  // --- Crowd-dim machinery ------------------------------------------------

  // Weight a peer's crowd contribution by their disposition. Smitten (5)
  // weighs 0 ("favorites don't crowd you"); Salty (1) weighs 2 ("the
  // ones you dislike crowd you more"). Neutral (3) is 1.0.
  float weightForDisposition_(uint8_t disposition) const;

  // Σ weight(disposition(peer)) over `peers`. Self-MAC filtering is the
  // caller's responsibility (nearbyLamps doesn't include self anyway).
  float computeWeightedCount_(const std::vector<NearbyLamp>& peers) const;

  // factor = max(kDimFloor, 1 - (1-kDimFloor) * log10(1+W) / log10(1+kCurveScaleW))
  // Clamped to [kDimFloor, 1.0]. Pure function of W; pinned in tests.
  float dimFactorForCount_(float weightedCount) const;

  // Pulls a fresh BLE-reachable snapshot (or the test override if active),
  // computes W, runs the rolling-median + EMA smoother, applies the
  // deadband, and updates crowdDimFactor_/lastCommittedLevel_ if needed.
  void sampleAndSmoothCrowd_(uint32_t nowMs,
                              const std::vector<NearbyLamp>& peers);

  // --- Arrival expressions + Smitten cycle (Step 5) ----------------------

  // Get the current BLE-reachable snapshot (or the test override). Pure
  // read; used by both the crowd-dim sampler and the per-tick arrival /
  // closest-peer scan so we share one snapshot per tick.
  std::vector<NearbyLamp> snapshotBlePeers_() const;

  // Per-tick scan: detects fresh arrivals (gap > kArrivalGapMs) and
  // dispatches by disposition — Smitten/Fond fire a colored pulse,
  // Salty fires the side-eye dim, Neutral/Wary do nothing. Updates
  // lastSeenBleMs_ for every visible peer (bounded by kMaxTrackedPeers
  // with LRU eviction).
  void tickArrivals_(uint32_t nowMs, const std::vector<NearbyLamp>& peers);

  // Drain the Smitten after-arrival follow-up queue: fires the next
  // pulse for any entry whose nextFireMs has elapsed. Entries with
  // remaining == 0 are erased.
  void tickFollowUpPulses_(uint32_t nowMs);

  // Smitten closest-peer pulse cycle: identifies the closest BLE peer
  // (peers are RSSI-sorted by nearby_lamps; peers.front() is highest).
  // If that peer is Smitten, fires a pulse on every transition AND every
  // kClosestPulsePeriodMs while they remain closest. A different peer
  // becoming closest resets the cadence.
  void tickClosestSmittenPulse_(uint32_t nowMs,
                                 const std::vector<NearbyLamp>& peers);

  // Auto-clear saltyActive_ after the full fade-in + hold + fade-out
  // window completes. Also trips pendingApply_ if the rendered Salty
  // level crossed an integer-brightness boundary since last tick so the
  // loop publishes the fade smoothly to NeoPixel setBrightness.
  void tickSaltyRestore_(uint32_t nowMs);

  // Compute the Salty multiplier envelope at `nowMs`. Returns 1.0
  // outside the fade window; piecewise-linear 1.0 → kSaltySideEyeFactor
  // over fadeIn, hold at kSaltySideEyeFactor, 1.0 over fadeOut.
  float saltyFactorAt_(uint32_t nowMs) const;

  // Maintain the per-peer Fond bleed fade state machine. For each
  // currently-Fond peer in `peers`: insert / refresh / cancel any
  // pending fade-out. For each tracked peer no longer present: start
  // (or continue) the fade-out and evict once the fade-out alpha hits
  // zero. Pure state mutation; the per-frame additive contribution is
  // surfaced via getBleedContributions() and rendered by
  // PersonalityBehavior — no override path involved.
  void tickBleed_(uint32_t nowMs, const std::vector<NearbyLamp>& peers);

  // Fire one `pulse` ExpressionInvocation in `color` via expressionManager_.
  // No-op if expressionManager_/showReceiver_ aren't wired. Receive-side
  // terminus — never cascades.
  void firePulse_(const Color& color);

  // --- State --------------------------------------------------------------

  Config* config_ = nullptr;
  ExpressionManager* expressionManager_ = nullptr;
  ShowReceiver* showReceiver_ = nullptr;

  uint32_t lastSampleMs_ = 0;
  // Rolling buffer of W samples; we take the median to absorb the
  // occasional outlier from a peer briefly fading in/out at the edge.
  float sampleBuf_[kSampleWindow] = {0};
  size_t sampleHead_ = 0;
  size_t sampleCount_ = 0;  // grows from 0 to kSampleWindow once seeded
  float smoothedW_ = 0.0f;
  bool emaSeeded_ = false;

  // The factor we'd return from applyCrowdDim() right now. Updated in
  // sampleAndSmoothCrowd_ when the deadband is crossed.
  float crowdDimFactor_ = 1.0f;
  // Last brightness level we committed to (after dim factor applied to a
  // nominal baseline of 100). Used to gate re-publishing pendingApply.
  uint8_t lastCommittedLevel_ = 100;
  // Set by sampleAndSmoothCrowd_ on a meaningful change OR by the
  // Introvert↔other mode transition. Consumed by the loop wiring.
  bool pendingApply_ = false;
  SocialMode lastSocialMode_ = SocialMode::Ambivert;

  // --- Arrival / follow-up / closest state (Step 5) --------------------

  // BLE last-seen timestamps per peer name. Bounded — eviction is
  // oldest-first when at cap. Used to detect "fresh arrival" (gap >
  // kArrivalGapMs since the last sighting of this name).
  std::map<std::string, uint32_t> lastSeenBleMs_;

  // Per-peer queue for the 5 Smitten after-arrival follow-up pulses.
  // Color is snapshotted at arrival time so a subsequent recolor of the
  // peer doesn't change the remaining pulses' tint.
  struct FollowUpPulseState {
    uint8_t remaining;       // counts down 5 → 0; entry erased at 0
    uint32_t nextFireMs;
    Color color;
  };
  std::map<std::string, FollowUpPulseState> followUpQueue_;

  // Smitten closest-peer tracking. Empty name = no Smitten peer is
  // currently the closest BLE neighbour. We don't cache the peer's
  // color across ticks — tickClosestSmittenPulse_ reads it from the
  // live peer snapshot every fire, so a recolored peer's pulse
  // automatically tracks without bookkeeping.
  std::string closestSmittenName_;
  uint32_t lastClosestPulseMs_ = 0;

  // Salty side-eye state machine for applySaltyDim. saltyArrivalMs_ is
  // the absolute time the Salty arrival fired; saltyActive_ stays true
  // through fade-in + hold + fade-out. lastSaltyLevel_ remembers the
  // last rendered baseline-100 level so tickSaltyRestore_ can trip
  // pendingApply_ on a crossed integer-brightness boundary.
  uint32_t saltyArrivalMs_  = 0;
  bool     saltyActive_     = false;
  uint8_t  lastSaltyLevel_  = 100;

  // Per-peer Fond-bleed fade state. Inserted on first sighting,
  // refreshed each tick the peer is present, fade-out armed on the
  // first tick the peer is absent, erased once the fade-out completes.
  // Bounded by kMaxTrackedPeers with LRU eviction (oldest lastSeenMs).
  struct BleedFadeState {
    Color    color;             // peer base color snapshotted at first sight
    uint32_t startMs        = 0;  // when this peer's fade-in began
    uint32_t lastSeenMs     = 0;  // refreshed each tick the peer is present
    uint32_t fadeOutStartMs = 0;  // 0 = peer still present; else fade-out start
  };
  std::map<std::string, BleedFadeState> bleedFadeStates_;

#if defined(LAMP_TEST) || defined(LAMP_DEBUG)
  std::vector<NearbyLamp> nearbyOverride_;
  bool nearbyOverrideActive_ = false;
#endif
};

extern PersonalityEngine personalityEngine;

}  // namespace lamp
