#ifndef LAMP_EXPRESSIONS_COMET_H
#define LAMP_EXPRESSIONS_COMET_H

#include "./expression.hpp"

namespace lamp {

/**
 * @brief Gentle comet pass with a soft trailing tail.
 */
class CometExpression : public Expression {
 private:
  float cometPosition = 0.0f;
  uint32_t cometTail = 8;
  uint32_t pixelTravelMs = 100;
  uint32_t lastUpdateMs = 0;
  Color cometColor;

  uint32_t calculateBlendFactor(int pixelIndex) const;
  void updatePosition();

 public:
  using Expression::Expression;

  CometExpression(FrameBuffer* inBuffer, uint32_t inFrames = 120);

  void configureFromParameters(const std::map<std::string, uint32_t>& parameters);

  void draw() override;

 protected:
  void onTrigger() override;
  void onUpdate() override;
};

}  // namespace lamp

#endif
