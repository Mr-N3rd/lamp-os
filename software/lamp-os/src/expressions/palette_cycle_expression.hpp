#ifndef LAMP_EXPRESSIONS_PALETTE_CYCLE_H
#define LAMP_EXPRESSIONS_PALETTE_CYCLE_H

#include "./expression.hpp"

namespace lamp {

/**
 * @brief Slowly rotates a configured palette across the target strip.
 */
class PaletteCycleExpression : public Expression {
 private:
  uint32_t stepDurationFrames = 600;  // 20 seconds at 30fps

 public:
  using Expression::Expression;

  PaletteCycleExpression(FrameBuffer* inBuffer, uint32_t inFrames = 600);

  void configureFromParameters(const std::map<std::string, uint32_t>& parameters);

  void draw() override;

 protected:
  void onTrigger() override;
};

}  // namespace lamp

#endif
