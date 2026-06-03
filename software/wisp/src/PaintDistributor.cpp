#include "PaintDistributor.h"

#include <Arduino.h>

#include <cstring>

#include "CurrentPalette.h"
#include "LampInventory.h"
#include "MeshLink.h"
#include "TupleSampler.h"
#include "lamp_protocol.hpp"

namespace wisp {

void PaintDistributor::begin(LampInventory* inventory, MeshLink* mesh,
                             CurrentPalette* palette) {
  inventory_ = inventory;
  mesh_ = mesh;
  palette_ = palette;
}

void PaintDistributor::setPaintMode(bool on) {
  if (on == paintMode_) {
    // Idempotent: paint:on while already on still kicks a fresh walk so
    // the user sees instant feedback if a lamp joined since last paint.
    if (on) beginWalk(Mode::Paint);
    return;
  }
  paintMode_ = on;
  if (on) {
    lastBackstopMs_ = millis();
    beginWalk(Mode::Paint);
  } else {
    // Walk every currently-known peer with a RESTORE so they fall back to
    // their authored personality. We deliberately keep paintMode_ false so
    // tick() doesn't re-trigger backstop refreshes.
    beginWalk(Mode::Restore);
  }
}

void PaintDistributor::onPaletteChanged() {
  if (!paintMode_) return;
  beginWalk(Mode::Paint);
}

void PaintDistributor::tick(uint32_t nowMs) {
  // Drain the paced queue first; backstop logic only fires when idle so we
  // don't double-up sends.
  if (walkMode_ != Mode::Idle && walkIdx_ < walkCount_) {
    if (nowMs - lastSendMs_ < kPerPeerPaceMs) return;
    lastSendMs_ = nowMs;
    const uint8_t* mac = walkMacs_[walkIdx_];
    if (walkMode_ == Mode::Paint) {
      sendPaintToPeer(mac);
    } else {
      sendRestoreToPeer(mac);
    }
    walkIdx_++;
    if (walkIdx_ >= walkCount_) walkMode_ = Mode::Idle;
    return;
  }

  // Backstop: every 10s while paintMode is on, kick a fresh walk in case a
  // lamp missed a frame or just joined the roster between palette changes.
  if (paintMode_ && (nowMs - lastBackstopMs_) >= kBackstopRefreshMs) {
    lastBackstopMs_ = nowMs;
    beginWalk(Mode::Paint);
  }
}

void PaintDistributor::beginWalk(Mode mode) {
  if (!inventory_) return;
  auto roster = inventory_->snapshot();
  walkCount_ = 0;
  for (const auto& e : roster) {
    if (walkCount_ >= kMaxWalkPeers) break;
    std::memcpy(walkMacs_[walkCount_], e.mac, 6);
    walkCount_++;
  }
  walkIdx_ = 0;
  walkMode_ = walkCount_ ? mode : Mode::Idle;
  // Force the first send through immediately (lastSendMs_ in the past).
  lastSendMs_ = millis() - kPerPeerPaceMs;
}

void PaintDistributor::sendPaintToPeer(const uint8_t mac[6]) {
  if (!mesh_ || !palette_) return;
  ColorTuple t = sampleTupleForMac(*palette_, mac);
  uint8_t colorsRGBW[8];
  colorsRGBW[0] = t.r[0]; colorsRGBW[1] = t.g[0]; colorsRGBW[2] = t.b[0]; colorsRGBW[3] = t.w[0];
  colorsRGBW[4] = t.r[1]; colorsRGBW[5] = t.g[1]; colorsRGBW[6] = t.b[1]; colorsRGBW[7] = t.w[1];

  uint8_t srcMac[6] = {0};
  mesh_->getMac(srcMac);

  uint8_t buf[lamp_protocol::OVERRIDE_COLORS_MAX_SIZE];
  size_t n = lamp_protocol::buildOverrideColors(
      buf, sizeof(buf), seqCounter_++,
      srcMac, mac,
      lamp_protocol::OverrideSurface::Base,
      lamp_protocol::OverrideSource::Wisp,
      kDefaultFadeDurationMs,
      colorsRGBW, /*numColors=*/2);
  if (!n) return;
  mesh_->send(mac, buf, n);
}

void PaintDistributor::sendRestoreToPeer(const uint8_t mac[6]) {
  if (!mesh_) return;
  uint8_t srcMac[6] = {0};
  mesh_->getMac(srcMac);

  uint8_t buf[lamp_protocol::RESTORE_FIXED_SIZE];
  size_t n = lamp_protocol::buildRestoreColors(
      buf, sizeof(buf), seqCounter_++,
      srcMac, mac,
      lamp_protocol::OverrideSurface::Base,
      lamp_protocol::OverrideSource::Wisp,
      kDefaultFadeDurationMs);
  if (!n) return;
  mesh_->send(mac, buf, n);
}

}  // namespace wisp
