// PaintDistributor — fans out the active Aurora palette to every lamp in
// the mesh roster as MSG_OVERRIDE_COLORS unicast frames.
//
// Phase C.4. Triggers:
//   - On palette change (Aurora callback) → walk roster, paint everyone.
//   - On paint:on toggle (serial cmd, later: BLE MSG_WISP_OP) → same.
//   - Every kBackstopRefreshMs while paintMode is on → refresh, in case
//     a lamp missed a frame or just joined.
//   - On paint:off → broadcast MSG_RESTORE_COLORS, surface=Base, source=Wisp.
//
// Pacing: kPerPeerPaceMs (5 ms) between unicasts so ESP-NOW's send queue
// doesn't back up with a single-shot fan-out of 12+ frames. The pacing
// queue is just a "next peer index" cursor; tick() drains one peer per
// pace interval.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wisp {

class CurrentPalette;
class LampInventory;
struct InventoryEntry;
class MeshLink;

class PaintDistributor {
 public:
  void begin(LampInventory* inventory, MeshLink* mesh,
             CurrentPalette* palette);

  // Toggle paint mode. paint:off broadcasts MSG_RESTORE_COLORS to every
  // currently-known peer. paint:on kicks off a fresh fan-out walk.
  void setPaintMode(bool on);
  bool paintMode() const { return paintMode_; }

  // Aurora palette callback hook. Called from the main loop task when a new
  // palette resolves. Snapshots the roster and starts the paced fan-out.
  void onPaletteChanged();

  // Called every loop iteration. Drains the paced send queue and triggers
  // the backstop refresh every kBackstopRefreshMs while paintMode is on.
  void tick(uint32_t nowMs);

 private:
  enum class Mode : uint8_t {
    Idle = 0,
    Paint,     // walking the roster sending MSG_OVERRIDE_COLORS
    Restore,   // walking the roster sending MSG_RESTORE_COLORS
  };

  // Start a fresh walk of `mode`. Snapshots the roster into walkMacs_ so
  // the walk doesn't bobble if a new HELLO mutates LampInventory mid-flight.
  void beginWalk(Mode mode);

  void sendPaintToPeer(const uint8_t mac[6]);
  void sendRestoreToPeer(const uint8_t mac[6]);

  LampInventory* inventory_ = nullptr;
  MeshLink* mesh_ = nullptr;
  CurrentPalette* palette_ = nullptr;
  bool paintMode_ = false;
  uint32_t lastBackstopMs_ = 0;
  uint32_t lastSendMs_ = 0;
  uint16_t seqCounter_ = 0;

  // Active walk state.
  Mode walkMode_ = Mode::Idle;
  static constexpr size_t kMaxWalkPeers = 32;  // matches LampInventory::MAX_LAMPS
  uint8_t walkMacs_[kMaxWalkPeers][6];
  size_t walkCount_ = 0;
  size_t walkIdx_ = 0;

  static constexpr uint32_t kPerPeerPaceMs       = 5;
  static constexpr uint32_t kBackstopRefreshMs   = 10000;
  static constexpr uint16_t kDefaultFadeDurationMs = 1500;
};

}  // namespace wisp
