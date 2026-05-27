#include "./expression_manager.hpp"
#include <Arduino.h>
#include <algorithm>

#include "../core/compositor.hpp"

namespace lamp {

// Define the global frame buffer vector for expressions
std::vector<FrameBuffer*> expressionFrameBuffers;

// Global expression manager pointer
static ExpressionManager* globalExpressionManager = nullptr;

void setGlobalExpressionManager(ExpressionManager* manager) {
  globalExpressionManager = manager;
}

ExpressionManager* getGlobalExpressionManager() {
  return globalExpressionManager;
}

void ExpressionManager::begin(FrameBuffer* shade, FrameBuffer* base) {
  shadeBuffer = shade;
  baseBuffer = base;

  // Set up global frame buffer references
  expressionFrameBuffers.clear();
  expressionFrameBuffers.push_back(shade);
  expressionFrameBuffers.push_back(base);
}

void ExpressionManager::loadFromConfig(const ExpressionSettings& settings) {
  clear();

  for (const auto& config : settings.expressions) {
    addExpression(config);
  }
}


void ExpressionManager::addExpression(const ExpressionConfig& config) {
  if (!shadeBuffer || !baseBuffer) return;

  auto target = static_cast<ExpressionTarget>(config.target);

  // Lambda to create appropriate expression type
  auto createExpression = [&](FrameBuffer* buffer) -> std::unique_ptr<Expression> {
    std::unique_ptr<Expression> expr;

    if (config.type == "glitchy") {
      auto glitchyExpr = std::make_unique<GlitchyExpression>(buffer, 3);
      glitchyExpr->configure(config.colors, config.intervalMin, config.intervalMax, target);
      glitchyExpr->configureFromParameters(config.parameters);
      expr = std::move(glitchyExpr);
    } else if (config.type == "shifty") {
      auto shiftyExpr = std::make_unique<ShiftyExpression>(buffer, 120);
      shiftyExpr->configure(config.colors, config.intervalMin, config.intervalMax, target);
      shiftyExpr->configureFromParameters(config.parameters);
      expr = std::move(shiftyExpr);
    } else if (config.type == "pulse") {
      auto pulseExpr = std::make_unique<PulseExpression>(buffer, 60);
      pulseExpr->configure(config.colors, config.intervalMin, config.intervalMax, target);
      pulseExpr->configureFromParameters(config.parameters);
      expr = std::move(pulseExpr);
    } else if (config.type == "breathing") {
      auto breathingExpr = std::make_unique<BreathingExpression>(buffer, 60);
      breathingExpr->configure(config.colors, config.intervalMin, config.intervalMax, target);
      breathingExpr->configureFromParameters(config.parameters);
      expr = std::move(breathingExpr);
    }

    if (expr) expr->autoTriggerEnabled = config.enabled;
    return expr;
  };

  // Determine target buffers
  std::vector<FrameBuffer*> targetBuffers;
  if (target == TARGET_BOTH) {
    targetBuffers = {shadeBuffer, baseBuffer};
  } else {
    targetBuffers = {(target == TARGET_SHADE) ? shadeBuffer : baseBuffer};
  }

  // Create expressions for each target buffer
  for (auto* buffer : targetBuffers) {
    if (auto expr = createExpression(buffer)) {
      expressions.push_back({std::move(expr), config.type});
    }
  }
}

std::vector<AnimatedBehavior*> ExpressionManager::getBehaviors() {
  std::vector<AnimatedBehavior*> behaviors;
  for (auto& entry : expressions) {
    behaviors.push_back(entry.expression.get());
  }
  return behaviors;
}

void ExpressionManager::clear() {
  expressions.clear();
}

bool ExpressionManager::triggerExpression(const std::string& type) {
  bool triggered = false;
  for (auto& entry : expressions) {
    if (entry.type == type && entry.expression) {
      entry.expression->trigger();
      triggered = true;
    }
  }
  return triggered;
}

bool ExpressionManager::triggerExpression(const std::string& type, ExpressionTarget target) {
  bool triggered = false;
  for (auto& entry : expressions) {
    if (entry.type == type && entry.expression && entry.expression->getTarget() == target) {
      entry.expression->trigger();
      triggered = true;
    }
  }
  return triggered;
}

std::vector<Color> ExpressionManager::getExpressionColors(const std::string& type) const {
  for (const auto& entry : expressions) {
    if (entry.type == type && entry.expression) {
      return entry.expression->getColors();
    }
  }
  return {};
}

void ExpressionManager::upsertExpression(const ExpressionConfig& config, Compositor* compositor) {
  ExpressionTarget target = static_cast<ExpressionTarget>(config.target);
  removeExpression(config.type, target, compositor);

  size_t prevCount = expressions.size();
  addExpression(config);

  if (compositor) {
    for (size_t i = prevCount; i < expressions.size(); i++) {
      compositor->addBehavior(expressions[i].expression.get());
    }
  }
}

void ExpressionManager::removeExpression(const std::string& type, ExpressionTarget target, Compositor* compositor) {
  for (auto it = expressions.begin(); it != expressions.end();) {
    if (it->type == type && it->expression && it->expression->getTarget() == target) {
      if (compositor) compositor->removeBehavior(it->expression.get());
      it = expressions.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace lamp