#ifndef LAMP_BEHAVIORS_SOCIAL_H
#define LAMP_BEHAVIORS_SOCIAL_H

#include <string>
#include <vector>

#include "../components/network/bluetooth.hpp"
#include "../config/config_types.hpp"
#include "../core/animated_behavior.hpp"
#include "../util/color.hpp"

/**
 * @brief social color exchange
 *
 * SocialBehavior reacts to nearby lamps discovered via Bluetooth by briefly
 * blending to their base color as a greeting animation.
 *
 * The behavior is governed by SocialSettings loaded from config:
 *   - social.mode       selects the lamp's social interaction style
 *   - social.friends    filters which lamps can trigger shy-mode greetings
 *   - social.cooldownMs replaces the previous compile-time constant
 *
 * Wire social config in by calling configure() after constructing the behavior.
 */
namespace lamp {
class SocialBehavior : public AnimatedBehavior {
  using AnimatedBehavior::AnimatedBehavior;

 public:
  enum class SocialMode {
    BUTTERFLY,
    SHY,
    GREET,
    OFF,
  };

  // how many frames to ease when greeting and returning to the lamp's
  // personality- the total frame count must be a multiple of the ease frames
  uint32_t easeFrames = 120;
  uint32_t nextAcknowledgeTimeMs = 0;
  Color foundLampColor;
  BluetoothComponent* bt;
  std::vector<BluetoothLampRecord>* foundLamps;
  bool allowedInHomeMode = false;

  // Social config applied via configure()
  SocialMode socialMode = SocialMode::GREET;
  std::vector<std::string> socialFriends;
  uint32_t socialCooldownMs = 30000;

  void draw() override;

  void control() override;

  void setBluetoothComponent(BluetoothComponent* inBt);

  /**
   * @brief Apply social settings from config to this behavior.
   * Call this in initBehaviors() after loading config.
   * @param [in] settings - the SocialSettings loaded from config
   */
  void configure(const SocialSettings& settings);

 private:
  bool isFriendLamp(const BluetoothLampRecord& lamp) const;
};
}  // namespace lamp
#endif