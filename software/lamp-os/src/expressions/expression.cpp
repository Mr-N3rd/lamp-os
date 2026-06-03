#include "./expression.hpp"

#include <Arduino.h>

#include "../core/behavior_context.hpp"
#include "../core/compositor.hpp"
#include "./expression_manager.hpp"

namespace lamp {

void Expression::configure(const std::vector<Color>& inColors,
                          uint32_t inIntervalMin,
                          uint32_t inIntervalMax,
                          ExpressionTarget inTarget) {
  colors = inColors;
  intervalMinMs = inIntervalMin * 1000;
  intervalMaxMs = inIntervalMax * 1000;
  target = inTarget;
  scheduleNextTrigger();
}

void Expression::scheduleNextTrigger() {
  std::uniform_int_distribution<uint32_t> dist(intervalMinMs, intervalMaxMs);
  nextTriggerMs = millis() + dist(rng);
}

void Expression::saveBufferState() {
  savedBuffer = fb->buffer;
}

bool Expression::shouldAffectBuffer() {
  // Context is wired by Compositor::addBehavior at register time (or by
  // ExpressionManager::setCompositor for transients). Until both are wired
  // and ExpressionManager::begin() has published the buffer list, treat as
  // not-yet-routable and skip — same behavior as the old empty-vector guard.
  if (!context_ || context_->expressionFrameBuffers.size() < 2) return false;

  // Check if current buffer matches our target
  bool isShade = (fb == context_->expressionFrameBuffers[0]);  // Shade is first
  bool isBase = (fb == context_->expressionFrameBuffers[1]);   // Base is second

  switch (target) {
    case TARGET_SHADE:
      return isShade;
    case TARGET_BASE:
      return isBase;
    case TARGET_BOTH:
      return true;
    default:
      return false;
  }
}

void Expression::control() {
  // Pause if an exclusive behavior is running (unless we are exclusive)
  if (shouldPause()) return;

  // Check for automatic trigger
  if (autoTriggerEnabled && animationState == STOPPED && millis() > nextTriggerMs) {
    trigger();
  }

  // Per-frame updates during animation
  if (animationState == PLAYING || animationState == PLAYING_ONCE) {
    onUpdate();
  }

  // Handle completion - check if we just stopped
  if (animationState == STOPPED && currentLoop > lastCompletedLoop) {
    onComplete();
    lastCompletedLoop = currentLoop;
  }
}

bool Expression::shouldPause() const {
  // Don't pause if this expression is exclusive
  if (isExclusive) return false;

  // Check if compositor has an active exclusive. Context is null until the
  // Compositor registers us via addBehavior — pre-registration we can't be
  // running anyway, so treat as "no exclusive".
  return context_ && context_->compositor && context_->compositor->hasActiveExclusive();
}

Color Expression::getRandomColor() {
  if (colors.empty()) {
    return Color(0, 0, 0, 0);
  }
  std::uniform_int_distribution<size_t> dist(0, colors.size() - 1);
  return colors[dist(rng)];
}

void Expression::trigger() {
  // Only trigger if this expression should affect this buffer
  // This ensures expressions respect their target configuration
  if (!shouldAffectBuffer()) {
    return;
  }

  // Start immediately
  onTrigger();            // Expression-specific setup
  scheduleNextTrigger();  // Reset next automatic trigger
  playOnce();

  // Notify the manager so the cascade convention fires for ALL trigger paths,
  // including the per-entry auto-trigger from control() (which previously
  // bypassed maybeCascade entirely). triggerExpression/triggerInvocation set
  // a suppress flag on the manager around their own loops so this callback
  // doesn't double-cascade (or re-cascade for remote-arrived invocations).
  if (context_ && context_->expressionManager) {
    context_->expressionManager->onExpressionFired(this);
  }
}

}  // namespace lamp