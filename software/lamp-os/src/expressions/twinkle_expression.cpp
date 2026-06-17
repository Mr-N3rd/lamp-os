#include "./twinkle_expression.hpp"

#include <algorithm>

#include "../util/fade.hpp"

namespace lamp {

TwinkleExpression::TwinkleExpression(FrameBuffer* inBuffer, uint32_t inFrames)
    : Expression(inBuffer, inFrames) {
  allowedInHomeMode = true;
}

void TwinkleExpression::configureFromParameters(const std::map<std::string, uint32_t>& parameters) {
  auto countIt = parameters.find("twinkleCount");
  auto fadeIt = parameters.find("twinkleFade");
  auto durationIt = parameters.find("duration");

  twinkleCount = static_cast<uint8_t>(std::clamp((countIt != parameters.end()) ? countIt->second : 3u, 1u, 6u));
  twinkleDurationFrames = std::clamp((fadeIt != parameters.end()) ? fadeIt->second : 3u, 1u, 8u) * 30;
  runDurationFrames = std::clamp((durationIt != parameters.end()) ? durationIt->second : 30u, 5u, 120u) * 30;
  spawnIntervalFrames = std::max<uint32_t>(6, twinkleDurationFrames / 6);
}

void TwinkleExpression::addTwinkle() {
  if (!fb || fb->pixelCount == 0) return;

  std::uniform_int_distribution<int> pixelDist(0, fb->pixelCount - 1);
  Twinkle twinkle;
  twinkle.pixel = pixelDist(rng);
  twinkle.startFrame = frame;

  if (!colors.empty()) {
    twinkle.color = getRandomColor();
  } else if (!savedBuffer.empty()) {
    twinkle.color = savedBuffer[twinkle.pixel];
  } else {
    twinkle.color = Color(255, 255, 255, 255);
  }

  activeTwinkles.push_back(twinkle);
}

void TwinkleExpression::onTrigger() {
  activeTwinkles.clear();
  frames = runDurationFrames;
  frame = 0;

  for (uint8_t i = 0; i < twinkleCount; i++) {
    addTwinkle();
  }
}

void TwinkleExpression::onUpdate() {
  activeTwinkles.erase(
      std::remove_if(activeTwinkles.begin(), activeTwinkles.end(),
                     [&](const Twinkle& twinkle) {
                       return frame - twinkle.startFrame >= twinkleDurationFrames;
                     }),
      activeTwinkles.end());

  if (frame % spawnIntervalFrames == 0) {
    while (activeTwinkles.size() < twinkleCount) {
      addTwinkle();
    }
  }
}

void TwinkleExpression::draw() {
  if (shouldPause()) return;

  if (!shouldAffectBuffer()) {
    nextFrame();
    return;
  }

  fb->buffer = savedBuffer;

  for (const auto& twinkle : activeTwinkles) {
    uint32_t age = frame - twinkle.startFrame;
    if (age >= twinkleDurationFrames) {
      continue;
    }

    uint32_t halfDuration = std::max<uint32_t>(1, twinkleDurationFrames / 2);
    uint32_t blendFactor = 0;

    if (age <= halfDuration) {
      blendFactor = (age * 100) / halfDuration;
    } else {
      uint32_t fadeOutFrames = std::max<uint32_t>(1, twinkleDurationFrames - halfDuration);
      blendFactor = ((twinkleDurationFrames - age) * 100) / fadeOutFrames;
    }

    fb->buffer[twinkle.pixel] =
        fadeLinear(fb->buffer[twinkle.pixel], twinkle.color, 100, blendFactor);

    if (twinkle.pixel > 0) {
      fb->buffer[twinkle.pixel - 1] =
          fadeLinear(fb->buffer[twinkle.pixel - 1], twinkle.color, 100, blendFactor / 3);
    }
    if (twinkle.pixel + 1 < fb->pixelCount) {
      fb->buffer[twinkle.pixel + 1] =
          fadeLinear(fb->buffer[twinkle.pixel + 1], twinkle.color, 100, blendFactor / 3);
    }
  }

  nextFrame();
}

}  // namespace lamp
