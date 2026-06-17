#include "./candle_expression.hpp"

#include <algorithm>

#include "../util/fade.hpp"

namespace lamp {
namespace {

Color scaleColor(Color color, uint32_t percent) {
  return fadeLinear(Color(0, 0, 0, 0), color, 100, std::min<uint32_t>(percent, 100));
}

}  // namespace

CandleExpression::CandleExpression(FrameBuffer* inBuffer, uint32_t inFrames)
    : Expression(inBuffer, inFrames) {
  allowedInHomeMode = true;
}

void CandleExpression::configureFromParameters(const std::map<std::string, uint32_t>& parameters) {
  auto durationIt = parameters.find("duration");
  auto amountIt = parameters.find("flickerAmount");

  runDurationFrames = std::clamp((durationIt != parameters.end()) ? durationIt->second : 30u, 5u, 180u) * 30;
  flickerAmount = static_cast<uint8_t>(
      std::clamp((amountIt != parameters.end()) ? amountIt->second : 18u, 5u, 35u));
}

void CandleExpression::buildNextSegment() {
  segmentTargetColors.assign(fb->pixelCount, candleColor);

  for (uint16_t i = 0; i < fb->pixelCount; i++) {
    uint32_t baseWarmBlend = 65;
    uint32_t brightness = 100 - std::uniform_int_distribution<uint32_t>(0, flickerAmount)(rng);
    Color warmed = fadeLinear(savedBuffer[i], candleColor, 100, baseWarmBlend);
    segmentTargetColors[i] = scaleColor(warmed, brightness);
  }
}

void CandleExpression::onTrigger() {
  candleColor = colors.empty() ? Color(255, 147, 41, 96) : getRandomColor();
  frames = runDurationFrames;
  frame = 0;
  segmentStartFrame = 0;
  segmentStartColors = savedBuffer;
  buildNextSegment();
}

void CandleExpression::onUpdate() {
  if (frame - segmentStartFrame >= segmentFrames) {
    segmentStartFrame = frame;
    segmentStartColors = fb->buffer;
    buildNextSegment();
  }
}

void CandleExpression::draw() {
  if (shouldPause()) return;

  if (!shouldAffectBuffer()) {
    nextFrame();
    return;
  }

  uint32_t localFrame = std::min<uint32_t>(frame - segmentStartFrame, segmentFrames);

  for (uint16_t i = 0; i < fb->pixelCount; i++) {
    fb->buffer[i] =
        fadeLinear(segmentStartColors[i], segmentTargetColors[i], segmentFrames, localFrame);
  }

  nextFrame();
}

}  // namespace lamp
