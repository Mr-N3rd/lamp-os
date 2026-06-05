#include "WispZoneSelector.h"

#include <algorithm>

namespace wisp {

const char* zoneSourceName(ZoneSource s) {
  switch (s) {
    case ZoneSource::None:      return "none";
    case ZoneSource::FirstSeen: return "firstSeen";
    case ZoneSource::Nvs:       return "nvs";
    case ZoneSource::AppOp:     return "appOp";
  }
  return "?";
}

void ZoneSelector::observe(int zone) {
  auto it = std::find(observedZones_.begin(), observedZones_.end(), zone);
  if (it != observedZones_.end()) return;  // already known
  if (observedZones_.size() >= kMaxObservedZones) {
    observedZones_.erase(observedZones_.begin());  // oldest-out FIFO
  }
  observedZones_.push_back(zone);
}

bool ZoneSelector::latchFirstSeen(int zone) {
  if (source_ != ZoneSource::None || currentZone_ >= 0) return false;
  currentZone_ = zone;
  source_ = ZoneSource::FirstSeen;
  return true;
}

void ZoneSelector::setFromOp(int zone) {
  currentZone_ = zone;
  source_ = ZoneSource::AppOp;
}

void ZoneSelector::clearFromOp() {
  currentZone_ = -1;
  source_ = ZoneSource::None;
}

void ZoneSelector::setFromNvs(int zone) {
  currentZone_ = zone;
  source_ = ZoneSource::Nvs;
}

}  // namespace wisp
