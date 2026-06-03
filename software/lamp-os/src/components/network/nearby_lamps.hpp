#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdint>
#include <string>
#include <vector>

#include "util/color.hpp"

// Default prune age (milliseconds) shared between callers. NimBLE scan
// timer uses this when it runs onScanEnd; if a lamp hasn't been heard on
// EITHER transport in this window, it's dropped.
#ifndef LAMP_PRUNE_TIME_MS
#define LAMP_PRUNE_TIME_MS 120000
#endif

namespace lamp {

/**
 * @brief One nearby lamp — observed via BLE manufacturer-data scan,
 *        ESP-NOW HELLO, or both. Identity is `name` (user-set + capped
 *        12 chars). Per-transport `lastSeenVia*Ms` track when we last
 *        heard from this lamp on each channel so consumers can filter:
 *
 *        - SocialBehavior cares about lastSeenViaBleMs (short-range
 *          intimacy); it doesn't greet 200 m peers.
 *        - The grid view + remote-config flow gate on lastSeenViaEspNowMs
 *          since CHAR_REMOTE_OP forwarding only works for ESP-NOW peers.
 *        - The app's "Nearby Lamps" list shows everything.
 *
 *        `mac` is only populated once we've heard at least one HELLO —
 *        BLE adv doesn't carry a stable lamp-MAC anyway (the BT MAC isn't
 *        the WiFi STA MAC the protocol uses for addressing).
 */
struct NearbyLamp {
  std::string name;
  Color baseColor = Color();
  Color shadeColor = Color();
  uint8_t mac[6] = {0};
  bool hasMac = false;
  uint32_t lastSeenViaBleMs = 0;
  uint32_t lastSeenViaEspNowMs = 0;
  bool acknowledged = false;  // SocialBehavior's per-name greeting state
  // Packed semver (major<<16|minor<<8|patch) — extracted from MSG_HELLO.
  // Only set via the ESP-NOW path; BLE adv doesn't carry it. Zero until
  // we've heard at least one HELLO from this peer.
  uint32_t firmwareVersion = 0;
  // Most recent ESP-NOW RSSI (dBm) reported by the radio for any frame
  // from this peer. The cascade path sorts peers by lastRssi descending
  // so that the lamp closest to the originator fires first, producing an
  // outward spatial wave. -127 means "unknown" (no ESP-NOW frame seen
  // yet, or the rx_ctrl block was unavailable on the recv callback).
  int8_t lastRssi = -127;
};

/**
 * @brief Wisp presence cache populated from MSG_WISP_HELLO. v1 has a single
 *        global slot — the deferred multi-wisp finding lets the firmware
 *        treat the most recent hello as authoritative; C.6+ will extend to
 *        a per-mac map for room-isolated wisps. The brightness-floor check
 *        in ShowReceiver reads from this struct to decide whether an
 *        incoming brightness-override below kBrightnessOverrideMin is
 *        allowed (yes if a recent hello from the same MAC is on file).
 */
struct WispCache {
  bool present = false;
  uint8_t mac[6] = {0};
  uint32_t lastHelloMs = 0;
  uint32_t wispVersion = 0;
  uint8_t flags = 0;
  // +1 for trailing NUL so logging the string is safe; the on-wire slot
  // is 8 bytes opaque so we don't enforce ASCII.
  char paletteIdPrefix[9] = {0};
  char carriedFwChannel[9] = {0};
  uint32_t carriedFwVersion = 0;
};

/**
 * @brief Single source of truth for "lamps I can hear right now."
 *        Mutated from NimBLE scan callback (Core 0) and ESP-NOW HELLO
 *        recv (WiFi task); read from the loop task. SemaphoreHandle_t
 *        mutex serialises.
 */
class NearbyLamps {
 public:
  static constexpr size_t MAX_NEARBY = 32;

  NearbyLamps();

  void addOrUpdateFromBle(const std::string& name,
                          const Color& base, const Color& shade);
  void addOrUpdateFromEspNow(const std::string& name, const uint8_t mac[6],
                             const Color& base, const Color& shade,
                             uint32_t firmwareVersion = 0,
                             int8_t rssi = -127);

  // Drop entries whose most-recent sighting (max of the two transports)
  // is older than `maxAgeMs`.
  void prune(uint32_t maxAgeMs);

  // SocialBehavior: only entries whose lastSeenViaBleMs is within maxAgeMs.
  std::vector<NearbyLamp> getReachableViaBle(uint32_t maxAgeMs);

  // Cheap "is anyone in BLE range" check — early-exits on first hit, no
  // heap allocation. For callers that only care about presence (e.g.
  // mesh-state advertisement flag), not the full snapshot.
  bool hasAnyReachableViaBle(uint32_t maxAgeMs);

  // Grid view / remote-config: only entries whose lastSeenViaEspNowMs
  // is within maxAgeMs.
  std::vector<NearbyLamp> getReachableViaEspNow(uint32_t maxAgeMs);

  // Full snapshot — used by CHAR_NEARBY_LAMPS for the app's unified list.
  std::vector<NearbyLamp> getAll();

  // Mark a lamp as acknowledged. SocialBehavior calls this once per peer
  // so a re-trigger doesn't re-greet the same lamp until it prunes.
  void acknowledge(const std::string& name);

  // Wisp presence — populated by the MSG_WISP_HELLO drain in the loop
  // task. Single global slot in v1 (per the deferred multi-wisp finding).
  void cacheWispHello(const uint8_t mac[6],
                      uint32_t wispVersion,
                      uint8_t flags,
                      const char* paletteIdPrefix,  // 8 bytes; not NUL-terminated
                      const char* carriedFwChannel, // 8 bytes; not NUL-terminated
                      uint32_t carriedFwVersion);

  // Snapshot of the wisp cache. Returns a copy so the brightness-floor
  // check can compare against `mac` and `lastHelloMs` without holding
  // any lock past the read.
  WispCache getWispCache();

 private:
  std::vector<NearbyLamp> store_;
  SemaphoreHandle_t mutex_ = nullptr;
  WispCache wispCache_;

  // Caller must hold the mutex. Returns index of entry or store_.size()
  // if not found.
  size_t findIndexLocked(const std::string& name) const;
  // Caller must hold the mutex. Evicts the entry with the oldest combined
  // last-seen if the store is at capacity.
  void evictOldestIfFullLocked();
};

extern NearbyLamps nearbyLamps;  // single global instance, defined in .cpp

}  // namespace lamp
