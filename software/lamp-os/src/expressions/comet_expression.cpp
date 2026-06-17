#include "./comet_expression.hpp"

#include <Arduino.h>
#include <algorithm>

#include "../util/fade.hpp"

namespace lamp {

static constexpr uint32_t COMET_MAX_FRAMES = 10000;

CometExpression::CometExpression(FrameBuffer* inBuffer, uint32_t inFrames)
    : Expression(inBuffer, inFrames) {
  allowedInHomeMode = true;
}

void CometExpression::configureFromParameters(const std::map<std::string, uint32_t>& parameters) {
  auto speedIt = parameters.find("cometSpeed");
  auto tailIt = parameters.find("cometTail");

  uint32_t cometSpeed = std::clamp((speedIt != parameters.end()) ? speedIt->second : 8u, 2u, 30u);
  cometTail = std::clamp((tailIt != parameters.end()) ? tailIt->second : 8u, 2u, 20u);

  uint32_t pixelSpan = fb ? static_cast<uint32_t>(fb->pixelCount + cometTail) : cometTail;
  pixelSpan = std::max<uint32_t>(pixelSpan, 1);
  pixelTravelMs = std::max<uint32_t>(40, (cometSpeed * 1000) / pixelSpan);
}

uint32_t CometExpression::calculateBlendFactor(int pixelIndex) const {
  float tailDistance = cometPosition - static_cast<float>(pixelIndex);

  if (tailDistance < 0.0f || tailDistance > static_cast<float>(cometTail)) {
    return 0;
  }

  if (tailDistance < 0.5f) {
    return 100;
  }

  float factor = 1.0f - (tailDistance / static_cast<float>(cometTail + 1));
  factor = std::max(0.0f, factor);
  return static_cast<uint32_t>(factor * 100.0f);
}

void CometExpression::updatePosition() {
  uint32_t currentMs = millis();

  if (lastUpdateMs == 0) {
    lastUpdateMs = currentMs;
    return;
  }

  uint32_t deltaMs = currentMs - lastUpdateMs;
  cometPosition += static_cast<float>(deltaMs) / static_cast<float>(pixelTravelMs);
  lastUpdateMs = currentMs;
}

void CometExpression::onTrigger() {
  cometPosition = -static_cast<float>(cometTail);
  lastUpdateMs = 0;
  cometColor = colors.empty() ? Color(255, 255, 255, 255) : getRandomColor();
  frames = COMET_MAX_FRAMES;
  frame = 0;
}

void CometExpression::onUpdate() {
  updatePosition();
}

void CometExpression::draw() {
  if (shouldPause()) return;

  if (!shouldAffectBuffer()) {
    nextFrame();
    return;
  }

  fb->buffer = savedBuffer;

  for (int i = 0; i < fb->pixelCount; i++) {
    uint32_t blendFactor = calculateBlendFactor(i);
    if (blendFactor > 0) {
      fb->buffer[i] = fadeLinear(savedBuffer[i], cometColor, 100, blendFactor);
    }
  }

  nextFrame();

  if (cometPosition > fb->pixelCount + cometTail) {
    stop();
  }
}

}  // namespace lamp
