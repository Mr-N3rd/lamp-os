#pragma once

#include <map>
#include <string>
#include <vector>

#include "config/config_types.hpp"
#include "core/animated_behavior.hpp"
#include "util/color.hpp"

#define LAMP_TIME_BETWEEN_ACKNOWLEDGEMENT_MS 30000

namespace lamp {

class Config;  // fwd-decl — full include is heavy and only the .cpp uses it.

/**
 * @brief social color exchange — reads the unified nearbyLamps store directly,
 *        gated on lastSeenViaBleMs so only short-range peers trigger greetings.
 *
 * Personality:
 *   - Extrovert: greet every reachable peer with a minimal cooldown
 *     (EXTROVERT_COOLDOWN_MS). No per-peer re-greet window — every fresh
 *     sighting earns a greeting.
 *   - Ambivert (default): 30s cooldown between greetings, won't re-greet
 *     the same peer within AMBIVERT_REGREET_WINDOW_MS. After
 *     AMBIVERT_FATIGUE_COUNT greetings inside AMBIVERT_FATIGUE_WINDOW_MS,
 *     enters a tired state for AMBIVERT_TIRED_DURATION_MS.
 *   - Introvert: longer base cooldown (INTROVERT_BASE_COOLDOWN_MS) plus a
 *     random recharge in [INTROVERT_RECHARGE_MIN_MS, INTROVERT_RECHARGE_MAX_MS]
 *     after each greeting. Won't re-greet the same peer within
 *     INTROVERT_REGREET_WINDOW_MS.
 *
 * Per-peer re-greet tracking is in-memory only (lastGreetedAtMs_) and
 * bounded to MAX_GREETED_TRACKED with LRU eviction. Survives the
 * NearbyLamps prune cycle so a peer leaving + returning doesn't re-greet
 * within the window.
 */
class SocialBehavior : public AnimatedBehavior {
  using AnimatedBehavior::AnimatedBehavior;

 public:
  // Legacy single-knob ease length. Kept for back-compat when control()
  // can't consult the PersonalityEngine (engine not begun, or no nearby
  // peer matched). When the engine returns a real GreetingTuning the
  // ease/hold/fadeOut/pulseBack fields below take precedence.
  uint32_t easeFrames = 120;
  uint32_t nextAcknowledgeTimeMs = 0;
  Color foundLampColor;

  // Waveform parameters, copied from PersonalityEngine::greetingFor()
  // at greeting time. easeInFrames + holdFrames + fadeOutFrames should
  // equal the AnimatedBehavior `frames`; draw() guards anyway.
  // pulseBackStrength is the depth of the optional "fade-in → subtle
  // pulse-back → pulse-into-hold" shape — 0 disables it; values up to
  // 255 darken foundLampColor proportionally during the dip. The pulse
  // occupies the first two-thirds of the hold window.
  uint32_t easeInFrames = 30;
  uint32_t holdFrames = 60;
  uint32_t fadeOutFrames = 30;
  uint8_t  pulseBackStrength = 0;

  void draw() override;
  void control() override;

  // Wires the live Config so control() can read the current socialMode.
  // No setter = behaves as Ambivert (the spec's pre-personality default).
  void setConfig(Config* config) { config_ = config; }

  // Gossip-OTA progress pulse (Phase 5b'.5). Returns a brightness
  // multiplier in [kOtaPulseMinPct, 100] composed into the brightness
  // chain alongside applyCrowdDim/applySaltyDim. When neither side of
  // an OTA flow is in progress: returns 100 (no effect). When the
  // distributor is mid-flow: single-pulse cadence (sender role). When
  // the receiver is mid-flow: double-pulse cadence (receiver role).
  // Distinct cadences let an observer identify roles by eye; both
  // patterns are subtle (multiplier dips to ~70 at the bottom of each
  // pulse) but visible — a casual observer won't notice, a paying-
  // attention observer notices the rhythm.
  //
  // Never reaches 0 — the lamp doesn't blackout during OTA.
  uint8_t otaPulseMultiplier(uint32_t nowMs) const;

  // OTA pulse tunables. Single-pulse: linear dip 100 → 70 over kDipMs,
  // hold briefly at 70, linear return over kReturnMs, pause kPauseMs,
  // repeat. Double-pulse: two dips back-to-back with kDoubleGapMs
  // between, then kDoublePauseMs pause, repeat. Subtle-but-visible
  // target. The hold beat at minimum is what makes the eye read this
  // as a "pulse" rather than a triangle wave; a smoothed curve adds
  // negligible perceptual upgrade over the configured tunables.
  static constexpr uint8_t  kOtaPulseMinPct       = 70;
  static constexpr uint32_t kOtaPulseDipMs        = 600;
  static constexpr uint32_t kOtaPulseHoldMs       = 100;
  static constexpr uint32_t kOtaPulseReturnMs     = 400;
  static constexpr uint32_t kOtaPulsePauseMs      = 400;
  static constexpr uint32_t kOtaPulseDoubleGapMs  = 200;
  static constexpr uint32_t kOtaPulseDoublePauseMs = 700;

  // OTA-hold extends the greeting animation lifetime so the shade stays
  // on the peer's base color while the distributor is mid-flow sending
  // to that peer. ~60s ceiling — a 1.5 MB image at 50 chunks/s lands in
  // ~30s, so 60s gives generous slack for retries without painting a
  // stale color forever. AnimatedBehavior frame budget at ~60fps.
  static constexpr uint32_t kOtaHoldFrameBudget = 60 * 60;

  // Throttle the gossip-OTA peer scan to this cadence. Lamps emit HELLO
  // every 1-2s, so 500 ms catches every fresh sighting while saving the
  // ~60 Hz vector allocation that control() would otherwise do per
  // frame. Skipped entirely while distributor.isInProgress() — the
  // single-source mutex blocks a second concurrent session anyway, so
  // there's nothing useful to scan for.
  static constexpr uint32_t kOtaScanIntervalMs  = 500;

 private:
  static constexpr size_t MAX_GREETED_TRACKED = 32;
  static constexpr uint32_t EXTROVERT_COOLDOWN_MS = 1000;
  static constexpr uint32_t INTROVERT_BASE_COOLDOWN_MS = 60000;
  static constexpr uint32_t INTROVERT_RECHARGE_MIN_MS = 30000;
  static constexpr uint32_t INTROVERT_RECHARGE_MAX_MS = 120000;
  static constexpr uint32_t INTROVERT_REGREET_WINDOW_MS = 600000;
  static constexpr uint32_t AMBIVERT_REGREET_WINDOW_MS = 300000;
  static constexpr size_t AMBIVERT_FATIGUE_COUNT = 5;
  static constexpr uint32_t AMBIVERT_FATIGUE_WINDOW_MS = 300000;
  static constexpr uint32_t AMBIVERT_TIRED_DURATION_MS = 60000;

  Config* config_ = nullptr;
  std::map<std::string, uint32_t> lastGreetedAtMs_;
  std::vector<uint32_t> recentGreetMs_;
  uint32_t tiredUntilMs_ = 0;

  // OTA-hold: when we greet a peer AND the distributor is mid-flow to
  // that same peer, we extend the greeting's color hold past the normal
  // fadeOut window so the OTA pulse modulates on the peer's color rather
  // than snapping back to the lamp's own shade. Cleared when the
  // distributor stops targeting otaHoldPeerMac_ (success, fail, or
  // pivoted to another peer).
  bool    otaHoldActive_       = false;
  uint8_t otaHoldPeerMac_[6]   = {0};

  // Last time the OTA peer-scan block in control() actually walked the
  // ESP-NOW roster. Throttled by kOtaScanIntervalMs to amortize the
  // vector allocation across many frames.
  uint32_t lastOtaScanMs_      = 0;
};

}  // namespace lamp
