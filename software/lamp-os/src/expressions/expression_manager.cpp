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


// Factory: build an Expression subclass instance for `type` bound to `buffer`,
// configured with the given palette / interval range / target / params.
// Returns nullptr for unknown types. Shared by addExpression (configured
// entries) and triggerInvocation (transient one-shots from remote cascades) —
// neither path needs to know the per-subclass constructor arguments.
static std::unique_ptr<Expression> makeExpression(
    const std::string& type, FrameBuffer* buffer,
    const std::vector<Color>& colors,
    uint32_t intervalMin, uint32_t intervalMax,
    ExpressionTarget target,
    const std::map<std::string, uint32_t>& parameters) {
  std::unique_ptr<Expression> expr;
  if (type == "glitchy") {
    auto e = std::make_unique<GlitchyExpression>(buffer, 3);
    e->configure(colors, intervalMin, intervalMax, target);
    e->configureFromParameters(parameters);
    expr = std::move(e);
  } else if (type == "shifty") {
    auto e = std::make_unique<ShiftyExpression>(buffer, 120);
    e->configure(colors, intervalMin, intervalMax, target);
    e->configureFromParameters(parameters);
    expr = std::move(e);
  } else if (type == "pulse") {
    auto e = std::make_unique<PulseExpression>(buffer, 60);
    e->configure(colors, intervalMin, intervalMax, target);
    e->configureFromParameters(parameters);
    expr = std::move(e);
  } else if (type == "breathing") {
    auto e = std::make_unique<BreathingExpression>(buffer, 60);
    e->configure(colors, intervalMin, intervalMax, target);
    e->configureFromParameters(parameters);
    expr = std::move(e);
  }
  return expr;
}

void ExpressionManager::addExpression(const ExpressionConfig& config) {
  if (!shadeBuffer || !baseBuffer) return;

  auto target = static_cast<ExpressionTarget>(config.target);

  // Determine target buffers
  std::vector<FrameBuffer*> targetBuffers;
  if (target == TARGET_BOTH) {
    targetBuffers = {shadeBuffer, baseBuffer};
  } else {
    targetBuffers = {(target == TARGET_SHADE) ? shadeBuffer : baseBuffer};
  }

  // Create expressions for each target buffer
  for (auto* buffer : targetBuffers) {
    if (auto expr = makeExpression(config.type, buffer, config.colors,
                                   config.intervalMin, config.intervalMax,
                                   target, config.parameters)) {
      expr->autoTriggerEnabled = config.enabled;
      expressions.push_back({std::move(expr), config.type, config});
    }
  }
}

void ExpressionManager::setShowReceiver(ShowReceiver* receiver) {
  showReceiver_ = receiver;
}

void ExpressionManager::setCompositor(Compositor* compositor) {
  compositor_ = compositor;
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
  // Suppress per-entry cascade callbacks from Expression::trigger() — we
  // batch a single cascade for the logical trigger after the loop.
  suppressCascade_ = true;
  for (auto& entry : expressions) {
    if (entry.type == type && entry.expression) {
      entry.expression->trigger();
      triggered = true;
      if (!firstFired) firstFired = &entry;
    }
  }
  suppressCascade_ = false;
  // Cascade once per logical trigger, not once per entry — a TARGET_BOTH
  // expression has two entries (shade + base) but should fan out a single
  // invocation that receivers' own managers expand back to both sides.
  if (firstFired) maybeCascade(*firstFired);
  return triggered;
}

bool ExpressionManager::triggerExpression(const std::string& type, ExpressionTarget target) {
  bool triggered = false;
  const ExpressionEntry* firstFired = nullptr;
  suppressCascade_ = true;
  for (auto& entry : expressions) {
    if (entry.type == type && entry.expression && entry.expression->getTarget() == target) {
      entry.expression->trigger();
      triggered = true;
      if (!firstFired) firstFired = &entry;
    }
  }
  suppressCascade_ = false;
  if (firstFired) maybeCascade(*firstFired);
  return triggered;
}

void ExpressionManager::onExpressionFired(Expression* e) {
  if (suppressCascade_ || !e) return;
  for (auto& entry : expressions) {
    if (entry.expression.get() == e) {
      maybeCascade(entry);
      return;
    }
  }
}

bool ExpressionManager::triggerInvocation(const ExpressionInvocation& inv) {
  if (!shadeBuffer || !baseBuffer) return false;

  ExpressionTarget invTarget = static_cast<ExpressionTarget>(inv.target);

  // Determine target buffers — same convention as addExpression. TARGET_BOTH
  // fires on both halves of the lamp; specific target fires on one.
  std::vector<FrameBuffer*> targetBuffers;
  if (invTarget == TARGET_BOTH) {
    targetBuffers = {shadeBuffer, baseBuffer};
  } else if (invTarget == TARGET_SHADE) {
    targetBuffers = {shadeBuffer};
  } else if (invTarget == TARGET_BASE) {
    targetBuffers = {baseBuffer};
  } else {
    return false;
  }

  // Loop-break invariant: the transient's trigger() will call
  // onExpressionFired via the global manager pointer; suppress so a
  // remote-received trigger can never re-cascade. Receivers are terminal in
  // the propagation graph.
  suppressCascade_ = true;

  bool triggered = false;
  for (auto* buffer : targetBuffers) {
    // Build a fresh one-shot Expression instance directly from the
    // invocation. NEVER consults this lamp's `expressions` (configured)
    // vector — the receiver's local config is intentionally irrelevant.
    // The cascade is a self-contained "execute this expression once and
    // forget it" command; the receiver's own configured expressions remain
    // entirely independent (untouched, unread, unmodified).
    auto expr = makeExpression(inv.type, buffer, inv.colors,
                               /*intervalMin*/ 60, /*intervalMax*/ 900,
                               invTarget, inv.parameters);
    if (!expr) continue;  // unknown type
    expr->autoTriggerEnabled = false;  // pure one-shot — never re-fires itself

    Expression* raw = expr.get();
    if (compositor_) compositor_->addBehavior(raw);
    transientExpressions_.push_back(std::move(expr));
    raw->trigger();
    triggered = true;
  }

  suppressCascade_ = false;
  return triggered;
}

void ExpressionManager::gcTransients() {
  if (transientExpressions_.empty()) return;
  for (auto it = transientExpressions_.begin(); it != transientExpressions_.end();) {
    if ((*it)->isAnimationComplete()) {
      if (compositor_) compositor_->removeBehavior(it->get());
      it = transientExpressions_.erase(it);
    } else {
      ++it;
    }
  }
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