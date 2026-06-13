#ifndef LAMP_BEHAVIORS_SHOW_BEHAVIOR_H
#define LAMP_BEHAVIORS_SHOW_BEHAVIOR_H

#include <cstdint>

#include "../components/network/show_receiver.hpp"
#include "../core/animated_behavior.hpp"

#define LAMP_SHOW_FRESH_FRAME_MS 3000

namespace lamp {

// Paints the framebuffer with the latest COLORS frame received from the
// ShowReceiver, with a small per-frame ease so color changes aren't abrupt.
// Active only while a recent frame exists; otherwise STOPPED so lower-priority
// behaviors (expressions, idle) render instead.
class ShowBehavior : public AnimatedBehavior {
  using AnimatedBehavior::AnimatedBehavior;

 public:
  enum Side { SHADE, BASE };

  Side side = SHADE;
  ShowReceiver* receiver = nullptr;

  void control() override;
  void draw() override;

  void setReceiver(ShowReceiver* inReceiver) { receiver = inReceiver; }
  void setSide(Side inSide) { side = inSide; }

 private:
  // Smoothed color the lamp is currently displaying (eases toward target).
  uint8_t displayedR_ = 0;
  uint8_t displayedG_ = 0;
  uint8_t displayedB_ = 0;
  uint8_t displayedW_ = 0;
  bool initialized_ = false;
};

}  // namespace lamp

#endif
