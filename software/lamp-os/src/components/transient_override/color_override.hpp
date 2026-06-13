#pragma once

// ColorOverride — Phase C transient color override owned per-surface
// (base / shade). The wisp module (C.4) and the future peer-swap social
// cascade both drive a lamp's surface through this primitive:
//
//   apply()   — paint an override color set, fade-in over fadeDurationMs.
//               Marks state FadingIn → Holding.
//   restore() — fade back to the saved baseline. Marks state Restoring →
//               Idle.
//   tick()    — drives the state machine. Watchdog auto-restores after
//               kPaintWatchdogMs (60s) of no apply() refresh, so a wisp
//               that goes silent doesn't leave a lamp painted forever.
//
// Per-pixel fade lives in ConfiguratorBehavior — apply() builds the
// gradient + calls configurator->beginFade(targetColors, fadeDurationMs).
// The configurator's own draw loop runs the interpolation. Single source
// of truth for fade math; ColorOverride never touches the buffer.
//
// Source ownership: each (apply, restore) carries an OverrideSource
// (Wisp, PeerSwap, ...). A restore() with a non-matching sourceKind is a
// no-op so a Wisp can't accidentally cancel a PeerSwap and vice versa.
// sourceKind == Any always succeeds (admin / shutdown path).

#include <cstdint>
#include <vector>

#include "components/network/lamp_protocol.hpp"
#include "util/color.hpp"

namespace lamp {

// Forward declarations to keep this header light.
class ConfiguratorBehavior;
struct BehaviorContext;

enum class FadeState : uint8_t {
  Idle = 0,       // No override active. apply() transitions to FadingIn.
  FadingIn = 1,   // Configurator is fading current → override target.
                  // Transitions to Holding when the fade window elapses.
  Holding = 2,    // Override fully applied; tick() watches the watchdog.
                  // Transitions to Restoring when restore() is called OR
                  // kPaintWatchdogMs (60s) elapses since the last apply().
  Restoring = 3,  // Configurator is fading override → savedColors_.
                  // Transitions to Idle when the restore fade completes.
};

class ColorOverride {
 public:
  // Wire the configurator pointer via the shared BehaviorContext. The
  // surface picks base vs shade; passing Surface::Any leaves both pointers
  // available (in v1 the caller binds a separate instance per surface,
  // so Any is reserved for a future combined-control variant).
  // pixelCount is read once from the bound configurator's frame buffer
  // at apply() time and cached so we don't re-resolve on every fade.
  void bind(BehaviorContext& ctx, lamp_protocol::OverrideSurface surface);

  // Apply an override. `colors` is the unexpanded list (1..N stops); we
  // expand to pixelCount via buildGradientWithStops before pushing into
  // the configurator. `fadeDurationMs == 0` means snap-in (configurator
  // writes target directly on the next frame). On entry: snapshots the
  // CURRENT buffer state (the live baseline) into savedColors_ so a
  // later restore lands on whatever the user had configured, even if it
  // was a prior BLE write that hasn't been persisted yet.
  void apply(const uint8_t sourceMac[6],
             lamp_protocol::OverrideSource source,
             const Color* colors, uint8_t numColors,
             uint16_t fadeDurationMs);

  // Restore to the saved baseline. Drops silently if the call doesn't
  // own this override (sourceKind mismatch and not Any). When state is
  // Idle this is a no-op — restore-without-prior-apply is benign.
  void restore(const uint8_t sourceMac[6],
               lamp_protocol::OverrideSource source,
               uint16_t fadeDurationMs);

  // Drives the state machine: FadingIn→Holding, Holding→Restoring
  // (watchdog), Restoring→Idle. Cheap when state == Idle. Call from the
  // loop task each iteration.
  void tick(uint32_t nowMs);

  // Update the "what to restore to" baseline mid-Holding. Called from the
  // BLE color drain after a user-driven write so a subsequent restore
  // lands on the new color set instead of the pre-override values. No-op
  // when state == Idle (the BLE write went straight into the configurator,
  // no override to update).
  void rebaseline(const std::vector<Color>& currentSavedColors);

  // Cross-touch the watchdog without running a fade. Wisp paint is sent as
  // a Base+Shade pair 10 ms apart but lands in a single-slot mailbox
  // (PendingTypedSlot, newest writer wins). If Core 1 doesn't drain
  // between the two posts the Shade frame silently drops the Base frame
  // and Base's lastApplyMs_ never advances — after 60 s the watchdog
  // auto-restores Base, expressions un-pause, and the next surviving
  // wisp Base frame snapshots the expression-painted buffer as the new
  // savedColors_, leaving the lamp visibly "stopped listening" to wisp.
  // Cross-touch from the Shade-side drain (and vice versa) is proof of
  // a healthy mesh and keeps both surfaces' watchdogs honest.
  void touchApply(uint32_t nowMs) {
    if (state_ == FadeState::FadingIn || state_ == FadeState::Holding) {
      lastApplyMs_ = nowMs;
    }
  }

  bool isActive() const { return state_ != FadeState::Idle; }
  FadeState state() const { return state_; }
  lamp_protocol::OverrideSource activeSource() const { return activeSource_; }

  // Operator-priority lockout. While set, apply() drops wisp-sourced
  // overrides on the floor (PeerSwap/social cascade still applies — a
  // greeting takes precedence over a quiet edit). Set on by the app
  // when the colour-picker / brightness-slider for this surface opens;
  // cleared when the picker closes. Bounded entirely by picker
  // lifecycle, no timer.
  void setOperatorEditing(bool editing) { operatorEditing_ = editing; }
  bool operatorEditing() const { return operatorEditing_; }

  // Auto-restore watchdog. If the wisp goes silent (or a peer-swap source
  // crashes) the override transitions out so the lamp can't be painted
  // forever. 60s matches the wisp's expected refresh cadence with margin.
  static constexpr uint32_t kPaintWatchdogMs = 60000;

 private:
  ConfiguratorBehavior* configurator_ = nullptr;
  lamp_protocol::OverrideSurface surface_ = lamp_protocol::OverrideSurface::Any;
  uint8_t pixelCount_ = 0;

  FadeState state_ = FadeState::Idle;
  lamp_protocol::OverrideSource activeSource_ = lamp_protocol::OverrideSource::None;
  uint8_t activeMac_[6] = {0};

  // Timestamp of the last apply() — drives both the FadingIn→Holding
  // transition (when elapsed >= currentFadeDurationMs_) and the
  // Holding→Restoring watchdog (when elapsed >= kPaintWatchdogMs).
  uint32_t lastApplyMs_ = 0;
  uint16_t currentFadeDurationMs_ = 0;

  // Timestamp of the restore() — drives the Restoring→Idle transition.
  uint32_t restoreStartMs_ = 0;
  uint16_t restoreDurationMs_ = 0;

  // The baseline we'll restore to. Snapshotted from the configurator's
  // `colors` at apply() time, and replaced by rebaseline() during Holding.
  std::vector<Color> savedColors_;

  // Operator-editing lock — see setOperatorEditing() above.
  bool operatorEditing_ = false;
};

}  // namespace lamp
