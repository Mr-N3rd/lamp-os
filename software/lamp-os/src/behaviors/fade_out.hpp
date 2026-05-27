#ifndef LAMP_BEHAVIORS_FADE_OUT_H
#define LAMP_BEHAVIORS_FADE_OUT_H

#include "../core/animated_behavior.hpp"

/**
 * @brief animation to fade to black and reboot. Driven by a global flag
 * (see fade_out.cpp) that any reboot path sets — e.g. BLE settings_blob save.
 * No WiFi dependency.
 */
namespace lamp {

// Set this to true from anywhere that wants a graceful reboot. The lamp
// fades to black over REBOOT_ANIMATION_FRAMES then calls ESP.restart().
extern volatile bool fadeOutRebootRequested;

class FadeOutBehavior : public AnimatedBehavior {
  using AnimatedBehavior::AnimatedBehavior;

 public:
  bool reboot = false;
  void draw() override;
  void control() override;
};
}  // namespace lamp
#endif
