#include "./expression_manager.hpp"
#include <Arduino.h>
#include <algorithm>

#include "../components/network/show_receiver.hpp"
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
      expressions.push_back({std::move(expr), config.type, config});
    }
  }
}

void ExpressionManager::setShowReceiver(ShowReceiver* receiver) {
  showReceiver_ = receiver;
}

void ExpressionManager::maybeCascade(const ExpressionEntry& entry) {
  if (!showReceiver_ || !entry.expression) return;
  if (entry.config.getParameter(kParamCascadeEnabled, 0) == 0) return;
  const uint32_t staggerMs = entry.config.getParameter(kParamCascadeStaggerMs, 0);

  ExpressionInvocation inv;
  inv.type = entry.config.type;
  inv.colors = entry.expression->getColors();
  inv.target = static_cast<uint8_t>(entry.expression->getTarget());
  inv.parameters = parametersWithoutCascadeKeys(entry.config.parameters);
  // inv.delayMs defaults to 0; sendExpressionToAll assigns per-peer stagger.

  showReceiver_->sendExpressionToAll(inv, staggerMs);
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
  const ExpressionEntry* firstFired = nullptr;
  for (auto& entry : expressions) {
    if (entry.type == type && entry.expression) {
      entry.expression->trigger();
      triggered = true;
      if (!firstFired) firstFired = &entry;
    }
  }
  // Cascade once per logical trigger, not once per entry — a TARGET_BOTH
  // expression has two entries (shade + base) but should fan out a single
  // invocation that receivers' own managers expand back to both sides.
  if (firstFired) maybeCascade(*firstFired);
  return triggered;
}

bool ExpressionManager::triggerExpression(const std::string& type, ExpressionTarget target) {
  bool triggered = false;
  const ExpressionEntry* firstFired = nullptr;
  for (auto& entry : expressions) {
    if (entry.type == type && entry.expression && entry.expression->getTarget() == target) {
      entry.expression->trigger();
      triggered = true;
      if (!firstFired) firstFired = &entry;
    }
  }
  if (firstFired) maybeCascade(*firstFired);
  return triggered;
}

bool ExpressionManager::triggerInvocation(const ExpressionInvocation& inv) {
  ExpressionTarget invTarget = static_cast<ExpressionTarget>(inv.target);
  bool triggered = false;
  for (auto& entry : expressions) {
    if (entry.type != inv.type || !entry.expression) continue;
    // TARGET_BOTH invocations fire any entry of this type; specific target
    // fires only entries with that exact target. Intentional double-fire:
    // a TARGET_BOTH expression has TWO entries on this lamp (shade buffer +
    // base buffer, created by addExpression's targetBuffers loop), and both
    // halves of the lamp should animate together.
    if (invTarget != TARGET_BOTH && entry.expression->getTarget() != invTarget) {
      continue;
    }
    // If the invocation supplies colors, swap the palette ONLY for this
    // firing — restore immediately after trigger() so the receiver's local
    // config isn't permanently mutated and subsequent local triggers use the
    // user's configured palette. Safe for one-shot expressions whose
    // onTrigger captures the color (Glitchy). For continuous expressions
    // that read `colors` in onUpdate (Pulse/Breathing/Shifty), the restore
    // wins and the override is silently dropped — see the SCOPE note on
    // Expression::setColors. Acceptable for slice 1 (Glitchy-only cascade).
    if (!inv.colors.empty()) {
      const auto savedColors = entry.expression->getColors();
      entry.expression->setColors(inv.colors);
      entry.expression->trigger();
      entry.expression->setColors(savedColors);
    } else {
      entry.expression->trigger();
    }
    triggered = true;
    // Loop-break invariant: NEVER cascade on a remote-triggered invocation.
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