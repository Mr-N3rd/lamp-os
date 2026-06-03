#pragma once

// BrightnessOverride — Phase C transient brightness override. v1 has a
// single global instance (surface = Any maps to "the lamp's master
// brightness"); the surface byte is preserved on the wire for forward
// compat with future per-strip brightness control.
//
// Change-driven callback model: tick() interpolates from→to over
// fadeDurationMs and only calls the registered onChange callback when
// the integer-rounded value actually changes. This preserves the
// existing event-driven brightness path (standard_lamp.cpp's
// applyEffectiveBrightness) — we don't push a setBrightness() into the
// strip every frame, only when the user-visible level moves.
//
// Source ownership identical to ColorOverride: a restore from a
// different sourceKind (and not Any) is a no-op.

#include <cstdint>
#include <functional>

#include "color_override.hpp"  // FadeState enum
#include "components/network/lamp_protocol.hpp"

namespace lamp {

class BrightnessOverride {
 public:
  // Apply a brightness override. `brightness` is 0..100 (already
  // validated by the protocol parser against kBrightnessOverrideMin..100
  // — defense-in-depth: the surface kind is the only forward-compat knob;
  // we still walk through the same change-driven callback so the strip
  // sees a clean transition).
  void apply(const uint8_t sourceMac[6],
             lamp_protocol::OverrideSource source,
             lamp_protocol::OverrideSurface surface,
             uint8_t brightness, uint16_t fadeDurationMs);

  // Restore — fade back to the baseline supplied at restore time
  // (effective() takes the current baseline as an arg so the caller
  // can pass whatever home-mode-aware value applies right now). Drops
  // silently on sourceKind mismatch.
  void restore(const uint8_t sourceMac[6],
               lamp_protocol::OverrideSource source,
               uint16_t fadeDurationMs);

  // Drives the state machine + the change-driven callback. Cheap when
  // Idle. nowMs is millis(); baseline is the caller's current baseline
  // (calling site reads effectiveBrightness() each frame so home mode
  // / lamp mode transitions are honored even during a fade).
  void tick(uint32_t nowMs, uint8_t baseline);

  // The interpolated value at nowMs (when active) or `baseline` (when
  // idle). Read-only; doesn't mutate state. Used by the caller's
  // effective-brightness path so a single source-of-truth value flows
  // into the NeoPixel setBrightness call.
  uint8_t effective(uint32_t nowMs, uint8_t baseline) const;

  // Wire the on-change handler. Called by tick() ONLY when the
  // integer-rounded effective value differs from the last reported
  // value — so a long-duration fade only generates ~N callbacks where
  // N is the perceived brightness step count, not one per frame.
  void setOnChangeCallback(std::function<void()> cb) { onChange_ = std::move(cb); }

  bool isActive() const { return state_ != FadeState::Idle; }
  FadeState state() const { return state_; }
  lamp_protocol::OverrideSource activeSource() const { return activeSource_; }

  // Was an override-MSG-WISP-HELLO seen recently from `mac`? Used by
  // the show_receiver brightness-floor check to allow Wisp-paired
  // sources to ignore the kBrightnessOverrideMin floor (e.g. during
  // a Lobby mode where the wisp wants full-dark hold). The window
  // matches the watchdog so a silent wisp loses its pairing too.
  static constexpr uint32_t kPaintWatchdogMs = 60000;

 private:
  FadeState state_ = FadeState::Idle;

  // Endpoints + window. fromBrightness_ is the value at fade start;
  // toBrightness_ is the override target. Stored unconditionally so
  // effective() can compute without branching on state.
  uint8_t fromBrightness_ = 0;
  uint8_t toBrightness_ = 0;
  // First-tick latch — at cold apply() we don't know the current baseline
  // (apply takes no baseline arg; the caller's tick supplies it). The
  // first tick() under FadingIn seeds fromBrightness_ = baseline so the
  // fade actually starts where the user was, instead of snapping to the
  // override value. After the first seed this stays true until the next
  // apply() (re-apply on a held override snapshots toBrightness_ as the
  // new from, no lazy seed needed).
  bool fromSeeded_ = false;
  uint8_t lastReportedBrightness_ = 0;  // change-detection latch

  uint32_t fadeStartMs_ = 0;
  uint16_t fadeDurationMs_ = 0;

  // Tracks the most recent apply() — drives the watchdog auto-restore.
  uint32_t lastApplyMs_ = 0;
  uint16_t currentFadeDurationMs_ = 0;

  // Restoring-state timing — mirrors the color override.
  uint32_t restoreStartMs_ = 0;
  uint16_t restoreDurationMs_ = 0;

  lamp_protocol::OverrideSource activeSource_ = lamp_protocol::OverrideSource::None;
  uint8_t activeMac_[6] = {0};
  lamp_protocol::OverrideSurface activeSurface_ = lamp_protocol::OverrideSurface::Any;

  std::function<void()> onChange_;
};

}  // namespace lamp
