#ifndef LAMP_EXPRESSIONS_EXPRESSION_H
#define LAMP_EXPRESSIONS_EXPRESSION_H

#include <cstdint>
#include <map>
#include <random>
#include <variant>
#include <vector>

#include "../core/animated_behavior.hpp"
#include "../util/color.hpp"

namespace lamp {

// Forward declaration
class Compositor;

// Set global compositor for expressions to check exclusive state
void setGlobalCompositor(Compositor* compositor);

enum ExpressionTarget {
  TARGET_SHADE = 1,
  TARGET_BASE = 2,
  TARGET_BOTH = 3
};

/**
 * @brief Base class for lamp expressions - behaviors that add personality
 * Expressions are time-triggered behaviors that modify the lamp's appearance
 */
class Expression : public AnimatedBehavior {
 protected:
  std::vector<Color> savedBuffer;
  std::vector<Color> colors;
  uint32_t nextTriggerMs = 0;
  uint32_t intervalMinMs = 60000;   // 1 min default
  uint32_t intervalMaxMs = 900000;  // 15 min default
  uint32_t lastCompletedLoop = 0;   // Track last completed animation loop
  ExpressionTarget target = TARGET_BOTH;
  std::mt19937 rng{esp_random()};

  /**
   * @brief Schedule next trigger within configured interval range
   */
  void scheduleNextTrigger();

  /**
   * @brief Save current buffer state for restoration
   */
  void saveBufferState();

  /**
   * @brief Check if this expression should affect current buffer
   */
  bool shouldAffectBuffer();

  /**
   * @brief Check if this expression should pause for an exclusive behavior
   */
  bool shouldPause() const;

 public:
  using AnimatedBehavior::AnimatedBehavior;

  virtual ~Expression() = default;

  /**
   * @brief Configure expression parameters (initial setup)
   * @param inColors Color palette for the expression
   * @param inIntervalMin Minimum trigger interval in seconds
   * @param inIntervalMax Maximum trigger interval in seconds
   * @param inTarget Which lamp component to affect
   */
  void configure(const std::vector<Color>& inColors,
                 uint32_t inIntervalMin,
                 uint32_t inIntervalMax,
                 ExpressionTarget inTarget);

  void control() override;

  /**
   * @brief Manually trigger this expression to start immediately
   * Can be called from UI or other expressions
   */
  void trigger();

  /**
   * @brief Get random color from configured palette
   */
  Color getRandomColor();

  const std::vector<Color>& getColors() const { return colors; }
  ExpressionTarget getTarget() const { return target; }

  /**
   * @brief Swap the palette without touching interval / target / schedule.
   *        Used by ExpressionManager::triggerInvocation to apply a remote
   *        invocation's colors for a single firing (then restore). Distinct
   *        from configure() which is intended for boot/upsert and resets the
   *        auto-trigger clock.
   *
   *        SCOPE: the manager's snapshot-set-trigger-restore pattern is
   *        fully correct for expression types that capture any color they
   *        need into a private member inside onTrigger() (e.g. Glitchy,
   *        which assigns glitchColor = getRandomColor() before returning).
   *        For continuous expressions that read `colors` in onUpdate()
   *        on later frames (Pulse, Breathing, Shifty), the restore happens
   *        before onUpdate runs and the override is silently dropped —
   *        those peers will animate with their own configured palette.
   *        That's acceptable for slice 1 (cascade UI is Glitchy-only). When
   *        cascade is exposed for continuous types, introduce a transient
   *        colorsOverride_ member consumed by subclasses' onUpdate paths.
   */
  void setColors(const std::vector<Color>& inColors) { colors = inColors; }

  // Suppresses auto-trigger from control() while true. Manual trigger() and
  // chain-triggered firing still work. Listing's enabled toggle drives this.
  bool autoTriggerEnabled = true;

protected:
  /**
   * @brief Expression-specific setup when triggered (REQUIRED)
   * Called when expression starts (both manual and automatic triggers)
   * Implement this to set up colors, state, etc.
   */
  virtual void onTrigger() = 0;

  /**
   * @brief Per-frame update during animation (OPTIONAL)
   * Called every frame while animationState == PLAYING
   * Implement this for continuous effects like moving waves
   */
  virtual void onUpdate() { }

  /**
   * @brief Cleanup when animation completes (OPTIONAL)
   * Called when animation finishes
   * Implement this for state cleanup or chaining effects
   */
  virtual void onComplete() { }
};

}  // namespace lamp

#endif