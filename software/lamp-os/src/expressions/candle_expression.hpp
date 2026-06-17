#ifndef LAMP_EXPRESSIONS_CANDLE_H
#define LAMP_EXPRESSIONS_CANDLE_H

#include <vector>

#include "./expression.hpp"

namespace lamp {

/**
 * @brief Subtle warm flicker inspired by candlelight.
 */
class CandleExpression : public Expression {
 private:
  uint32_t runDurationFrames = 900;  // 30 seconds
  uint8_t flickerAmount = 18;
  uint32_t segmentFrames = 4;
  uint32_t segmentStartFrame = 0;
  Color candleColor;
  std::vector<Color> segmentStartColors;
  std::vector<Color> segmentTargetColors;

  void buildNextSegment();

 public:
  using Expression::Expression;

  CandleExpression(FrameBuffer* inBuffer, uint32_t inFrames = 900);

  void configureFromParameters(const std::map<std::string, uint32_t>& parameters);

  void draw() override;

 protected:
  void onTrigger() override;
  void onUpdate() override;
};

}  // namespace lamp

#endif
