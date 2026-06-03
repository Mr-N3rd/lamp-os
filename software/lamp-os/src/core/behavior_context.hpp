#ifndef LAMP_CORE_BEHAVIOR_CONTEXT_H
#define LAMP_CORE_BEHAVIOR_CONTEXT_H

#include <vector>

namespace lamp {

// Forward declarations — keep this header dependency-light so it can be
// included from animated_behavior.hpp without dragging in compositor/manager
// definitions.
class Compositor;
class ExpressionManager;
class FrameBuffer;

/**
 * @brief Per-behavior context wired by the Compositor at register time.
 *        Replaces the hidden singleton globals (globalCompositor,
 *        globalExpressionManager, expressionFrameBuffers) with an explicit
 *        pointer that the Compositor sets via AnimatedBehavior::setBehaviorContext
 *        the moment a behavior is registered.
 *
 *        The Compositor owns one instance; every behavior it registers points
 *        at that single instance. Mutating a field (e.g. ExpressionManager
 *        wiring up the frame buffer list after begin()) is observed by every
 *        behavior without re-pointing.
 */
struct BehaviorContext {
  Compositor* compositor = nullptr;
  ExpressionManager* expressionManager = nullptr;
  // Mirrors the previous `expressionFrameBuffers` extern: ExpressionManager
  // populates [shade, base] in begin() so Expression::shouldAffectBuffer()
  // can route by target without grabbing a global.
  std::vector<FrameBuffer*> expressionFrameBuffers;
};

}  // namespace lamp

#endif
