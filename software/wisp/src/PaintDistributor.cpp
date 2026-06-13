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
  // Defensive: don't blast a peer with a zero-palette paint frame.
  // The Aurora-default boot path flips paintMode on BEFORE any
  // Aurora callback has populated currentPalette — without this
  // gate the 10 s backstop walks every peer with an all-zero
  // (black, fade=1500 ms) OVERRIDE_COLORS frame, which on the lamp
  // side fights every BLE base-color edit the operator makes.
  if (palette_->colors().empty()) return;

  // TupleSampler produces two RGBW samples per peer at decorrelated
  // positions on the palette (FNV-1a + golden-ratio salt). The intent
  // of the pair has always been one for base + one for shade — but the
  // pre-fix code shipped both as `numColors=2` to surface=Base, so
  // shade got nothing and base got a 2-stop gradient nobody asked for.
  // Split into two frames now: t[0] → Base, t[1] → Shade, single colour
  // per surface.
  ColorTuple t = sampleTupleForMac(*palette_, mac);
  uint8_t srcMac[6] = {0};
  mesh_->getMac(srcMac);

  // Frame 1 — Base, t[0]
  {
    uint8_t colorsRGBW[4] = {t.r[0], t.g[0], t.b[0], t.w[0]};
    uint8_t buf[lamp_protocol::OVERRIDE_COLORS_MAX_SIZE];
    size_t n = lamp_protocol::buildOverrideColors(
        buf, sizeof(buf), seqCounter_++,
        srcMac, mac,
        lamp_protocol::OverrideSurface::Base,
        lamp_protocol::OverrideSource::Wisp,
        kDefaultFadeDurationMs,
        colorsRGBW, /*numColors=*/1);
    if (n) mesh_->send(mac, buf, n);
  }
  // ESP-NOW needs a breather between back-to-back unicasts to the same
  // MAC. Without it the second send queues while the first frame's ACK
  // is still in-flight; the radio surfaces ESP_NOW_SEND_FAIL in the
  // first frame's trampoline callback and the lamp only sees the
  // second frame. Empirically jacko was seeing only the Shade frame
  // (sent second) and the Base trampoline logged FAIL. 10 ms is enough
  // headroom for the C6 radio's send → ACK → callback cycle at this
  // distance + RSSI on the local mesh; bump higher if FAIL lines come
  // back. Outside `kPerPeerPaceMs` (5 ms) but still under the human
  // perception threshold for the paint walk pace.
  delay(10);
  // Frame 2 — Shade, t[1]
  {
    uint8_t colorsRGBW[4] = {t.r[1], t.g[1], t.b[1], t.w[1]};
    uint8_t buf[lamp_protocol::OVERRIDE_COLORS_MAX_SIZE];
    size_t n = lamp_protocol::buildOverrideColors(
        buf, sizeof(buf), seqCounter_++,
        srcMac, mac,
        lamp_protocol::OverrideSurface::Shade,
        lamp_protocol::OverrideSource::Wisp,
        kDefaultFadeDurationMs,
        colorsRGBW, /*numColors=*/1);
    if (n) mesh_->send(mac, buf, n);
  }
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
