#ifndef LAMP_BEHAVIORS_SOCIAL_H
#define LAMP_BEHAVIORS_SOCIAL_H

#include <map>
#include <string>
#include <vector>

#include "../config/config_types.hpp"
#include "../core/animated_behavior.hpp"
#include "../util/color.hpp"

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
  // how many frames to ease when greeting and returning to the lamp's
  // personality- the total frame count must be a multiple of the ease frames
  uint32_t easeFrames = 120;
  uint32_t nextAcknowledgeTimeMs = 0;
  Color foundLampColor;

  void draw() override;
  void control() override;

  // Wires the live Config so control() can read the current socialMode.
  // No setter = behaves as Ambivert (the spec's pre-personality default).
  void setConfig(Config* config) { config_ = config; }

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
};

}  // namespace lamp

#endif
