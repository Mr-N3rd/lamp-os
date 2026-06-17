#ifndef LAMP_EXPRESSIONS_TWINKLE_H
#define LAMP_EXPRESSIONS_TWINKLE_H

#include <vector>

#include "./expression.hpp"

namespace lamp {

/**
 * @brief Gentle sparkle effect for small LED counts.
 */
class TwinkleExpression : public Expression {
 private:
  struct Twinkle {
    uint8_t pixel = 0;
    Color color;
    uint32_t startFrame = 0;
  };

  std::vector<Twinkle> activeTwinkles;
  uint32_t runDurationFrames = 900;      // 30 seconds
  uint32_t twinkleDurationFrames = 90;   // 3 seconds
  uint32_t spawnIntervalFrames = 12;     // 400ms
  uint8_t twinkleCount = 3;

  void addTwinkle();

 public:
  using Expression::Expression;

  TwinkleExpression(FrameBuffer* inBuffer, uint32_t inFrames = 900);

  void configureFromParameters(const std::map<std::string, uint32_t>& parameters);

  void draw() override;

 protected:
  void onTrigger() override;
  void onUpdate() override;
};

}  // namespace lamp

#endif
