// Native-host unit tests for PersonalityEngine. Mirrors the production
// engine inline (same convention as test_transient_override) so the
// native rig doesn't need to link the firmware. Each test pins the
// observable contract (curve shape, hysteresis behavior, mode-flip
// release, greeting matrix, chained brightness multipliers, bleed
// contribution math) so production refactors are free as long as the
// contract holds.

#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace test {

// --- Time scaffolding -----------------------------------------------------
static uint32_t g_nowMs = 0;
static uint32_t mockMillis() { return g_nowMs; }

// --- Mirrors of production types -----------------------------------------

enum class SocialMode : uint8_t {
  Introvert = 0,
  Ambivert  = 1,
  Extrovert = 2,
};

struct Color {
  uint8_t r = 0, g = 0, b = 0, w = 0;
  Color() = default;
  Color(uint8_t inR, uint8_t inG, uint8_t inB, uint8_t inW)
      : r(inR), g(inG), b(inB), w(inW) {}
  bool operator==(const Color& o) const {
    return r == o.r && g == o.g && b == o.b && w == o.w;
  }
};

struct NearbyLamp {
  std::string name;
  Color baseColor;
  int8_t lastRssi = -127;
};

// Mock ExpressionManager + BrightnessOverride + ShowReceiver for tests
// 8-10 and 18-22. Records every call so assertions can pin the contract.

struct InvocationRecord {
  std::string type;
  Color color;
  uint32_t timeMs = 0;
};

struct BrightnessApplyRecord {
  uint8_t level;
  uint16_t fadeMs;
  uint32_t timeMs = 0;
};

struct BrightnessRestoreRecord {
  uint16_t fadeMs;
  uint32_t timeMs = 0;
};

class MockExpressionManager {
 public:
  std::vector<InvocationRecord> invocations;
  void triggerPulse(const Color& color) {
    invocations.push_back({"pulse", color, mockMillis()});
  }
};

class MockBrightnessOverride {
 public:
  std::vector<BrightnessApplyRecord> applies;
  std::vector<BrightnessRestoreRecord> restores;
  void apply(uint8_t level, uint16_t fadeMs) {
    applies.push_back({level, fadeMs, mockMillis()});
  }
  void restore(uint16_t fadeMs) {
    restores.push_back({fadeMs, mockMillis()});
  }
};

// Mirror of lamp::GreetingTuning + the matrix profiles. Tests pin the
// observable shape — production refactor of internals is fine as long
// as these (mode × disposition) cells continue to map to the same
// (totalFrames, pulseBackStrength, skip) outputs.
struct GreetingTuning {
  uint32_t totalFrames = 0;
  uint32_t easeInFrames = 0;
  uint32_t holdFrames = 0;
  uint32_t fadeOutFrames = 0;
  uint8_t  pulseBackStrength = 0;
  bool     skip = true;
};

// Minimal Config stand-in: just enough to support socialMode + disposition
// lookup. Mirrors the public surface PersonalityEngine reads.
class FakeConfig {
 public:
  SocialMode socialMode = SocialMode::Ambivert;
  std::map<std::string, uint8_t> dispositions;
  uint8_t getDisposition(const std::string& name) const {
    auto it = dispositions.find(name);
    return it == dispositions.end() ? /*kDispositionDefault=*/3 : it->second;
  }
};

// --- PersonalityEngine inline mirror (crowd-dim subset, Step 3) ----------
//
// Mirrors the surface that effectiveBrightness() / loop() in
// standard_lamp.cpp consumes. greetingFor() returns a neutral default
// for Step 3 — Step 4 fills in the matrix and the dedicated tests.
class PersonalityEngine {
 public:
  static constexpr float kDimFloor = 0.5f;
  static constexpr float kCurveScaleW = 10.0f;
  static constexpr uint32_t kSamplePeriodMs = 1000;
  static constexpr size_t kSampleWindow = 4;
  static constexpr float kEmaAlpha = 0.15f;
  static constexpr uint8_t kDeadbandLevels = 2;
  // Public-mirror of the production cap so tests can reference it
  // directly (production exposes the same constant at public scope).
  static constexpr size_t kMaxTrackedPeers = 32;
  // Bleed contribution constants — referenced directly by tests.
  static constexpr uint16_t kBleedFadeInMs     = 4000;
  static constexpr uint16_t kBleedFadeOutMs    = 3000;
  static constexpr float    kBleedAlphaPerPeer = 0.20f;
  static constexpr float    kBleedMaxAlpha     = 0.60f;

  // Test-only inspector for the bleed-state map size — confirms the LRU
  // eviction in tickBleed_ keeps the map bounded.
  size_t bleedStateMapSize() const { return bleedFadeStates_.size(); }

  // Mock instances are still threaded through for the arrival / pulse
  // tests. MockColorOverride is gone — the bleed no longer routes through
  // ColorOverride. brightnessOverride_ is retained but unused (the salty
  // dim is now an in-engine multiplier).
  void begin(FakeConfig* config,
             MockBrightnessOverride* brightnessOverride = nullptr,
             MockExpressionManager* expressionManager = nullptr) {
    config_ = config;
    brightnessOverride_ = brightnessOverride;
    expressionManager_ = expressionManager;
    if (config_) lastSocialMode_ = config_->socialMode;
  }

  void setNearbyOverride(std::vector<NearbyLamp> peers) {
    nearbyOverride_ = std::move(peers);
  }

  void tick(uint32_t nowMs) {
    if (!config_) return;
    const SocialMode currentMode = config_->socialMode;
    if (currentMode != lastSocialMode_) {
      lastSocialMode_ = currentMode;
      if (currentMode != SocialMode::Introvert && crowdDimFactor_ < 1.0f) {
        crowdDimFactor_ = 1.0f;
        lastCommittedLevel_ = 100;
        pendingApply_ = true;
      }
      sampleCount_ = 0;
      emaSeeded_ = false;
    }
    // Per-tick arrival / pulse cycles (reactive, no sample gate).
    tickArrivals_(nowMs);
    tickClosestSmittenPulse_(nowMs);
    tickFollowUpPulses_(nowMs);
    tickSaltyRestore_(nowMs);
    tickBleed_(nowMs);
    // 1 Hz sample cadence for crowd dim.
    if (nowMs - lastSampleMs_ >= kSamplePeriodMs || lastSampleMs_ == 0) {
      lastSampleMs_ = nowMs;
      sampleAndSmoothCrowd_(nowMs);
    }
  }

  uint8_t applyCrowdDim(uint8_t baseline) const {
    if (!config_ || config_->socialMode != SocialMode::Introvert) {
      return baseline;
    }
    if (crowdDimFactor_ >= 0.999f) return baseline;
    const float scaled = static_cast<float>(baseline) * crowdDimFactor_;
    uint8_t out = static_cast<uint8_t>(scaled + 0.5f);
    if (out < 1) out = 1;
    return out;
  }

  // Mirror of production applySaltyDim — multiplies baseline by the
  // Salty envelope at the test clock's current g_nowMs.
  uint8_t applySaltyDim(uint8_t baseline) const {
    if (!saltyActive_) return baseline;
    const float f = saltyFactorAt_(mockMillis());
    if (f >= 0.999f) return baseline;
    const float scaled = static_cast<float>(baseline) * f;
    uint8_t out = static_cast<uint8_t>(scaled + 0.5f);
    if (out < 1) out = 1;
    return out;
  }

  // Mirror of production BleedContribution + getBleedContributions.
  struct BleedContribution {
    Color color;
    float alpha;
  };
  std::vector<BleedContribution> getBleedContributions(uint32_t nowMs) const {
    std::vector<BleedContribution> out;
    if (bleedFadeStates_.empty()) return out;
    out.reserve(bleedFadeStates_.size());
    float totalAlpha = 0.0f;
    for (const auto& kv : bleedFadeStates_) {
      const BleedFadeState& s = kv.second;
      float frac = 0.0f;
      if (s.fadeOutStartMs == 0) {
        const int32_t elapsed = static_cast<int32_t>(nowMs - s.startMs);
        if (elapsed <= 0)                                            frac = 0.0f;
        else if (elapsed >= static_cast<int32_t>(kBleedFadeInMs))    frac = 1.0f;
        else frac = static_cast<float>(elapsed) /
                    static_cast<float>(kBleedFadeInMs);
      } else {
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
        if (elapsedSinceFadeOut <= 0)                                   frac = fadeInFrac;
        else if (elapsedSinceFadeOut >= static_cast<int32_t>(kBleedFadeOutMs)) frac = 0.0f;
        else frac = fadeInFrac *
                    (1.0f - static_cast<float>(elapsedSinceFadeOut) /
                                static_cast<float>(kBleedFadeOutMs));
      }
      const float alpha = kBleedAlphaPerPeer * frac;
      if (alpha < 0.005f) continue;
      out.push_back({s.color, alpha});
      totalAlpha += alpha;
    }
    if (totalAlpha > kBleedMaxAlpha && totalAlpha > 0.0f) {
      const float scale = kBleedMaxAlpha / totalAlpha;
      for (auto& c : out) c.alpha *= scale;
    }
    return out;
  }

  // Greeting waveform matrix — pure read; production reads via Config.
  GreetingTuning greetingFor(const std::string& peerName) const {
    if (!config_) return profileToTuning_(150, 36, 48, 66, 0);
    const uint8_t disp = config_->getDisposition(peerName);  // unknown → 3
    return profileForCell_(config_->socialMode, disp);
  }

  bool consumePendingApply() {
    const bool was = pendingApply_;
    pendingApply_ = false;
    return was;
  }

  // Test-only inspectors
  float currentDimFactor() const { return crowdDimFactor_; }
  float currentSmoothedW() const { return smoothedW_; }
  int pendingApplyCount() const { return pendingApplyCount_; }

  // Direct curve probe — pure function so tests can pin sanity points
  // without going through the sampling pipeline.
  float dimFactorForCount(float w) const { return dimFactorForCount_(w); }

 private:
  static GreetingTuning profileToTuning_(uint32_t total, uint32_t easeIn,
                                          uint32_t hold, uint32_t fadeOut,
                                          uint8_t pulseBack) {
    GreetingTuning t;
    t.totalFrames = total;
    t.easeInFrames = easeIn;
    t.holdFrames = hold;
    t.fadeOutFrames = fadeOut;
    t.pulseBackStrength = pulseBack;
    t.skip = (total == 0);
    return t;
  }
  static GreetingTuning profileForCell_(SocialMode mode, uint8_t disp) {
    // Profiles named the same as production. Salty is skip in every row.
    switch (disp) {
      case 1: return profileToTuning_(0, 0, 0, 0, 0);              // skip
      case 2:  // Wary
        if (mode == SocialMode::Extrovert)
          return profileToTuning_(90, 18, 30, 42, 0);              // quick
        return profileToTuning_(60, 12, 24, 24, 0);                // minimal
      case 4:  // Fond
        if (mode == SocialMode::Introvert)
          return profileToTuning_(120, 30, 36, 54, 0);             // gentle
        if (mode == SocialMode::Ambivert)
          return profileToTuning_(180, 42, 60, 78, 100);           // warm
        return profileToTuning_(240, 54, 96, 90, 100);             // enthused
      case 5:  // Smitten
        if (mode == SocialMode::Introvert)
          return profileToTuning_(180, 42, 60, 78, 100);           // warm
        if (mode == SocialMode::Ambivert)
          return profileToTuning_(240, 54, 96, 90, 100);           // enthused
        return profileToTuning_(300, 60, 120, 120, 100);           // full-show
      case 3:
      default:  // Neutral / unknown
        if (mode == SocialMode::Introvert)
          return profileToTuning_(60, 12, 24, 24, 0);              // minimal
        return profileToTuning_(150, 36, 48, 66, 0);               // standard
    }
  }
  float weightForDisposition_(uint8_t d) const {
    switch (d) {
      case 1: return 2.0f;
      case 2: return 1.5f;
      case 3: return 1.0f;
      case 4: return 0.5f;
      case 5: return 0.0f;
      default: return 1.0f;
    }
  }
  float computeWeightedCount_(const std::vector<NearbyLamp>& peers) const {
    if (!config_) return 0.0f;
    float w = 0.0f;
    for (const auto& p : peers) {
      if (p.name.empty()) continue;
      w += weightForDisposition_(config_->getDisposition(p.name));
    }
    return w;
  }
  float dimFactorForCount_(float w) const {
    if (w <= 0.0f) return 1.0f;
    const float numer = std::log10(1.0f + w);
    const float denom = std::log10(1.0f + kCurveScaleW);
    const float drop  = (1.0f - kDimFloor) * (numer / denom);
    float f = 1.0f - drop;
    if (f < kDimFloor) f = kDimFloor;
    if (f > 1.0f)      f = 1.0f;
    return f;
  }
  void sampleAndSmoothCrowd_(uint32_t /*nowMs*/) {
    const float rawW = computeWeightedCount_(nearbyOverride_);
    sampleBuf_[sampleHead_] = rawW;
    sampleHead_ = (sampleHead_ + 1) % kSampleWindow;
    if (sampleCount_ < kSampleWindow) sampleCount_++;
    float sorted[kSampleWindow];
    for (size_t i = 0; i < sampleCount_; ++i) sorted[i] = sampleBuf_[i];
    std::sort(sorted, sorted + sampleCount_);
    const float median = (sampleCount_ % 2 == 1)
        ? sorted[sampleCount_ / 2]
        : 0.5f * (sorted[sampleCount_ / 2 - 1] + sorted[sampleCount_ / 2]);
    if (!emaSeeded_) {
      smoothedW_ = median;
      emaSeeded_ = true;
    } else {
      smoothedW_ = kEmaAlpha * median + (1.0f - kEmaAlpha) * smoothedW_;
    }
    const float targetFactor = dimFactorForCount_(smoothedW_);
    const uint8_t targetLevel = static_cast<uint8_t>(targetFactor * 100.0f + 0.5f);
    const int delta = static_cast<int>(targetLevel) - static_cast<int>(lastCommittedLevel_);
    const int absDelta = delta < 0 ? -delta : delta;
    if (absDelta >= kDeadbandLevels) {
      crowdDimFactor_ = targetFactor;
      lastCommittedLevel_ = targetLevel;
      if (config_->socialMode == SocialMode::Introvert) {
        pendingApply_ = true;
        pendingApplyCount_++;
      }
    }
  }

  // --- Step 5 internals: arrival / follow-up / closest / salty ----------

  void firePulse_(const Color& color) {
    if (expressionManager_) expressionManager_->triggerPulse(color);
  }

  void tickArrivals_(uint32_t nowMs) {
    if (!config_) return;
    for (const auto& p : nearbyOverride_) {
      if (p.name.empty()) continue;
      bool freshArrival = true;
      auto it = lastSeenBleMs_.find(p.name);
      if (it != lastSeenBleMs_.end()) {
        if (nowMs - it->second <= kArrivalGapMs) freshArrival = false;
      }
      if (it != lastSeenBleMs_.end()) {
        it->second = nowMs;
      } else {
        if (lastSeenBleMs_.size() >= kMaxTrackedPeers) {
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
        case 5: {
          // No unconditional arrival pulse — Smitten's pulse delivery is
          // either the closest-cycle (45s cadence) or the 5 follow-up
          // pulses below. The greeting itself comes from SocialBehavior.
          FollowUpPulseState st;
          st.remaining = kFollowUpCount;
          st.nextFireMs = nowMs + kGreetingMaxFramesMs;
          st.color = p.baseColor;
          followUpQueue_[p.name] = st;
          break;
        }
        case 4: firePulse_(p.baseColor); break;
        case 1:
          // Salty arrival → trip the in-engine envelope. Production no
          // longer routes through BrightnessOverride; the mirror tracks
          // the same state machine for applySaltyDim() assertions.
          saltyArrivalMs_ = nowMs;
          saltyActive_    = true;
          lastSaltyLevel_ = 100;
          pendingApply_   = true;
          break;
        default: break;
      }
    }
  }

  void tickFollowUpPulses_(uint32_t nowMs) {
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

  void tickClosestSmittenPulse_(uint32_t nowMs) {
    if (!config_) return;
    // Sort a local copy by RSSI desc (mirrors production
    // getReachableViaBle behavior). nearbyOverride_ stays in test-author
    // order so test setup doesn't need to know about the sort.
    std::vector<NearbyLamp> sorted = nearbyOverride_;
    std::stable_sort(sorted.begin(), sorted.end(),
                      [](const NearbyLamp& a, const NearbyLamp& b) {
                        return a.lastRssi > b.lastRssi;
                      });
    const NearbyLamp* closest = nullptr;
    for (const auto& p : sorted) {
      if (!p.name.empty()) { closest = &p; break; }
    }
    if (!closest) {
      closestSmittenName_.clear();
      lastClosestPulseMs_ = 0;
      return;
    }
    const uint8_t disp = config_->getDisposition(closest->name);
    if (disp != 5) {
      closestSmittenName_.clear();
      lastClosestPulseMs_ = 0;
      return;
    }
    if (closest->name != closestSmittenName_) {
      // Hysteresis (mirrors production): if the previous closest is still
      // visible, only swap when the new closest is ≥kRssiHysteresisDb
      // dBm stronger. Stops noise-driven flapping.
      if (!closestSmittenName_.empty()) {
        const NearbyLamp* prev = nullptr;
        for (const auto& p : sorted) {
          if (p.name == closestSmittenName_) { prev = &p; break; }
        }
        if (prev != nullptr) {
          const int delta = static_cast<int>(closest->lastRssi) -
                            static_cast<int>(prev->lastRssi);
          if (delta < static_cast<int>(kRssiHysteresisDb)) return;
        }
      }
      closestSmittenName_ = closest->name;
      lastClosestPulseMs_ = nowMs;
      followUpQueue_.erase(closest->name);
      firePulse_(closest->baseColor);
      return;
    }
    if (nowMs - lastClosestPulseMs_ >= kClosestPulsePeriodMs) {
      lastClosestPulseMs_ = nowMs;
      firePulse_(closest->baseColor);
    }
  }

  void tickSaltyRestore_(uint32_t nowMs) {
    if (!saltyActive_) return;
    const float f = saltyFactorAt_(nowMs);
    const uint8_t level = static_cast<uint8_t>(100.0f * f + 0.5f);
    if (level != lastSaltyLevel_) {
      lastSaltyLevel_ = level;
      pendingApply_   = true;
    }
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

  float saltyFactorAt_(uint32_t nowMs) const {
    if (!saltyActive_) return 1.0f;
    const int32_t elapsed = static_cast<int32_t>(nowMs - saltyArrivalMs_);
    if (elapsed < 0) return 1.0f;
    const uint32_t e = static_cast<uint32_t>(elapsed);
    const uint32_t fadeIn  = kSaltySideEyeFadeInMs;
    const uint32_t hold    = kSaltySideEyeHoldMs;
    const uint32_t fadeOut = kSaltySideEyeFadeOutMs;
    if (e < fadeIn) {
      const float t = static_cast<float>(e) / static_cast<float>(fadeIn);
      return 1.0f - (1.0f - kSaltySideEyeFactor) * t;
    }
    if (e < fadeIn + hold) return kSaltySideEyeFactor;
    if (e < fadeIn + hold + fadeOut) {
      const float t = static_cast<float>(e - fadeIn - hold) /
                      static_cast<float>(fadeOut);
      return kSaltySideEyeFactor + (1.0f - kSaltySideEyeFactor) * t;
    }
    return 1.0f;
  }

  void tickBleed_(uint32_t nowMs) {
    if (!config_) return;
    // 1. Insert / refresh fond peer entries.
    for (const auto& p : nearbyOverride_) {
      if (p.name.empty()) continue;
      if (config_->getDisposition(p.name) < 4) continue;  // Fond + Smitten
      auto it = bleedFadeStates_.find(p.name);
      if (it == bleedFadeStates_.end()) {
        if (bleedFadeStates_.size() >= kMaxTrackedPeers) {
          auto oldest = bleedFadeStates_.begin();
          for (auto i = bleedFadeStates_.begin(); i != bleedFadeStates_.end(); ++i) {
            if (i->second.lastSeenMs < oldest->second.lastSeenMs) oldest = i;
          }
          bleedFadeStates_.erase(oldest);
        }
        BleedFadeState st;
        st.color = p.baseColor;
        st.startMs = nowMs;
        st.lastSeenMs = nowMs;
        st.fadeOutStartMs = 0;
        bleedFadeStates_[p.name] = st;
      } else {
        it->second.lastSeenMs = nowMs;
        it->second.fadeOutStartMs = 0;
      }
    }
    // 2. Fade-out / evict entries whose peer wasn't refreshed this tick.
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

  // Constants (mirror production). kMaxTrackedPeers is declared at
  // public scope above so tests can reference it without breaking
  // encapsulation; the rest stay private.
  static constexpr uint32_t kArrivalGapMs       = 30000;
  static constexpr uint8_t  kFollowUpCount      = 5;
  static constexpr uint32_t kFollowUpIntervalMs = 5000;
  static constexpr uint32_t kGreetingMaxFramesMs = 5000;
  static constexpr uint32_t kClosestPulsePeriodMs = 45000;
  static constexpr uint8_t  kRssiHysteresisDb     = 3;
  static constexpr float    kSaltySideEyeFactor   = 0.70f;
  static constexpr uint16_t kSaltySideEyeFadeInMs  = 200;
  static constexpr uint16_t kSaltySideEyeHoldMs    = 400;
  static constexpr uint16_t kSaltySideEyeFadeOutMs = 200;
  // kBleedFadeInMs / kBleedFadeOutMs / kBleedAlphaPerPeer / kBleedMaxAlpha
  // are declared at public scope above so tests can reference them.

  struct FollowUpPulseState {
    uint8_t remaining;
    uint32_t nextFireMs;
    Color color;
  };

  struct BleedFadeState {
    Color    color;
    uint32_t startMs        = 0;
    uint32_t lastSeenMs     = 0;
    uint32_t fadeOutStartMs = 0;
  };

  FakeConfig* config_ = nullptr;
  MockBrightnessOverride* brightnessOverride_ = nullptr;
  MockExpressionManager*  expressionManager_  = nullptr;
  std::vector<NearbyLamp> nearbyOverride_;
  std::map<std::string, uint32_t> lastSeenBleMs_;
  std::map<std::string, FollowUpPulseState> followUpQueue_;
  std::map<std::string, BleedFadeState> bleedFadeStates_;
  std::string closestSmittenName_;
  uint32_t lastClosestPulseMs_ = 0;
  uint32_t saltyArrivalMs_ = 0;
  bool     saltyActive_    = false;
  uint8_t  lastSaltyLevel_ = 100;
  uint32_t lastSampleMs_ = 0;
  float sampleBuf_[kSampleWindow] = {0};
  size_t sampleHead_ = 0;
  size_t sampleCount_ = 0;
  float smoothedW_ = 0.0f;
  bool emaSeeded_ = false;
  float crowdDimFactor_ = 1.0f;
  uint8_t lastCommittedLevel_ = 100;
  bool pendingApply_ = false;
  int pendingApplyCount_ = 0;
  SocialMode lastSocialMode_ = SocialMode::Ambivert;
};

// --- Helpers for tests ---------------------------------------------------

static std::vector<NearbyLamp> peers(size_t n, uint8_t dispositionToSet,
                                     FakeConfig& cfg, const char* prefix = "p") {
  std::vector<NearbyLamp> v;
  v.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    NearbyLamp lamp;
    lamp.name = std::string(prefix) + std::to_string(i);
    v.push_back(lamp);
    cfg.dispositions[lamp.name] = dispositionToSet;
  }
  return v;
}

// Build a single named peer with color + RSSI (for arrival / closest tests).
static NearbyLamp makePeer(const char* name, uint8_t disp, FakeConfig& cfg,
                           Color color = Color(255, 0, 0, 0),
                           int8_t rssi = -50) {
  NearbyLamp p;
  p.name = name;
  p.baseColor = color;
  p.lastRssi = rssi;
  cfg.dispositions[name] = disp;
  return p;
}

// Reset shared mocks between tests (engine reset re-binds them).
static void resetMocks(MockExpressionManager& em, MockBrightnessOverride& bo) {
  em.invocations.clear();
  bo.applies.clear();
  bo.restores.clear();
}

// Run enough sample ticks to flush the median window + EMA convergence.
// Each tick advances g_nowMs by kSamplePeriodMs.
static void runSampleTicks(PersonalityEngine& eng, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    g_nowMs += PersonalityEngine::kSamplePeriodMs;
    eng.tick(g_nowMs);
  }
}

// Settle the smoother fully against a held nearby list. Returns the
// final committed dim factor.
static float settleAt(PersonalityEngine& eng, const std::vector<NearbyLamp>& nearby) {
  eng.setNearbyOverride(nearby);
  runSampleTicks(eng, 40);  // way more than needed; EMA τ ≈ 6s = 6 ticks
  return eng.currentDimFactor();
}

// Reset clock + engine state between tests so each test starts clean.
static void resetEngine(PersonalityEngine& eng, FakeConfig& cfg) {
  g_nowMs = 0;
  cfg.socialMode = SocialMode::Introvert;
  cfg.dispositions.clear();
  eng = PersonalityEngine{};
  eng.begin(&cfg);
}

// --- Tests ---------------------------------------------------------------

// 1. test_crowd_weight_curve_neutral_peers
void test_crowd_weight_curve_neutral_peers() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);

  // Pure curve probe — bypass sampling, hit dimFactorForCount directly.
  // Reference values computed from the formula
  //   factor = max(0.5, 1 - 0.5 * log10(1+W) / log10(11))
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0000f, eng.dimFactorForCount(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.7709f, eng.dimFactorForCount(2.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6644f, eng.dimFactorForCount(4.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5664f, eng.dimFactorForCount(7.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5000f, eng.dimFactorForCount(10.0f));
  // Past the floor — clamped, not negative.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5000f, eng.dimFactorForCount(20.0f));
}

// 2. test_crowd_weight_smitten_is_zero
void test_crowd_weight_smitten_is_zero() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  // 10 Smitten peers contribute W = 0 — no dim.
  const float factor = settleAt(eng, peers(10, /*disposition=*/5, cfg));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.000f, factor);
  TEST_ASSERT_EQUAL_UINT8(100, eng.applyCrowdDim(100));
}

// 3. test_crowd_weight_salty_double
void test_crowd_weight_salty_double() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  // 5 Salty peers contribute W = 10 — hits the 50% floor.
  const float factor = settleAt(eng, peers(5, /*disposition=*/1, cfg));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.500f, factor);
  TEST_ASSERT_EQUAL_UINT8(50, eng.applyCrowdDim(100));
}

// 4. test_crowd_weight_mixed
void test_crowd_weight_mixed() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  // 1 Salty (2.0) + 2 Wary (3.0) + 1 Neutral (1.0) + 1 Fond (0.5) + 1 Smitten (0.0)
  //   = 6.5 weighted
  std::vector<NearbyLamp> mix;
  auto add = [&](const std::string& n, uint8_t d) {
    NearbyLamp lamp;
    lamp.name = n;
    lamp.lastRssi = -50;
    mix.push_back(lamp);
    cfg.dispositions[n] = d;
  };
  add("salt1", 1);
  add("wary1", 2); add("wary2", 2);
  add("neut1", 3);
  add("fond1", 4);
  add("smit1", 5);
  const float factor = settleAt(eng, mix);
  // Expected: dimFactorForCount(6.5) ≈ 0.5799 at convergence; allow a
  // little slack for the deadband lag (committed factor lags by up to
  // 0.02 from the next un-committed step).
  TEST_ASSERT_FLOAT_WITHIN(0.025f, 0.5799f, factor);
}

// 5. test_dim_only_when_introvert
void test_dim_only_when_introvert() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);

  // Settle a crowded state under Introvert.
  cfg.socialMode = SocialMode::Introvert;
  settleAt(eng, peers(10, 3, cfg));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, eng.currentDimFactor());
  TEST_ASSERT_EQUAL_UINT8(50, eng.applyCrowdDim(100));

  // Flip to Ambivert — engine should release dim and trip pendingApply.
  cfg.socialMode = SocialMode::Ambivert;
  // Drain the pendingApply count established under Introvert so we only
  // see the transition-driven trip.
  (void)eng.consumePendingApply();
  g_nowMs += 1;
  eng.tick(g_nowMs);
  TEST_ASSERT_TRUE(eng.consumePendingApply());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, eng.currentDimFactor());
  // applyCrowdDim returns the raw baseline regardless of internal factor.
  TEST_ASSERT_EQUAL_UINT8(100, eng.applyCrowdDim(100));

  // Flip back to Introvert with the same crowd present — dim re-engages
  // after the smoother re-converges.
  cfg.socialMode = SocialMode::Introvert;
  runSampleTicks(eng, 30);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, eng.currentDimFactor());
  TEST_ASSERT_EQUAL_UINT8(50, eng.applyCrowdDim(100));
}

// 6. test_dim_deadband_absorbs_alternation
void test_dim_deadband_absorbs_alternation() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);

  // Settle at a stable weighted count first (3 neutrals).
  settleAt(eng, peers(3, 3, cfg));
  const int baselineApplies = eng.pendingApplyCount();
  // Drain accumulated pendingApply flag.
  (void)eng.consumePendingApply();

  // Alternate snapshots between W=3 and W=4 (Neutrals). The factor at
  // W=3 is ~0.711, at W=4 is ~0.665 — a 4-5 level swing, BUT the median
  // of a [3,4,3,4] window is 3.5 → smoothed monotonically settles
  // somewhere between the two without crossing the deadband repeatedly.
  for (int i = 0; i < 10; ++i) {
    eng.setNearbyOverride(peers((i % 2) == 0 ? 3 : 4, 3, cfg));
    g_nowMs += PersonalityEngine::kSamplePeriodMs;
    eng.tick(g_nowMs);
  }
  // After settling on a roughly constant median, the deadband should keep
  // re-issuances rare. Allow at most 2 extra commits during the 10-tick
  // alternation (one for the median shift, one for trailing EMA settle).
  const int alternationApplies = eng.pendingApplyCount() - baselineApplies;
  TEST_ASSERT_LESS_OR_EQUAL_INT(2, alternationApplies);
}

// 15. test_greeting_tuning_matrix — table-driven across all 15 cells.
void test_greeting_tuning_matrix() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);

  struct Row {
    SocialMode mode;
    uint8_t disposition;
    uint32_t expectedTotal;
    uint8_t expectedPulseBack;
    bool expectedSkip;
  };
  const Row rows[] = {
    // Salty — skip in every mode
    {SocialMode::Introvert, 1,   0,   0, true},
    {SocialMode::Ambivert,  1,   0,   0, true},
    {SocialMode::Extrovert, 1,   0,   0, true},
    // Wary
    {SocialMode::Introvert, 2,  60,   0, false},
    {SocialMode::Ambivert,  2,  60,   0, false},
    {SocialMode::Extrovert, 2,  90,   0, false},
    // Neutral
    {SocialMode::Introvert, 3,  60,   0, false},
    {SocialMode::Ambivert,  3, 150,   0, false},
    {SocialMode::Extrovert, 3, 150,   0, false},
    // Fond
    {SocialMode::Introvert, 4, 120,   0, false},
    {SocialMode::Ambivert,  4, 180, 100, false},
    {SocialMode::Extrovert, 4, 240, 100, false},
    // Smitten
    {SocialMode::Introvert, 5, 180, 100, false},
    {SocialMode::Ambivert,  5, 240, 100, false},
    {SocialMode::Extrovert, 5, 300, 100, false},
  };

  cfg.dispositions["peer"] = 3;  // placeholder; we'll set per-row
  for (const auto& r : rows) {
    cfg.socialMode = r.mode;
    cfg.dispositions["peer"] = r.disposition;
    const GreetingTuning t = eng.greetingFor("peer");
    TEST_ASSERT_EQUAL_UINT32(r.expectedTotal, t.totalFrames);
    TEST_ASSERT_EQUAL_UINT8(r.expectedPulseBack, t.pulseBackStrength);
    TEST_ASSERT_EQUAL(r.expectedSkip, t.skip);
  }
}

// 16. test_greeting_for_salty_is_skip_in_all_modes — explicit guard.
void test_greeting_for_salty_is_skip_in_all_modes() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  cfg.dispositions["nemesis"] = 1;
  for (auto mode : {SocialMode::Introvert, SocialMode::Ambivert, SocialMode::Extrovert}) {
    cfg.socialMode = mode;
    const GreetingTuning t = eng.greetingFor("nemesis");
    TEST_ASSERT_TRUE(t.skip);
    TEST_ASSERT_EQUAL_UINT32(0, t.totalFrames);
    TEST_ASSERT_EQUAL_UINT8(0, t.pulseBackStrength);
  }
}

// 17. test_greeting_for_unknown_peer_is_neutral — peer not in store
// defaults to the current mode's Neutral row.
void test_greeting_for_unknown_peer_is_neutral() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  // Don't insert "stranger" into cfg.dispositions.
  cfg.socialMode = SocialMode::Ambivert;
  const GreetingTuning t = eng.greetingFor("stranger");
  TEST_ASSERT_FALSE(t.skip);
  // Ambivert × Neutral → standard (150, 36, 48, 66, 0).
  TEST_ASSERT_EQUAL_UINT32(150, t.totalFrames);
  TEST_ASSERT_EQUAL_UINT32(36, t.easeInFrames);
  TEST_ASSERT_EQUAL_UINT32(48, t.holdFrames);
  TEST_ASSERT_EQUAL_UINT32(66, t.fadeOutFrames);
  TEST_ASSERT_EQUAL_UINT8(0, t.pulseBackStrength);
}

// 7. test_dim_ema_step_response
void test_dim_ema_step_response() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);

  // Cold start with no peers — first sample commits nothing significant
  // (factor 1.0 == default). Then step to 8 neutrals.
  settleAt(eng, {});
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, eng.currentDimFactor());
  (void)eng.consumePendingApply();

  // Step to 8 neutrals — dimFactorForCount(8) ≈ 0.5418 at convergence.
  // The committed factor lags by up to the deadband (0.02), and the EMA
  // approaches asymptotically; allow a bit of slack on both axes.
  eng.setNearbyOverride(peers(8, 3, cfg));
  runSampleTicks(eng, 40);
  TEST_ASSERT_FLOAT_WITHIN(0.025f, 0.5418f, eng.currentDimFactor());
  // pendingApply has been tripped at least once during convergence.
  TEST_ASSERT_GREATER_THAN_INT(0, eng.pendingApplyCount());
}

// 7b. test_brightness_multipliers_chain_crowd_then_salty
// effectiveBrightness() in production chains applyCrowdDim then
// applySaltyDim. With crowd-dim settled to ~0.5 and Salty at full 0.7
// hold, the rendered level should be ≈ 100 * 0.5 * 0.7 = 35. Pinning
// the chained behavior here keeps refactors from accidentally
// reordering or dropping one multiplier.
void test_brightness_multipliers_chain_crowd_then_salty() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Introvert;

  // Settle a crowd-dim factor of ~0.5 (5 Salty peers — W=10 hits the
  // floor). applyCrowdDim(100) ≈ 50.
  settleAt(eng, peers(5, /*disposition=*/1, cfg, "crowd"));
  TEST_ASSERT_INT_WITHIN(2, 50, (int)eng.applyCrowdDim(100));

  // Trigger Salty arrival from a separately-named Salty peer that
  // appears alongside the crowd. The arrival sets saltyActive_=true at
  // g_nowMs; without advancing the clock the envelope is at 1.0 (start
  // of fade-in).
  auto crowd = peers(5, 1, cfg, "crowd");
  auto withArrival = crowd;
  withArrival.push_back(makePeer("freshNemesis", 1, cfg, Color(50, 0, 0, 0)));
  eng.setNearbyOverride(withArrival);
  eng.tick(g_nowMs);

  // Advance into the hold window (fade-in 200ms + 200ms hold-in). At
  // this point the Salty factor is at 0.70 exactly.
  g_nowMs += 400;
  eng.tick(g_nowMs);

  // Chained: applyCrowdDim then applySaltyDim. Crowd-dim factor still
  // ~0.5 (peers held steady), Salty factor 0.70 → 100 * 0.5 * 0.7 = 35.
  const uint8_t afterCrowd = eng.applyCrowdDim(100);
  const uint8_t chained    = eng.applySaltyDim(afterCrowd);
  TEST_ASSERT_INT_WITHIN(3, 35, (int)chained);
}

// 7c. test_brightness_multiplier_identity_when_both_inactive
// No crowd, no salty arrival → both multipliers are pure identity.
void test_brightness_multiplier_identity_when_both_inactive() {
  FakeConfig cfg;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  cfg.socialMode = SocialMode::Ambivert;  // crowd-dim is identity outside Introvert
  // No peers, no arrivals — nothing to do.
  TEST_ASSERT_EQUAL_UINT8(100, eng.applyCrowdDim(100));
  TEST_ASSERT_EQUAL_UINT8(100, eng.applySaltyDim(100));
  TEST_ASSERT_EQUAL_UINT8(100, eng.applySaltyDim(eng.applyCrowdDim(100)));
  // Non-100 baselines pass through unchanged too.
  TEST_ASSERT_EQUAL_UINT8(42, eng.applySaltyDim(eng.applyCrowdDim(42)));
}

// --- Step 5 tests: arrivals + Smitten cycles -----------------------------
//
// These all use the inline-mirrored MockExpressionManager. The
// MockBrightnessOverride is still threaded through for API parity but
// no longer participates in observable behavior — Salty side-eye is now
// a pure in-engine multiplier (see test_arrival_salty_side_eye_only).
// resetEngine() rebuilds the engine fresh; we then begin() with the
// mocks injected.

// 8. test_arrival_fresh_only_after_30s_gap — gap <30s no arrival; >30s fires.
// Uses a FOND peer so the closest-pulse / follow-up-queue paths don't
// interfere — Fond's arrival is a single pulse, period.
void test_arrival_fresh_only_after_30s_gap() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;

  // First sighting → arrival pulse for Fond.
  eng.setNearbyOverride({makePeer("buddy", 4, cfg, Color(255, 0, 128, 0))});
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());

  // Drop the peer, advance 20s, return — within the 30s gap, no new arrival.
  eng.setNearbyOverride({});
  g_nowMs += 20000;
  eng.tick(g_nowMs);
  eng.setNearbyOverride({makePeer("buddy", 4, cfg, Color(255, 0, 128, 0))});
  g_nowMs += 100;
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());  // no new arrival pulse

  // Drop, advance >30s, return — arrival fires again.
  resetMocks(em, bo);
  eng.setNearbyOverride({});
  g_nowMs += 35000;
  eng.tick(g_nowMs);
  eng.setNearbyOverride({makePeer("buddy", 4, cfg, Color(255, 0, 128, 0))});
  g_nowMs += 100;
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());
}

// 9. test_arrival_fires_pulse_for_fond_smitten_with_peer_color
// Fond → 1 arrival pulse in their color (immediate).
// Smitten → arrival pulse comes via the closest-cycle when they're the
//          closest peer (Smitten here is the closest = front of list).
//          The greeting / 5-pulse-follow-up paths are separate.
void test_arrival_fires_pulse_for_fond_smitten_with_peer_color() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;

  const Color smittenColor = Color(255, 80, 200, 0);
  const Color fondColor    = Color(10, 220, 30, 0);
  // Smitten is closest (front of list / highest RSSI).
  eng.setNearbyOverride({
    makePeer("smitten_peer", 5, cfg, smittenColor, -35),
    makePeer("fond_peer",    4, cfg, fondColor,    -55),
  });
  eng.tick(g_nowMs);

  // Two pulses fired in this tick:
  //   - Fond arrival pulse (immediate, in fondColor)
  //   - Smitten closest-transition pulse (in smittenColor)
  // No Salty side-eye, no brightnessOverride apply.
  TEST_ASSERT_EQUAL_INT(2, (int)em.invocations.size());
  TEST_ASSERT_EQUAL_INT(0, (int)bo.applies.size());

  // Iteration-order: fond peer first (index 1 in nearby list iterates
  // second in tickArrivals_, but the closest-Smitten pulse fires from
  // tickClosestSmittenPulse_ which runs AFTER tickArrivals_). So:
  //   invocation[0] = fond arrival
  //   invocation[1] = smitten closest-transition
  TEST_ASSERT_TRUE(em.invocations[0].color == fondColor);
  TEST_ASSERT_TRUE(em.invocations[1].color == smittenColor);
}

// 10. test_arrival_salty_side_eye_only — Salty arrival drives the
//     in-engine applySaltyDim multiplier envelope (1.0 → 0.70 → 1.0).
//     No greeting pulse via expressionManager, no BrightnessOverride
//     apply (the engine no longer routes through the override path).
void test_arrival_salty_side_eye_only() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;

  // Pre-arrival: identity multiplier.
  TEST_ASSERT_EQUAL_UINT8(100, eng.applySaltyDim(100));

  eng.setNearbyOverride({makePeer("nemesis", 1, cfg, Color(120, 30, 40, 0))});
  eng.tick(g_nowMs);

  // No expression pulse, no BrightnessOverride apply — the envelope is
  // local to the engine and surfaces via applySaltyDim.
  TEST_ASSERT_EQUAL_INT(0, (int)em.invocations.size());
  TEST_ASSERT_EQUAL_INT(0, (int)bo.applies.size());

  // Immediately after arrival (t=0): start of fade-in → factor = 1.0.
  TEST_ASSERT_EQUAL_UINT8(100, eng.applySaltyDim(100));

  // Mid-fade-in (100ms into 200ms fade) → factor ≈ 0.85 → applySaltyDim(100) ≈ 85.
  g_nowMs += 100;
  eng.tick(g_nowMs);
  TEST_ASSERT_INT_WITHIN(2, 85, (int)eng.applySaltyDim(100));

  // End of fade-in (t=200ms) → factor = 0.70 exactly.
  g_nowMs += 100;
  eng.tick(g_nowMs);
  TEST_ASSERT_INT_WITHIN(1, 70, (int)eng.applySaltyDim(100));

  // Mid-hold (t=400ms, halfway through the 400ms hold) → still 0.70.
  g_nowMs += 200;
  eng.tick(g_nowMs);
  TEST_ASSERT_INT_WITHIN(1, 70, (int)eng.applySaltyDim(100));

  // End of hold (t=600ms) → still at 0.70, start of fade-out.
  g_nowMs += 200;
  eng.tick(g_nowMs);
  TEST_ASSERT_INT_WITHIN(1, 70, (int)eng.applySaltyDim(100));

  // After fade-out completes (t = 200 + 400 + 200 + slack), envelope
  // releases — identity multiplier returns, no BrightnessOverride
  // restore was ever called.
  g_nowMs += 200 + 10;
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_UINT8(100, eng.applySaltyDim(100));
  TEST_ASSERT_EQUAL_INT(0, (int)bo.restores.size());
}

// 18. test_smitten_after_arrival_5_pulses_at_5s_cadence
// Smitten peer that is NOT the closest (a Neutral peer is closer) →
// follow-up queue runs to completion: 5 pulses at +5, +10, +15, +20, +25s.
void test_smitten_after_arrival_5_pulses_at_5s_cadence() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  const Color smittenColor = Color(200, 50, 200, 0);
  // Neutral peer is closest (-30); Smitten is farther (-60) so the
  // closest-cycle won't claim them, and the 5-pulse follow-up runs.
  eng.setNearbyOverride({
    makePeer("neighbor", 3, cfg, Color(80, 80, 80, 0), -30),
    makePeer("crush",    5, cfg, smittenColor,         -60),
  });
  eng.tick(g_nowMs);
  // On first tick: no arrival pulse (Smitten arrival doesn't fire one),
  // no closest-pulse (closest is Neutral). Queue is seeded but won't fire
  // for 5s (kGreetingMaxFramesMs).
  TEST_ASSERT_EQUAL_INT(0, (int)em.invocations.size());

  // Advance in 5s+ steps; each crosses one follow-up's nextFireMs.
  for (int i = 0; i < 5; ++i) {
    g_nowMs += 5000 + 50;
    eng.tick(g_nowMs);
  }
  // 5 follow-up pulses fired in the crush's color.
  TEST_ASSERT_EQUAL_INT(5, (int)em.invocations.size());
  for (const auto& inv : em.invocations) {
    TEST_ASSERT_TRUE(inv.color == smittenColor);
  }

  // Beyond the queue: more time → no further pulses.
  g_nowMs += 10000;
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(5, (int)em.invocations.size());
}

// 19. test_smitten_after_arrival_queue_survives_peer_departure
// Same Neutral-closest setup as test 18 so the Smitten queue is seeded.
// Then drop the Smitten peer entirely and verify the queue still drains.
void test_smitten_after_arrival_queue_survives_peer_departure() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  const Color color = Color(80, 200, 80, 0);
  eng.setNearbyOverride({
    makePeer("neighbor", 3, cfg, Color(80, 80, 80, 0), -30),
    makePeer("crush",    5, cfg, color,                -60),
  });
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(0, (int)em.invocations.size());  // no immediate pulses

  // Peer leaves (only the Neutral remains).
  eng.setNearbyOverride({
    makePeer("neighbor", 3, cfg, Color(80, 80, 80, 0), -30),
  });
  for (int i = 0; i < 5; ++i) {
    g_nowMs += 5050;
    eng.tick(g_nowMs);
  }
  // All 5 follow-up pulses fired despite the Smitten peer being absent.
  TEST_ASSERT_EQUAL_INT(5, (int)em.invocations.size());
}

// 20. test_smitten_closest_pulse_fires_on_transition_and_every_45s
void test_smitten_closest_pulse_fires_on_transition_and_every_45s() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;

  // Single Smitten peer = the closest. First tick: closest-transition
  // fires 1 pulse (and erases the would-be follow-up queue entry).
  eng.setNearbyOverride({makePeer("crush", 5, cfg, Color(200, 0, 200, 0), -40)});
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());

  // Tick frequently for the next 30s so lastSeenBleMs_ stays current
  // (prevents the 30s gap from accidentally triggering a re-arrival).
  // 30s < 45s closest cadence, so no new pulses expected.
  for (int t = 0; t < 30; ++t) {
    g_nowMs += 1000;
    eng.tick(g_nowMs);
  }
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());

  // Advance another 16s (total 46s since the transition). Closest cadence
  // fires its second pulse.
  for (int t = 0; t < 16; ++t) {
    g_nowMs += 1000;
    eng.tick(g_nowMs);
  }
  TEST_ASSERT_EQUAL_INT(2, (int)em.invocations.size());

  // Another 45s of ticks → another pulse.
  for (int t = 0; t < 45; ++t) {
    g_nowMs += 1000;
    eng.tick(g_nowMs);
  }
  TEST_ASSERT_EQUAL_INT(3, (int)em.invocations.size());
}

// 21. test_smitten_closest_pulse_resets_when_closest_changes
void test_smitten_closest_pulse_resets_when_closest_changes() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;

  // Crush A is closest (higher RSSI = closer = less negative).
  const Color colorA = Color(255, 0, 0, 0);
  const Color colorB = Color(0, 0, 255, 0);
  eng.setNearbyOverride({
    makePeer("crushA", 5, cfg, colorA, -40),
    makePeer("crushB", 5, cfg, colorB, -55),
  });
  eng.tick(g_nowMs);
  // A is closest → closest-transition fires for A. B is Smitten but not
  // closest → its follow-up queue is seeded (no immediate pulse). Total
  // pulses this tick = 1.
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());
  TEST_ASSERT_TRUE(em.invocations[0].color == colorA);

  // Now B becomes closest — engine should fire an immediate pulse in B's color.
  resetMocks(em, bo);
  eng.setNearbyOverride({
    makePeer("crushB", 5, cfg, colorB, -30),  // now closer
    makePeer("crushA", 5, cfg, colorA, -55),
  });
  g_nowMs += 100;  // small advance
  eng.tick(g_nowMs);
  // Exactly one new pulse (B's closest transition); no arrival pulses
  // (gap < 30s for both peers).
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());
  TEST_ASSERT_TRUE(em.invocations[0].color == colorB);
}

// 22. test_smitten_closest_pulse_does_not_fire_if_closest_is_non_smitten
void test_smitten_closest_pulse_does_not_fire_if_closest_is_non_smitten() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;

  // Neutral lamp is closest; Smitten is further away.
  eng.setNearbyOverride({
    makePeer("neut",  3, cfg, Color(80, 80, 80, 0), -35),  // closest
    makePeer("crush", 5, cfg, Color(200, 0, 200, 0), -65),
  });
  eng.tick(g_nowMs);
  // Smitten arrival doesn't fire a pulse (queue is seeded silently),
  // Neutral arrival is a no-op, closest is Neutral so no closest-pulse.
  TEST_ASSERT_EQUAL_INT(0, (int)em.invocations.size());

  // Tick every second for 50s so lastSeenBleMs_ stays current. The
  // follow-up queue fires its 5 pulses at +5, +10, +15, +20, +25s — but
  // we're testing that the *closest-cycle* doesn't fire. After 50s the
  // queue is fully drained = exactly 5 pulses, all in Smitten's color.
  // No closest-pulse because closest is Neutral throughout.
  for (int t = 0; t < 50; ++t) {
    g_nowMs += 1000;
    eng.tick(g_nowMs);
  }
  TEST_ASSERT_EQUAL_INT(5, (int)em.invocations.size());
  for (const auto& inv : em.invocations) {
    // All 5 are the Smitten color (follow-up pulses), not the Neutral color.
    TEST_ASSERT_TRUE(inv.color == Color(200, 0, 200, 0));
  }
}

// --- Step 6 tests: Fond bleed contribution math (additive layer) -------

// 11. test_bleed_contribution_fade_in_for_fond_peer
// Single Fond peer → contribution alpha ramps 0 → kBleedAlphaPerPeer
// over kBleedFadeInMs; halfway through the ramp it's ≈ 50% of full.
void test_bleed_contribution_fade_in_for_fond_peer() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  const Color peerColor(200, 100, 0, 0);
  eng.setNearbyOverride({makePeer("buddy", 4, cfg, peerColor)});

  // First tick at t=0 seeds the fade-in. Contribution is zero (elapsed=0).
  eng.tick(g_nowMs);
  auto contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(0, (int)contribs.size());  // sub-perceptible

  // Half-way through the 4s fade-in (t = 2000ms).
  g_nowMs += 2000;
  eng.tick(g_nowMs);
  contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)contribs.size());
  // Expected ≈ 0.5 * kBleedAlphaPerPeer = 0.10.
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.10f, contribs[0].alpha);
  TEST_ASSERT_TRUE(contribs[0].color == peerColor);

  // Full fade-in completed (t = 4000ms total).
  g_nowMs += 2000;
  eng.tick(g_nowMs);
  contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)contribs.size());
  TEST_ASSERT_FLOAT_WITHIN(0.005f, PersonalityEngine::kBleedAlphaPerPeer,
                           contribs[0].alpha);
}

// 12. test_bleed_contribution_fade_out_after_peer_leaves
// Settled-in Fond peer drops out → alpha decays over kBleedFadeOutMs
// and the entry is erased once the fade-out completes.
void test_bleed_contribution_fade_out_after_peer_leaves() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  const Color peerColor(0, 200, 100, 0);
  eng.setNearbyOverride({makePeer("buddy", 4, cfg, peerColor)});

  // Settle: fade-in fully (4s) + a hold tick.
  for (int t = 0; t < 5; ++t) { g_nowMs += 1000; eng.tick(g_nowMs); }
  auto contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)contribs.size());
  TEST_ASSERT_FLOAT_WITHIN(0.005f, PersonalityEngine::kBleedAlphaPerPeer,
                           contribs[0].alpha);

  // Peer drops. First tick after drop arms fade-out (alpha still ≈ full).
  eng.setNearbyOverride({});
  g_nowMs += 100;
  eng.tick(g_nowMs);

  // Halfway through 3s fade-out (t ≈ +1500ms).
  g_nowMs += 1500;
  eng.tick(g_nowMs);
  contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)contribs.size());
  TEST_ASSERT_FLOAT_WITHIN(0.025f,
                           0.5f * PersonalityEngine::kBleedAlphaPerPeer,
                           contribs[0].alpha);

  // Fade-out fully elapsed — entry erased.
  g_nowMs += 2000;
  eng.tick(g_nowMs);
  contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(0, (int)contribs.size());
  TEST_ASSERT_EQUAL_INT(0, (int)eng.bleedStateMapSize());
}

// 13. test_bleed_contributions_blend_multiple_fond_peers
// Two Fond peers, both fully faded-in → exactly two contributions,
// each at kBleedAlphaPerPeer (no cap hit at sum = 0.4 < 0.6).
void test_bleed_contributions_blend_multiple_fond_peers() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  const Color rose(200, 0, 0, 0);
  const Color blue(0, 0, 200, 0);
  eng.setNearbyOverride({
    makePeer("rose", 4, cfg, rose),
    makePeer("blue", 4, cfg, blue),
  });
  for (int t = 0; t < 5; ++t) { g_nowMs += 1000; eng.tick(g_nowMs); }
  auto contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(2, (int)contribs.size());
  float sum = 0.0f;
  bool sawRose = false, sawBlue = false;
  for (const auto& c : contribs) {
    sum += c.alpha;
    TEST_ASSERT_FLOAT_WITHIN(0.005f, PersonalityEngine::kBleedAlphaPerPeer,
                             c.alpha);
    if (c.color == rose) sawRose = true;
    if (c.color == blue) sawBlue = true;
  }
  TEST_ASSERT_TRUE(sawRose);
  TEST_ASSERT_TRUE(sawBlue);
  // Sum stays below the cap.
  TEST_ASSERT_LESS_OR_EQUAL_FLOAT(PersonalityEngine::kBleedMaxAlpha, sum);
}

// 13b. test_bleed_contribution_capped_at_max_alpha
// 5 fully-faded Fond peers → sum of raw alphas = 5 * 0.20 = 1.0,
// over the 0.60 cap; engine scales every contribution proportionally
// so the perceptual mix is preserved.
void test_bleed_contribution_capped_at_max_alpha() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  std::vector<NearbyLamp> peers5;
  for (int i = 0; i < 5; ++i) {
    const std::string name = std::string("f") + std::to_string(i);
    peers5.push_back(makePeer(name.c_str(), 4, cfg,
                              Color(20 * (i + 1), 100, 100, 0)));
  }
  eng.setNearbyOverride(peers5);
  for (int t = 0; t < 5; ++t) { g_nowMs += 1000; eng.tick(g_nowMs); }
  auto contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(5, (int)contribs.size());
  float sum = 0.0f;
  for (const auto& c : contribs) sum += c.alpha;
  // Summed alpha clamped to kBleedMaxAlpha exactly.
  TEST_ASSERT_FLOAT_WITHIN(0.005f, PersonalityEngine::kBleedMaxAlpha, sum);
  // Each individual contribution scaled by the same factor →
  // pre-scale each was kBleedAlphaPerPeer, post-scale all match within
  // float epsilon at kBleedMaxAlpha / 5 = 0.12.
  const float expectedEach = PersonalityEngine::kBleedMaxAlpha / 5.0f;
  for (const auto& c : contribs) {
    TEST_ASSERT_FLOAT_WITHIN(0.005f, expectedEach, c.alpha);
  }
}

// 14. test_bleed_contributes_for_smitten_peer
// Smitten is an escalation of Fond — Smitten peers bleed the same way Fond
// peers do (same per-peer alpha). The closest-Smitten pulse cycle is a
// SEPARATE effect on top; this test pins only the bleed contribution.
void test_bleed_contributes_for_smitten_peer() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  eng.setNearbyOverride({makePeer("crush", 5, cfg, Color(255, 0, 255, 0))});
  // Tick long enough for fade-in to complete (kBleedFadeInMs=4s + slack).
  for (int t = 0; t < 6; ++t) { g_nowMs += 1000; eng.tick(g_nowMs); }
  const auto contribs = eng.getBleedContributions(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)contribs.size());
  TEST_ASSERT_EQUAL_INT(1, (int)eng.bleedStateMapSize());
  TEST_ASSERT_FLOAT_WITHIN(0.005f,
                           PersonalityEngine::kBleedAlphaPerPeer,
                           contribs.front().alpha);
}

// 14b. test_bleed_no_contribution_for_no_peers
// Empty nearby snapshot → empty contributions, no leftover state.
void test_bleed_no_contribution_for_no_peers() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  eng.setNearbyOverride({});
  for (int t = 0; t < 3; ++t) { g_nowMs += 1000; eng.tick(g_nowMs); }
  TEST_ASSERT_EQUAL_INT(0, (int)eng.getBleedContributions(g_nowMs).size());
  TEST_ASSERT_EQUAL_INT(0, (int)eng.bleedStateMapSize());
}

// 23. test_smitten_closest_picks_highest_rssi_regardless_of_insertion_order
// Production `getReachableViaBle()` had a long-standing bug: the header
// comment claimed the result was sorted by RSSI descending (so consumers
// could do peers.front() for the closest) but the sort was never actually
// performed. With multiple Smitten peers, the wrong one would be picked
// as "closest". This test mirrors the new sort behavior — peer inserted
// LAST but with the strongest RSSI must be the chosen closest.
void test_smitten_closest_picks_highest_rssi_regardless_of_insertion_order() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  const Color colorWeak   = Color(255, 0, 0, 0);  // red, far away
  const Color colorStrong = Color(0, 0, 255, 0);  // blue, closest
  // Author insertion order: weak peer first, strong peer second.
  // Without the sort fix, peers.front() would be the WEAK peer's color.
  eng.setNearbyOverride({
    makePeer("far",  5, cfg, colorWeak,   -70),  // inserted first
    makePeer("near", 5, cfg, colorStrong, -35),  // inserted second
  });
  eng.tick(g_nowMs);
  // First non-arrival pulse fires from the closest-transition path. With
  // the sort fix, that pulse should be in the strong/closest peer's color.
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());
  TEST_ASSERT_TRUE(em.invocations[0].color == colorStrong);
}

// 25. test_smitten_closest_cadence_resets_after_disposition_demotion
// Cadence clock must not carry across a Smitten→non-Smitten flip. If
// "crush" is Smitten and the cadence just fired at t=100, then crush is
// remotely demoted to Neutral at t=110, then re-promoted to Smitten at
// t=130 (still within the 45s cadence window from t=100), the next
// pulse should come from a FRESH transition at t=130, NOT from the
// stale clock that would have fired at t=145.
void test_smitten_closest_cadence_resets_after_disposition_demotion() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  const Color color = Color(255, 0, 255, 0);
  // Smitten "crush" is closest. First tick fires transition pulse.
  eng.setNearbyOverride({makePeer("crush", 5, cfg, color, -40)});
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());

  // Demote to Neutral. closestSmittenName_ should clear AND
  // lastClosestPulseMs_ should reset to 0.
  cfg.dispositions["crush"] = 3;
  g_nowMs += 10000;
  eng.tick(g_nowMs);
  // No new pulse — closest is no longer Smitten.
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());

  // Re-promote within the 45s cadence window. With the cadence-reset fix,
  // this fires a fresh transition pulse. Without the fix, the stale clock
  // from t=0 would have already exceeded 45s by now (g_nowMs is around
  // 10000+) — actually no, 10s < 45s, so the cadence WOULDN'T fire under
  // either behavior here. The transition path always fires on a name
  // change (or first-time empty closestSmittenName_), so this verifies
  // the transition happens cleanly.
  cfg.dispositions["crush"] = 5;
  g_nowMs += 1000;
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(2, (int)em.invocations.size());

  // 44s later: cadence has NOT yet elapsed since the fresh transition
  // (lastClosestPulseMs_ was reset to the re-promotion time, ~t=11000).
  // Without the reset fix, lastClosestPulseMs_ would still point to ~0,
  // and 44s + 11s = 55s > 45s would have fired a pulse.
  g_nowMs += 44000;  // total since re-promotion: 44s
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(2, (int)em.invocations.size());  // no stale-clock fire

  // 2s later (46s since re-promotion): cadence fires.
  g_nowMs += 2000;
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(3, (int)em.invocations.size());
}

// 24. test_smitten_closest_hysteresis_absorbs_small_rssi_noise
// Two Smitten peers with nearly identical RSSI. Without hysteresis, ±1
// dBm noise (well below the 3 dB hysteresis) would flap "closest" every
// tick and strobe a pulse on every flip. Verify only ONE pulse fires
// across many ticks of alternation.
void test_smitten_closest_hysteresis_absorbs_small_rssi_noise() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  const Color colorA = Color(255, 0, 0, 0);
  const Color colorB = Color(0, 255, 0, 0);

  // Initial state: A is closest by 2 dB (sub-threshold). One transition fires.
  eng.setNearbyOverride({
    makePeer("A", 5, cfg, colorA, -50),
    makePeer("B", 5, cfg, colorB, -52),
  });
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());
  TEST_ASSERT_TRUE(em.invocations[0].color == colorA);

  // Now flap: B nudges 1 dB stronger, then A regains 1 dB, etc. Each
  // tick the "front" alternates between A and B, but the delta is only
  // 1-2 dB — below kRssiHysteresisDb=3. No new transitions should fire.
  for (int i = 1; i <= 20; ++i) {
    const int8_t aRssi = (i % 2 == 0) ? -50 : -51;
    const int8_t bRssi = (i % 2 == 0) ? -51 : -50;
    eng.setNearbyOverride({
      makePeer("A", 5, cfg, colorA, aRssi),
      makePeer("B", 5, cfg, colorB, bRssi),
    });
    g_nowMs += 200;
    eng.tick(g_nowMs);
  }
  // Still just the original 1 pulse — hysteresis absorbed the noise.
  TEST_ASSERT_EQUAL_INT(1, (int)em.invocations.size());

  // But a decisive 5 dB jump (B now -45 vs A -50) should transition.
  eng.setNearbyOverride({
    makePeer("A", 5, cfg, colorA, -50),
    makePeer("B", 5, cfg, colorB, -45),  // 5 dB stronger than A
  });
  g_nowMs += 200;
  eng.tick(g_nowMs);
  TEST_ASSERT_EQUAL_INT(2, (int)em.invocations.size());
  TEST_ASSERT_TRUE(em.invocations[1].color == colorB);
}

// 14c. test_bleed_state_map_evicts_oldest_at_cap
// Push more than kMaxTrackedPeers distinct Fond peers through. The
// engine drops the oldest (lowest lastSeenMs) so the map stays bounded.
// Guards against the unbounded-growth leak the audit flagged.
void test_bleed_state_map_evicts_oldest_at_cap() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;

  // Walk 40 distinct Fond peers past the lamp. Each peer is "present"
  // for one tick then gone; the LRU eviction is keyed on lastSeenMs so
  // the oldest entries fall out first.
  for (int i = 0; i < 40; ++i) {
    const std::string name = std::string("fond") + std::to_string(i);
    cfg.dispositions[name] = 4;
    NearbyLamp p;
    p.name = name;
    p.baseColor = Color(static_cast<uint8_t>(i * 5), 100, 100, 0);
    eng.setNearbyOverride({p});
    g_nowMs += 100;
    eng.tick(g_nowMs);
  }
  TEST_ASSERT_LESS_OR_EQUAL_INT(
      (int)PersonalityEngine::kMaxTrackedPeers,
      (int)eng.bleedStateMapSize());
}

// 14. test_bleed_contribution_excludes_wary_neutral_salty
// Bleed thresholds at disposition >= 4 — Fond (4) and Smitten (5) bleed;
// Wary (2), Neutral (3), Salty (1) do not. With only sub-Fond peers
// present, contributions are empty.
void test_bleed_contribution_excludes_wary_neutral_salty() {
  FakeConfig cfg;
  MockExpressionManager em;
  MockBrightnessOverride bo;
  PersonalityEngine eng;
  resetEngine(eng, cfg);
  eng.begin(&cfg, &bo, &em);
  cfg.socialMode = SocialMode::Ambivert;
  eng.setNearbyOverride({
    makePeer("wary1",   2, cfg, Color(50, 50, 50, 0)),   // Wary
    makePeer("neut",    3, cfg, Color(80, 80, 80, 0)),   // Neutral
    makePeer("nemesis", 1, cfg, Color(100, 0, 0, 0)),    // Salty
  });
  for (int t = 0; t < 6; ++t) { g_nowMs += 1000; eng.tick(g_nowMs); }
  TEST_ASSERT_EQUAL_INT(0, (int)eng.getBleedContributions(g_nowMs).size());
  TEST_ASSERT_EQUAL_INT(0, (int)eng.bleedStateMapSize());
}

}  // namespace test

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test::test_crowd_weight_curve_neutral_peers);
  RUN_TEST(test::test_crowd_weight_smitten_is_zero);
  RUN_TEST(test::test_crowd_weight_salty_double);
  RUN_TEST(test::test_crowd_weight_mixed);
  RUN_TEST(test::test_dim_only_when_introvert);
  RUN_TEST(test::test_dim_deadband_absorbs_alternation);
  RUN_TEST(test::test_dim_ema_step_response);
  RUN_TEST(test::test_brightness_multipliers_chain_crowd_then_salty);
  RUN_TEST(test::test_brightness_multiplier_identity_when_both_inactive);
  RUN_TEST(test::test_greeting_tuning_matrix);
  RUN_TEST(test::test_greeting_for_salty_is_skip_in_all_modes);
  RUN_TEST(test::test_greeting_for_unknown_peer_is_neutral);
  RUN_TEST(test::test_arrival_fresh_only_after_30s_gap);
  RUN_TEST(test::test_arrival_fires_pulse_for_fond_smitten_with_peer_color);
  RUN_TEST(test::test_arrival_salty_side_eye_only);
  RUN_TEST(test::test_smitten_after_arrival_5_pulses_at_5s_cadence);
  RUN_TEST(test::test_smitten_after_arrival_queue_survives_peer_departure);
  RUN_TEST(test::test_smitten_closest_pulse_fires_on_transition_and_every_45s);
  RUN_TEST(test::test_smitten_closest_pulse_resets_when_closest_changes);
  RUN_TEST(test::test_smitten_closest_pulse_does_not_fire_if_closest_is_non_smitten);
  RUN_TEST(test::test_bleed_contribution_fade_in_for_fond_peer);
  RUN_TEST(test::test_bleed_contribution_fade_out_after_peer_leaves);
  RUN_TEST(test::test_bleed_contributions_blend_multiple_fond_peers);
  RUN_TEST(test::test_bleed_contribution_capped_at_max_alpha);
  RUN_TEST(test::test_bleed_contributes_for_smitten_peer);
  RUN_TEST(test::test_bleed_no_contribution_for_no_peers);
  RUN_TEST(test::test_bleed_contribution_excludes_wary_neutral_salty);
  RUN_TEST(test::test_bleed_state_map_evicts_oldest_at_cap);
  RUN_TEST(test::test_smitten_closest_picks_highest_rssi_regardless_of_insertion_order);
  RUN_TEST(test::test_smitten_closest_hysteresis_absorbs_small_rssi_noise);
  RUN_TEST(test::test_smitten_closest_cadence_resets_after_disposition_demotion);

  return UNITY_END();
}
