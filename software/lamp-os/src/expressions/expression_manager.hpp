#ifndef LAMP_EXPRESSIONS_MANAGER_H
#define LAMP_EXPRESSIONS_MANAGER_H

#include <memory>
#include <vector>

#include "../config/config_types.hpp"
#include "../core/frame_buffer.hpp"
#include "./expression.hpp"
#include "./expression_invocation.hpp"
#include "./glitchy_expression.hpp"
#include "./shifty_expression.hpp"
#include "./pulse_expression.hpp"
#include "./breathing_expression.hpp"

namespace lamp {

class Compositor;
class ExpressionManager;
class ShowReceiver;

// Global expression manager access
void setGlobalExpressionManager(ExpressionManager* manager);
ExpressionManager* getGlobalExpressionManager();

/**
 * @brief Manages active expressions and their lifecycle
 */
class ExpressionManager {
 private:
  // Store expression with its type for triggering. `config` is a snapshot
  // of the ExpressionConfig that built this entry — kept so the manager can
  // make cascade decisions without each Expression subclass having to
  // expose its raw parameter map.
  struct ExpressionEntry {
    std::unique_ptr<Expression> expression;
    std::string type;
    ExpressionConfig config;
  };
  std::vector<ExpressionEntry> expressions;
  FrameBuffer* shadeBuffer = nullptr;
  FrameBuffer* baseBuffer = nullptr;
  ShowReceiver* showReceiver_ = nullptr;
  Compositor* compositor_ = nullptr;
  // Set during the manager's own trigger* loops so per-entry Expression::trigger()
  // callbacks don't fan out a cascade we're already handling explicitly (or
  // intentionally skipping for remote-arrived invocations). Loop-task only —
  // no concurrency.
  bool suppressCascade_ = false;

  // One-shot Expression instances created on-demand by triggerInvocation when
  // a remote cascade arrives. They live in the compositor for the duration of
  // their animation, then gcTransients() removes them. Entirely independent of
  // the `expressions` (configured) vector — no interaction with the receiver's
  // local config in any direction.
  std::vector<std::unique_ptr<Expression>> transientExpressions_;

  // Send the cascade fan-out for an expression that just fired locally,
  // if its config opts in via the cascadeEnabled parameter. No-op when
  // no ShowReceiver has been wired in. Never called for remote-arrived
  // triggers — that's the structural loop break.
  void maybeCascade(const ExpressionEntry& entry);

 public:
  /**
   * @brief Initialize manager with frame buffers
   */
  void begin(FrameBuffer* shade, FrameBuffer* base);

  /**
   * @brief Wire up the mesh send path for the cascade convention. Optional —
   *        when unset, cascade is silently disabled (boot before mesh ready,
   *        or test environments).
   */
  void setShowReceiver(ShowReceiver* receiver);

  /**
   * @brief Wire up the compositor so triggerInvocation can lazy-upsert a
   *        transient entry when a remote cascade arrives for an expression
   *        type the receiver doesn't have configured. Without this, remote
   *        triggers for unconfigured types are silently no-op'd.
   */
  void setCompositor(Compositor* compositor);

  /**
   * @brief Load expressions from config
   */
  void loadFromConfig(const ExpressionSettings& settings);

  /**
   * @brief Get active expression behaviors for compositor
   */
  std::vector<AnimatedBehavior*> getBehaviors();

  /**
   * @brief Add a new expression. Used at boot by loadFromConfig (compositor
   *        not yet built). Runtime callers should use upsertExpression.
   */
  void addExpression(const ExpressionConfig& config);

  /**
   * @brief Clear all expressions. Only safe before the compositor has been
   *        built — does not unregister behaviors from a running compositor.
   */
  void clear();

  /**
   * @brief Trigger every expression whose type matches. LOCAL path —
   *        honors the cascade convention if configured.
   */
  bool triggerExpression(const std::string& type);

  /**
   * @brief Trigger expressions matching both type and target. Used by the
   *        per-row Test button to fire exactly the configured instance.
   *        LOCAL path — honors the cascade convention if configured.
   */
  bool triggerExpression(const std::string& type, ExpressionTarget target);

  /**
   * @brief REMOTE path. Called only by the receive side of the mesh when a
   *        triggerExpression CONTROL_OP arrives. Overrides colors for this
   *        single firing (if the invocation provides any), then triggers
   *        the matching configured expression(s). NEVER cascades — this is
   *        the structural loop break that makes flood propagation safe.
   */
  bool triggerInvocation(const ExpressionInvocation& inv);

  /**
   * @brief Called by Expression::trigger() after onTrigger() runs, regardless
   *        of how trigger() was reached (auto-interval from control(),
   *        internal chain triggers, etc.). Looks up the entry by pointer and
   *        runs the cascade convention. Suppressed when the manager's own
   *        trigger* loops are running so they can batch cascade themselves.
   */
  void onExpressionFired(Expression* e);

  /**
   * @brief Garbage-collect transient one-shot expressions created by
   *        triggerInvocation whose animation has finished. Unregisters them
   *        from the compositor and destroys the instance. Cheap; safe to
   *        call every loop tick. Call AFTER compositor.tick() so the final
   *        frame of the animation gets drawn before removal.
   */
  void gcTransients();

  std::vector<Color> getExpressionColors(const std::string& type) const;

  /**
   * @brief Live insert-or-update keyed by (type, target). Destroys any
   *        existing entries for (type, target), builds fresh ones from
   *        config, and registers them with the compositor.
   */
  void upsertExpression(const ExpressionConfig& config, Compositor* compositor);

  /**
   * @brief Live remove keyed by (type, target). Unregisters from compositor
   *        first, then destroys the Expression instances.
   */
  void removeExpression(const std::string& type, ExpressionTarget target, Compositor* compositor);
};

}  // namespace lamp

#endif