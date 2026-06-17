#include "./palette_cycle_expression.hpp"

#include <algorithm>
#include <vector>

#include "../util/gradient.hpp"

namespace lamp {
namespace {

std::vector<Color> rotatePalette(const std::vector<Color>& palette, size_t offset) {
  if (palette.empty()) {
    return {};
  }

  std::vector<Color> rotated;
  rotated.reserve(palette.size());

  for (size_t i = 0; i < palette.size(); i++) {
    rotated.push_back(palette[(i + offset) % palette.size()]);
  }

  return rotated;
}

}  // namespace

PaletteCycleExpression::PaletteCycleExpression(FrameBuffer* inBuffer, uint32_t inFrames)
    : Expression(inBuffer, inFrames) {
  allowedInHomeMode = true;
}

void PaletteCycleExpression::configureFromParameters(const std::map<std::string, uint32_t>& parameters) {
  auto it = parameters.find("stepDuration");
  uint32_t stepDuration = (it != parameters.end()) ? it->second : 20;

  stepDuration = std::clamp(stepDuration, 5u, 120u);
  stepDurationFrames = stepDuration * 30;
}

void PaletteCycleExpression::onTrigger() {
  if (colors.empty() && !savedBuffer.empty()) {
    colors = {savedBuffer.front()};
  }

  if (colors.size() == 1) {
    colors.push_back(colors.front());
  }

  frames = std::max<uint32_t>(stepDurationFrames, 1u) *
           static_cast<uint32_t>(std::max<size_t>(colors.size(), 1));
  frame = 0;
}

void PaletteCycleExpression::draw() {
  if (shouldPause()) return;

  if (!shouldAffectBuffer()) {
    nextFrame();
    return;
  }

  if (colors.empty() || fb->pixelCount == 0) {
    nextFrame();
    return;
  }

  const size_t phaseCount = std::max<size_t>(colors.size(), 1);
  const uint32_t phaseFrames = std::max<uint32_t>(stepDurationFrames, 1u);
  const size_t phaseIndex = std::min<size_t>(frame / phaseFrames, phaseCount - 1);
  const uint32_t localFrame = frame % phaseFrames;

  std::vector<Color> startStops = rotatePalette(colors, phaseIndex);
  std::vector<Color> endStops = rotatePalette(colors, (phaseIndex + 1) % phaseCount);
  std::vector<Color> startGradient = buildGradientWithStops(fb->pixelCount, startStops);
  std::vector<Color> endGradient = buildGradientWithStops(fb->pixelCount, endStops);

  for (int i = 0; i < fb->pixelCount; i++) {
    fb->buffer[i] = fadeLinear(startGradient[i], endGradient[i], phaseFrames, localFrame);
  }

  nextFrame();
}

}  // namespace lamp
