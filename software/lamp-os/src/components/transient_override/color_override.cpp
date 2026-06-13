#include "color_override.hpp"

#include <cstring>

#include "behaviors/configurator.hpp"
#include "core/behavior_context.hpp"
#include "core/frame_buffer.hpp"
#include "util/gradient.hpp"

namespace lamp {

void ColorOverride::bind(BehaviorContext& ctx, lamp_protocol::OverrideSurface surface) {
  surface_ = surface;
  switch (surface) {
    case lamp_protocol::OverrideSurface::Base:
      configurator_ = ctx.baseConfigurator;
      break;
    case lamp_protocol::OverrideSurface::Shade:
      configurator_ = ctx.shadeConfigurator;
      break;
    case lamp_protocol::OverrideSurface::Any:
      // V1: caller binds two instances (one per surface). Any is reserved
      // for a future combined-control variant; leave configurator_ null.
      configurator_ = nullptr;
      break;
  }
  if (configurator_ && configurator_->fb) {
    pixelCount_ = configurator_->fb->pixelCount;
  }
}

void ColorOverride::apply(const uint8_t sourceMac[6],
                          lamp_protocol::OverrideSource source,
                          const Color* colors, uint8_t numColors,
                          uint16_t fadeDurationMs) {
  if (!configurator_ || numColors == 0 || !colors) return;
  // Operator-priority lockout: while the app has the colour picker /
  // brightness slider for this surface open, wisp paints lose. Social
  // cascade (PeerSwap) still applies — greetings outrank quiet edits.
  if (operatorEditing_ &&
      source == lamp_protocol::OverrideSource::Wisp) {
#ifdef LAMP_DEBUG
    // Diagnostic for "lamp stopped listening to wisp" — fires when the
    // operatorEditing flag is stuck true (e.g. ungraceful BLE
    // disconnect skipped the defensive sweep in ControlServerCallbacks::
    // onDisconnect at ble_control.cpp:376-378). If you see this line
    // while no app is connected, the flag is stranded.
    Serial.printf("[override] DROP wisp surface=0x%02X (operatorEditing=true)\n",
                  (unsigned)surface_);
#endif
    return;
  }
  // Source-ownership: a Holding override from a different source can be
  // overtaken — newer apply() wins (matches the UX: a wisp painting room A
  // gets superseded the moment a peer-swap from a closer source arrives).
  // We DON'T re-snapshot savedColors_ in that case because savedColors_
  // already holds the true pre-any-override baseline. Snapshot only when
  // transitioning from Idle into FadingIn.
  if (state_ == FadeState::Idle) {
    savedColors_ = configurator_->colors;
  }

  // Refresh pixel count from the configurator's frame buffer in case
  // configuration changed (rare; only at boot or factory reset).
  if (configurator_->fb) pixelCount_ = configurator_->fb->pixelCount;

  // Expand the stops into a per-pixel gradient via the existing helper.
  std::vector<Color> stops(colors, colors + numColors);
  std::vector<Color> target =
      lamp::buildGradientWithStops(pixelCount_, stops);

  configurator_->beginFade(target, fadeDurationMs);
  // Keep the configurator's animation state machine alive. Without this,
  // ConfiguratorBehavior::control() (behaviors/configurator.cpp:77-89)
  // lapses to STOPPED after CONFIGURATOR_WEBSOCKET_TIMEOUT_MS=60s of no
  // BLE writes — the legacy WebSocket-era inactivity gate. While
  // STOPPED, the Compositor skips its draw() (core/compositor.cpp:62),
  // so the wisp gradient we just stored in `colors` never reaches the
  // LED buffer even though apply() is firing every ~5-10s. Wisp paint
  // cadence (well inside 60s) keeps the configurator perpetually awake
  // through this bump; the existing 6 BLE-write bump sites in
  // standard_lamp.cpp keep the operator-edit path covered.
  configurator_->lastWebSocketUpdateTimeMs = millis();

  state_ = FadeState::FadingIn;
  activeSource_ = source;
  std::memcpy(activeMac_, sourceMac, 6);
  lastApplyMs_ = configurator_->fadeStartMs();
  currentFadeDurationMs_ = fadeDurationMs;
}

void ColorOverride::restore(const uint8_t sourceMac[6],
                            lamp_protocol::OverrideSource source,
                            uint16_t fadeDurationMs) {
  (void)sourceMac;  // not used for the dedup decision in v1 — see source
                    // ownership rules below.
  if (!configurator_ || state_ == FadeState::Idle) return;

  // Source-ownership check: only the active source (or Any) can restore.
  // PeerSwap restore on a Wisp-owned override is dropped; otherwise a wisp
  // going noisy with restores would cancel a peer-swap mid-cascade.
  if (source != lamp_protocol::OverrideSource::Any &&
      source != activeSource_) {
    return;
  }

  // Snap target back to the saved baseline via the configurator's fade
  // machinery so this restore visually behaves identically to a fresh
  // apply (same per-pixel interp curve, same mid-fade interrupt support).
  configurator_->beginFade(savedColors_, fadeDurationMs);
  // See apply() above — keep the configurator drawing through the
  // restore fade-back too, otherwise the watchdog-driven restore would
  // hand off to a STOPPED configurator and the fade would be invisible.
  configurator_->lastWebSocketUpdateTimeMs = millis();
  state_ = FadeState::Restoring;
  restoreStartMs_ = configurator_->fadeStartMs();
  restoreDurationMs_ = fadeDurationMs;
}

void ColorOverride::tick(uint32_t nowMs) {
  switch (state_) {
    case FadeState::Idle:
      return;
    case FadeState::FadingIn: {
      // Once the fade-in window has elapsed, transition to Holding.
      // currentFadeDurationMs_ == 0 (instant snap) lands here on the
      // very next tick too.
      const uint32_t elapsed = nowMs - lastApplyMs_;
      if (elapsed >= currentFadeDurationMs_) {
        state_ = FadeState::Holding;
      }
      return;
    }
    case FadeState::Holding: {
      // Watchdog: if the source hasn't re-applied within the window,
      // auto-restore so a silent wisp can't keep us painted forever.
      // Use Any as the synthetic source so the source-ownership check
      // in restore() doesn't bounce us.
      const uint32_t elapsed = nowMs - lastApplyMs_;
      if (elapsed >= kPaintWatchdogMs) {
        restore(activeMac_, lamp_protocol::OverrideSource::Any,
                /*fadeDurationMs=*/currentFadeDurationMs_);
      }
      return;
    }
    case FadeState::Restoring: {
      // Settle to Idle once the restore fade window has elapsed.
      const uint32_t elapsed = nowMs - restoreStartMs_;
      if (elapsed >= restoreDurationMs_) {
        state_ = FadeState::Idle;
        activeSource_ = lamp_protocol::OverrideSource::None;
        std::memset(activeMac_, 0, 6);
      }
      return;
    }
  }
}

void ColorOverride::rebaseline(const std::vector<Color>& currentSavedColors) {
  // BLE write landed mid-Holding (user changed the underlying config
  // colors). Replace the baseline so the next restore lands on the new
  // colors. No-op when Idle — the BLE write went straight to the
  // configurator and there's no override to fix up.
  if (state_ == FadeState::Idle) return;
  savedColors_ = currentSavedColors;
}

}  // namespace lamp
