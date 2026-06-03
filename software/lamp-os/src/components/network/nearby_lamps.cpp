#include "nearby_lamps.hpp"

#include <cstring>

namespace lamp {

NearbyLamps nearbyLamps;  // global instance

NearbyLamps::NearbyLamps() {
  mutex_ = xSemaphoreCreateMutex();
}

size_t NearbyLamps::findIndexLocked(const std::string& name) const {
  for (size_t i = 0; i < store_.size(); i++) {
    if (store_[i].name == name) return i;
  }
  return store_.size();
}

void NearbyLamps::evictOldestIfFullLocked() {
  if (store_.size() < MAX_NEARBY) return;
  size_t oldestIdx = 0;
  uint32_t oldestMax = store_[0].lastSeenViaBleMs > store_[0].lastSeenViaEspNowMs
                           ? store_[0].lastSeenViaBleMs
                           : store_[0].lastSeenViaEspNowMs;
  for (size_t i = 1; i < store_.size(); i++) {
    uint32_t m = store_[i].lastSeenViaBleMs > store_[i].lastSeenViaEspNowMs
                     ? store_[i].lastSeenViaBleMs
                     : store_[i].lastSeenViaEspNowMs;
    if (m < oldestMax) { oldestMax = m; oldestIdx = i; }
  }
  if (oldestIdx != store_.size() - 1) {
    store_[oldestIdx] = store_.back();
  }
  store_.pop_back();
}

void NearbyLamps::addOrUpdateFromBle(const std::string& name,
                                     const Color& base, const Color& shade) {
  uint32_t now = millis();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  size_t idx = findIndexLocked(name);
  if (idx == store_.size()) {
    evictOldestIfFullLocked();
    NearbyLamp e;
    e.name = name;
    e.baseColor = base;
    e.shadeColor = shade;
    e.lastSeenViaBleMs = now;
    store_.push_back(e);
  } else {
    store_[idx].baseColor = base;
    store_[idx].shadeColor = shade;
    store_[idx].lastSeenViaBleMs = now;
  }
  xSemaphoreGive(mutex_);
}

void NearbyLamps::addOrUpdateFromEspNow(const std::string& name, const uint8_t mac[6],
                                        const Color& base, const Color& shade,
                                        uint32_t firmwareVersion) {
  uint32_t now = millis();
  // Bounded take: this runs on the ESP-NOW recv callback (WiFi task). A long
  // wait here stalls subsequent recv frames and the immediate
  // link_.broadcast() rebroadcast on the same task. 2 ms is well above
  // typical loop-task contention and well below any radio housekeeping
  // threshold. On timeout we silently drop the write — HELLO repeats every
  // 2 s, so the lamp is caught on the next beacon. Drop is preferable to a
  // recv-task stall.
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
#ifdef LAMP_DEBUG
    static uint32_t lastDropLogMs = 0;
    uint32_t logNow = millis();
    if (logNow - lastDropLogMs > 1000) {
      Serial.printf("[nearby] addOrUpdateFromEspNow: mutex contended, dropped (name=%s)\n",
                    name.c_str());
      lastDropLogMs = logNow;
    }
#endif
    return;
  }
  size_t idx = findIndexLocked(name);
  if (idx == store_.size()) {
    evictOldestIfFullLocked();
    NearbyLamp e;
    e.name = name;
    e.baseColor = base;
    e.shadeColor = shade;
    std::memcpy(e.mac, mac, 6);
    e.hasMac = true;
    e.lastSeenViaEspNowMs = now;
    e.firmwareVersion = firmwareVersion;
    store_.push_back(e);
  } else {
    store_[idx].baseColor = base;
    store_[idx].shadeColor = shade;
    std::memcpy(store_[idx].mac, mac, 6);
    store_[idx].hasMac = true;
    store_[idx].lastSeenViaEspNowMs = now;
    // Don't clobber a known version with a 0 — pre-HELLO BLE-only callers
    // pass the default; we only refresh once we actually got a HELLO.
    if (firmwareVersion != 0) store_[idx].firmwareVersion = firmwareVersion;
  }
  xSemaphoreGive(mutex_);
}

void NearbyLamps::prune(uint32_t maxAgeMs) {
  uint32_t now = millis();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (size_t i = 0; i < store_.size(); ) {
    uint32_t mostRecent = store_[i].lastSeenViaBleMs > store_[i].lastSeenViaEspNowMs
                              ? store_[i].lastSeenViaBleMs
                              : store_[i].lastSeenViaEspNowMs;
    if (mostRecent != 0 && (now - mostRecent) > maxAgeMs) {
      if (i != store_.size() - 1) store_[i] = store_.back();
      store_.pop_back();
      continue;
    }
    i++;
  }
  xSemaphoreGive(mutex_);
}

// Reader pattern: take lock just long enough to copy store_ into a stack
// snapshot, then release before any filtering/allocation work. Keeps the
// critical section bounded by the vector copy itself (no per-element
// predicate evaluation inside the lock) so ESP-NOW recv-side bounded takes
// don't time out waiting on a loop-task reader.
std::vector<NearbyLamp> NearbyLamps::getReachableViaBle(uint32_t maxAgeMs) {
  uint32_t now = millis();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  std::vector<NearbyLamp> snapshot = store_;
  xSemaphoreGive(mutex_);
  std::vector<NearbyLamp> out;
  out.reserve(snapshot.size());
  for (const auto& e : snapshot) {
    if (e.lastSeenViaBleMs != 0 && (now - e.lastSeenViaBleMs) <= maxAgeMs) {
      out.push_back(e);
    }
  }
  return out;
}

bool NearbyLamps::hasAnyReachableViaBle(uint32_t maxAgeMs) {
  uint32_t now = millis();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  std::vector<NearbyLamp> snapshot = store_;
  xSemaphoreGive(mutex_);
  for (const auto& e : snapshot) {
    if (e.lastSeenViaBleMs != 0 && (now - e.lastSeenViaBleMs) <= maxAgeMs) {
      return true;
    }
  }
  return false;
}

std::vector<NearbyLamp> NearbyLamps::getReachableViaEspNow(uint32_t maxAgeMs) {
  uint32_t now = millis();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  std::vector<NearbyLamp> snapshot = store_;
  xSemaphoreGive(mutex_);
  std::vector<NearbyLamp> out;
  out.reserve(snapshot.size());
  for (const auto& e : snapshot) {
    if (e.lastSeenViaEspNowMs != 0 && (now - e.lastSeenViaEspNowMs) <= maxAgeMs) {
      out.push_back(e);
    }
  }
  return out;
}

std::vector<NearbyLamp> NearbyLamps::getAll() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  std::vector<NearbyLamp> snapshot = store_;
  xSemaphoreGive(mutex_);
  return snapshot;
}

void NearbyLamps::acknowledge(const std::string& name) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  size_t idx = findIndexLocked(name);
  if (idx < store_.size()) store_[idx].acknowledged = true;
  xSemaphoreGive(mutex_);
}

void NearbyLamps::cacheWispHello(const uint8_t mac[6],
                                 uint32_t wispVersion,
                                 uint8_t flags,
                                 const char* paletteIdPrefix,
                                 const char* carriedFwChannel,
                                 uint32_t carriedFwVersion) {
  // Loop-task-only writer; the WiFi recv path memcpys into a typed pending
  // slot and the drain calls this on Core 1. portMAX_DELAY is fine here
  // because the only contended reader is also on Core 1 (we never hold
  // this mutex from Core 0). See the addOrUpdate paths above for the
  // bounded-take pattern when the writer is Core 0.
  xSemaphoreTake(mutex_, portMAX_DELAY);
  std::memcpy(wispCache_.mac, mac, 6);
  wispCache_.present = true;
  wispCache_.lastHelloMs = millis();
  wispCache_.wispVersion = wispVersion;
  wispCache_.flags = flags;
  // 8-byte fixed-width on-wire slots — copy as bytes, then ensure the
  // trailing NUL for safe logging. The caller's source pointers are NOT
  // NUL-terminated.
  std::memcpy(wispCache_.paletteIdPrefix, paletteIdPrefix, 8);
  wispCache_.paletteIdPrefix[8] = '\0';
  std::memcpy(wispCache_.carriedFwChannel, carriedFwChannel, 8);
  wispCache_.carriedFwChannel[8] = '\0';
  wispCache_.carriedFwVersion = carriedFwVersion;
  xSemaphoreGive(mutex_);
}

WispCache NearbyLamps::getWispCache() {
  // Bounded take: ShowReceiver's MSG_OVERRIDE_BRIGHTNESS branch on the
  // WiFi recv task (Core 0) reads this synchronously to decide whether
  // a below-floor brightness is wisp-paired. A long wait would stall the
  // recv task; on contention we return a "not present" snapshot — the
  // floor check then drops the suspect frame, which is the safe default.
  // Loop-task callers (the wispHello drain) take their own write side
  // with portMAX_DELAY; the only contention here is brief.
  WispCache snap;  // present=false by default
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
    return snap;
  }
  snap = wispCache_;
  xSemaphoreGive(mutex_);
  return snap;
}

}  // namespace lamp
