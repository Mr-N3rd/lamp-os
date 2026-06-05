// WispZoneSelector — process-local zone-selection state for the wisp.
//
// Extracted from main.cpp in Phase D Task 2 because StatusBeacon now needs to
// read currentZone / source / observedZones to emit wispStatus broadcasts.
// Only `currentZone` is persistent (and that lives in WispConfig); everything
// here is RAM.
//
// THREADING: all access must occur on the loop task. observe() may be invoked
// from auroraClient.loop() OR drainPendingWispOp — both run on loop. Never
// wire a recv-task call site without adding a portMUX.
//
// The `ZoneSource` discriminator tells the app pane where the current
// selection came from. The string form is camelCase to match the
// `char:"wispOp"` JSON naming convention on the wire.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wisp {

enum class ZoneSource : uint8_t { None, FirstSeen, Nvs, AppOp };

// camelCase wire form. The serial dump and JSON broadcast both use this so
// there's one mapping to chase.
const char* zoneSourceName(ZoneSource s);

// 16 matches Aurora's per-notification states cap; oldest-eviction FIFO
// keeps the set bounded without leaking memory if a noisy Aurora keeps
// rotating zone ids. The wispStatus JSON also caps at 16 entries, so this
// upper bound is the single source of truth for the budget.
constexpr size_t kMaxObservedZones = 16;

class ZoneSelector {
 public:
  int currentZone() const { return currentZone_; }
  ZoneSource source() const { return source_; }
  const std::vector<int>& observed() const { return observedZones_; }

  void observe(int zone);

  // Returns true if the first-seen latch actually changed state (caller can
  // log accordingly). No-op when a Nvs/AppOp selection is already in force.
  bool latchFirstSeen(int zone);

  void setFromOp(int zone);
  void clearFromOp();

  // Seed from NVS at boot. Used by main.cpp before the recv path is alive.
  void setFromNvs(int zone);

 private:
  int currentZone_ = -1;
  ZoneSource source_ = ZoneSource::None;
  std::vector<int> observedZones_;  // FIFO, uniqued on insert
};

}  // namespace wisp
