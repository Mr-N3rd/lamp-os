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
