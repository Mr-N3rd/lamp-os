#include "./show_behavior.hpp"

namespace lamp {

// Exponential ease per frame: displayed += (target - displayed) * STEP.
// At ~30fps, STEP=0.2 gets to ~80% of a new target in 8 frames (~270ms),
// which matches the repeater's 250ms COLORS cadence so each frame is fully
// settled before the next one arrives.
static constexpr float SHOW_EASE_STEP = 0.20f;

static inline uint8_t easeChannel(uint8_t cur, uint8_t target) {
  int delta = static_cast<int>(target) - static_cast<int>(cur);
  if (delta == 0) return cur;
  // Round half-away-from-zero so we always make progress on small deltas.
  int step = static_cast<int>(delta * SHOW_EASE_STEP);
  if (step == 0) step = (delta > 0) ? 1 : -1;
  return static_cast<uint8_t>(cur + step);
}

void ShowBehavior::control() {
  if (!receiver || !receiver->hasRecentFrame(LAMP_SHOW_FRESH_FRAME_MS)) {
    animationState = STOPPED;
    return;
  }
  animationState = PLAYING;
}

void ShowBehavior::draw() {
  if (!fb || !receiver) return;
  uint8_t shade[4];
  uint8_t base[4];
  if (!receiver->snapshot(shade, base, nullptr, nullptr)) return;

  const uint8_t* target = (side == SHADE) ? shade : base;

  if (!initialized_) {
    displayedR_ = target[0];
    displayedG_ = target[1];
    displayedB_ = target[2];
    displayedW_ = target[3];
    initialized_ = true;
  } else {
    displayedR_ = easeChannel(displayedR_, target[0]);
    displayedG_ = easeChannel(displayedG_, target[1]);
    displayedB_ = easeChannel(displayedB_, target[2]);
    displayedW_ = easeChannel(displayedW_, target[3]);
  }

  fb->fill(Color(displayedR_, displayedG_, displayedB_, displayedW_));
}

}  // namespace lamp
